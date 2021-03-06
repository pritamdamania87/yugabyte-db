// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//

#ifndef YB_CONSENSUS_CONSENSUS_TEST_UTIL_H_
#define YB_CONSENSUS_CONSENSUS_TEST_UTIL_H_

#include <gmock/gmock.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <boost/bind.hpp>

#include "yb/common/hybrid_time.h"
#include "yb/common/wire_protocol.h"
#include "yb/consensus/consensus.h"
#include "yb/consensus/consensus_peers.h"
#include "yb/consensus/consensus_queue.h"
#include "yb/consensus/log.h"
#include "yb/consensus/raft_consensus.h"
#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/server/clock.h"
#include "yb/util/countdown_latch.h"
#include "yb/util/locks.h"
#include "yb/util/test_macros.h"
#include "yb/util/threadpool.h"

#define TOKENPASTE(x, y) x ## y
#define TOKENPASTE2(x, y) TOKENPASTE(x, y)

#define ASSERT_OPID_EQ(left, right) \
  OpId TOKENPASTE2(_left, __LINE__) = (left); \
  OpId TOKENPASTE2(_right, __LINE__) = (right); \
  if (!consensus::OpIdEquals(TOKENPASTE2(_left, __LINE__), TOKENPASTE2(_right, __LINE__))) \
    FAIL() << "Expected: " << TOKENPASTE2(_right, __LINE__).ShortDebugString() << "\n" \
           << "Value: " << TOKENPASTE2(_left, __LINE__).ShortDebugString() << "\n"

namespace yb {
namespace consensus {

using log::Log;
using strings::Substitute;

inline ReplicateMsgPtr CreateDummyReplicate(int term,
                                            int index,
                                            const HybridTime& hybrid_time,
                                            int payload_size) {
  auto msg = std::make_shared<ReplicateMsg>();
  OpId* id = msg->mutable_id();
  id->set_term(term);
  id->set_index(index);

  msg->set_op_type(NO_OP);
  msg->mutable_noop_request()->mutable_payload_for_tests()->resize(payload_size);
  msg->set_hybrid_time(hybrid_time.ToUint64());
  return msg;
}

// Returns RaftPeerPB with given UUID and obviously-fake hostname / port combo.
RaftPeerPB FakeRaftPeerPB(const std::string& uuid) {
  RaftPeerPB peer_pb;
  peer_pb.set_permanent_uuid(uuid);
  peer_pb.mutable_last_known_addr()->set_host(Substitute("$0-fake-hostname", CURRENT_TEST_NAME()));
  peer_pb.mutable_last_known_addr()->set_port(0);
  return peer_pb;
}

// Appends 'count' messages to 'queue' with different terms and indexes.
//
// An operation will only be considered done (TestOperationStatus::IsDone()
// will become true) once at least 'n_majority' peers have called
// TestOperationStatus::AckPeer().
static inline void AppendReplicateMessagesToQueue(
    PeerMessageQueue* queue,
    const scoped_refptr<server::Clock>& clock,
    int first,
    int count,
    int payload_size = 0) {

  for (int i = first; i < first + count; i++) {
    int term = i / 7;
    int index = i;
    CHECK_OK(queue->AppendOperation(CreateDummyReplicate(term, index, clock->Now(), payload_size)));
  }
}

// Builds a configuration of 'num' voters.
RaftConfigPB BuildRaftConfigPBForTests(int num) {
  RaftConfigPB raft_config;
  for (int i = 0; i < num; i++) {
    RaftPeerPB* peer_pb = raft_config.add_peers();
    peer_pb->set_member_type(RaftPeerPB::VOTER);
    peer_pb->set_permanent_uuid(Substitute("peer-$0", i));
    HostPortPB* hp = peer_pb->mutable_last_known_addr();
    hp->set_host(Substitute("peer-$0.fake-domain-for-tests", i));
    hp->set_port(0);
  }
  return raft_config;
}

// Abstract base class to build PeerProxy implementations on top of for testing.
// Provides a single-threaded pool to run callbacks in and callback
// registration/running, along with an enum to identify the supported methods.
class TestPeerProxy : public PeerProxy {
 public:
  // Which PeerProxy method to invoke.
  enum Method {
    kUpdate,
    kRequestVote,
  };

  explicit TestPeerProxy(ThreadPool* pool) : pool_(pool) {}

 protected:
  // Register the RPC callback in order to call later.
  // We currently only support one request of each method being in flight at a time.
  virtual void RegisterCallback(Method method, const rpc::ResponseCallback& callback) {
    std::lock_guard<simple_spinlock> lock(lock_);
    InsertOrDie(&callbacks_, method, callback);
  }

  // Answer the peer.
  virtual void Respond(Method method) {
    rpc::ResponseCallback callback;
    {
      std::lock_guard<simple_spinlock> lock(lock_);
      callback = FindOrDie(callbacks_, method);
      CHECK_EQ(1, callbacks_.erase(method));
      // Drop the lock before submitting to the pool, since the callback itself may
      // destroy this instance.
    }
    CHECK_OK(pool_->SubmitFunc(callback));
  }

  virtual void RegisterCallbackAndRespond(Method method, const rpc::ResponseCallback& callback) {
    RegisterCallback(method, callback);
    Respond(method);
  }

  mutable simple_spinlock lock_;
  ThreadPool* pool_;
  std::map<Method, rpc::ResponseCallback> callbacks_; // Protected by lock_.
};

template <typename ProxyType>
class DelayablePeerProxy : public TestPeerProxy {
 public:
  // Add delayability of RPC responses to the delegated impl.
  // This class takes ownership of 'proxy'.
  explicit DelayablePeerProxy(ThreadPool* pool, ProxyType* proxy)
    : TestPeerProxy(pool),
      proxy_(CHECK_NOTNULL(proxy)),
      delay_response_(false),
      latch_(1) {
  }

  // Delay the answer to the next response to this remote
  // peer. The response callback will only be called on Respond().
  virtual void DelayResponse() {
    std::lock_guard<simple_spinlock> l(lock_);
    delay_response_ = true;
    latch_.Reset(1); // Reset for the next time.
  }

  virtual void RespondUnlessDelayed(Method method) {
    {
      std::lock_guard<simple_spinlock> l(lock_);
      if (delay_response_) {
        latch_.CountDown();
        delay_response_ = false;
        return;
      }
    }
    TestPeerProxy::Respond(method);
  }

  virtual void Respond(Method method) override {
    latch_.Wait();   // Wait until strictly after peer would have responded.
    return TestPeerProxy::Respond(method);
  }

  virtual void UpdateAsync(const ConsensusRequestPB* request,
                           ConsensusResponsePB* response,
                           rpc::RpcController* controller,
                           const rpc::ResponseCallback& callback) override {
    RegisterCallback(kUpdate, callback);
    return proxy_->UpdateAsync(
        request, response, controller,
        std::bind(&DelayablePeerProxy::RespondUnlessDelayed, this, kUpdate));
  }

  virtual void RequestConsensusVoteAsync(const VoteRequestPB* request,
                                         VoteResponsePB* response,
                                         rpc::RpcController* controller,
                                         const rpc::ResponseCallback& callback) override {
    RegisterCallback(kRequestVote, callback);
    return proxy_->RequestConsensusVoteAsync(
        request, response, controller,
        std::bind(&DelayablePeerProxy::RespondUnlessDelayed, this, kRequestVote));
  }

  ProxyType* proxy() const {
    return proxy_.get();
  }

 protected:
  gscoped_ptr<ProxyType> const proxy_;
  bool delay_response_; // Protected by lock_.
  CountDownLatch latch_;
};

// Allows complete mocking of a peer's responses.
// You set the response, it will respond with that.
class MockedPeerProxy : public TestPeerProxy {
 public:
  explicit MockedPeerProxy(ThreadPool* pool)
  : TestPeerProxy(pool),
    update_count_(0) {
  }

  virtual void set_update_response(const ConsensusResponsePB& update_response) {
    CHECK(update_response.IsInitialized()) << update_response.ShortDebugString();
    {
      std::lock_guard<simple_spinlock> l(lock_);
      update_response_ = update_response;
    }
  }

  virtual void set_vote_response(const VoteResponsePB& vote_response) {
    {
      std::lock_guard<simple_spinlock> l(lock_);
      vote_response_ = vote_response;
    }
  }

  virtual void UpdateAsync(const ConsensusRequestPB* request,
                           ConsensusResponsePB* response,
                           rpc::RpcController* controller,
                           const rpc::ResponseCallback& callback) override {
    {
      std::lock_guard<simple_spinlock> l(lock_);
      update_count_++;
      *response = update_response_;
    }
    return RegisterCallbackAndRespond(kUpdate, callback);
  }

  virtual void RequestConsensusVoteAsync(const VoteRequestPB* request,
                                         VoteResponsePB* response,
                                         rpc::RpcController* controller,
                                         const rpc::ResponseCallback& callback) override {
    *response = vote_response_;
    return RegisterCallbackAndRespond(kRequestVote, callback);
  }

  // Return the number of times that UpdateAsync() has been called.
  int update_count() const {
    std::lock_guard<simple_spinlock> l(lock_);
    return update_count_;
  }

 protected:
  int update_count_;

  ConsensusResponsePB update_response_;
  VoteResponsePB vote_response_;
};

// Allows to test peers by emulating a noop remote endpoint that just replies
// that the messages were received/replicated/committed.
class NoOpTestPeerProxy : public TestPeerProxy {
 public:

  explicit NoOpTestPeerProxy(ThreadPool* pool, const consensus::RaftPeerPB& peer_pb)
    : TestPeerProxy(pool), peer_pb_(peer_pb) {
    last_received_.CopyFrom(MinimumOpId());
  }

  virtual void UpdateAsync(const ConsensusRequestPB* request,
                           ConsensusResponsePB* response,
                           rpc::RpcController* controller,
                           const rpc::ResponseCallback& callback) override {

    response->Clear();
    {
      std::lock_guard<simple_spinlock> lock(lock_);
      if (OpIdLessThan(last_received_, request->preceding_id())) {
        ConsensusErrorPB* error = response->mutable_status()->mutable_error();
        error->set_code(ConsensusErrorPB::PRECEDING_ENTRY_DIDNT_MATCH);
        StatusToPB(STATUS(IllegalState, ""), error->mutable_status());
      } else if (request->ops_size() > 0) {
        last_received_.CopyFrom(request->ops(request->ops_size() - 1).id());
      }

      response->set_responder_uuid(peer_pb_.permanent_uuid());
      response->set_responder_term(request->caller_term());
      response->mutable_status()->mutable_last_received()->CopyFrom(last_received_);
      response->mutable_status()->mutable_last_received_current_leader()->CopyFrom(last_received_);
      // We set the last committed index to be the same index as the last received. While
      // this is unlikely to happen in a real situation, its not technically incorrect and
      // avoids having to come up with some other index that it still correct.
      response->mutable_status()->set_last_committed_idx(last_received_.index());
    }
    return RegisterCallbackAndRespond(kUpdate, callback);
  }

  virtual void RequestConsensusVoteAsync(const VoteRequestPB* request,
                                         VoteResponsePB* response,
                                         rpc::RpcController* controller,
                                         const rpc::ResponseCallback& callback) override {
    {
      std::lock_guard<simple_spinlock> lock(lock_);
      response->set_responder_uuid(peer_pb_.permanent_uuid());
      response->set_responder_term(request->candidate_term());
      response->set_vote_granted(true);
    }
    return RegisterCallbackAndRespond(kRequestVote, callback);
  }

  const OpId& last_received() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return last_received_;
  }

 private:
  const consensus::RaftPeerPB peer_pb_;
  ConsensusStatusPB last_status_; // Protected by lock_.
  OpId last_received_;            // Protected by lock_.
};

class NoOpTestPeerProxyFactory : public PeerProxyFactory {
 public:
  NoOpTestPeerProxyFactory() {
    CHECK_OK(ThreadPoolBuilder("test-peer-pool").set_max_threads(3).Build(&pool_));
  }

  virtual CHECKED_STATUS NewProxy(const consensus::RaftPeerPB& peer_pb,
                          gscoped_ptr<PeerProxy>* proxy) override {
    proxy->reset(new NoOpTestPeerProxy(pool_.get(), peer_pb));
    return Status::OK();
  }

  gscoped_ptr<ThreadPool> pool_;
};

typedef std::unordered_map<std::string, scoped_refptr<RaftConsensus> > TestPeerMap;

// Thread-safe manager for list of peers being used in tests.
class TestPeerMapManager {
 public:
  explicit TestPeerMapManager(const RaftConfigPB& config) : config_(config) {}

  void AddPeer(const std::string& peer_uuid, const scoped_refptr<RaftConsensus>& peer) {
    std::lock_guard<simple_spinlock> lock(lock_);
    InsertOrDie(&peers_, peer_uuid, peer);
  }

  CHECKED_STATUS GetPeerByIdx(int idx, scoped_refptr<RaftConsensus>* peer_out) const {
    CHECK_LT(idx, config_.peers_size());
    return GetPeerByUuid(config_.peers(idx).permanent_uuid(), peer_out);
  }

  CHECKED_STATUS GetPeerByUuid(const std::string& peer_uuid,
                       scoped_refptr<RaftConsensus>* peer_out) const {
    std::lock_guard<simple_spinlock> lock(lock_);
    if (!FindCopy(peers_, peer_uuid, peer_out)) {
      return STATUS(NotFound, "Other consensus instance was destroyed");
    }
    return Status::OK();
  }

  void RemovePeer(const std::string& peer_uuid) {
    std::lock_guard<simple_spinlock> lock(lock_);
    peers_.erase(peer_uuid);
  }

  TestPeerMap GetPeerMapCopy() const {
    std::lock_guard<simple_spinlock> lock(lock_);
    return peers_;
  }

  void Clear() {
    // We create a copy of the peers before we clear 'peers_' so that there's
    // still a reference to each peer. If we reduce the reference count to 0 under
    // the lock we might get a deadlock as on shutdown consensus indirectly
    // destroys the test proxies which in turn reach into this class.
    TestPeerMap copy = peers_;
    {
      std::lock_guard<simple_spinlock> lock(lock_);
      peers_.clear();
    }

  }

 private:
  const RaftConfigPB config_;
  TestPeerMap peers_;
  mutable simple_spinlock lock_;
};


// Allows to test remote peers by emulating an RPC.
// Both the "remote" peer's RPC call and the caller peer's response are executed
// asynchronously in a ThreadPool.
class LocalTestPeerProxy : public TestPeerProxy {
 public:
  LocalTestPeerProxy(std::string peer_uuid, ThreadPool* pool,
                     TestPeerMapManager* peers)
      : TestPeerProxy(pool),
        peer_uuid_(std::move(peer_uuid)),
        peers_(peers),
        miss_comm_(false) {}

  virtual void UpdateAsync(const ConsensusRequestPB* request,
                           ConsensusResponsePB* response,
                           rpc::RpcController* controller,
                           const rpc::ResponseCallback& callback) override {
    RegisterCallback(kUpdate, callback);
    CHECK_OK(pool_->SubmitFunc(
        std::bind(&LocalTestPeerProxy::SendUpdateRequest, this, request, response)));
  }

  virtual void RequestConsensusVoteAsync(const VoteRequestPB* request,
                                         VoteResponsePB* response,
                                         rpc::RpcController* controller,
                                         const rpc::ResponseCallback& callback) override {
    RegisterCallback(kRequestVote, callback);
    CHECK_OK(pool_->SubmitFunc(
        std::bind(&LocalTestPeerProxy::SendVoteRequest, this, request, response)));
  }

  template<class Response>
  void SetResponseError(const Status& status, Response* response) {
    tserver::TabletServerErrorPB* error = response->mutable_error();
    error->set_code(tserver::TabletServerErrorPB::UNKNOWN_ERROR);
    StatusToPB(status, error->mutable_status());
  }

  template<class Request, class Response>
  void RespondOrMissResponse(Request* request,
                             const Response& response_temp,
                             Response* final_response,
                             Method method) {

    bool miss_comm_copy;
    {
      std::lock_guard<simple_spinlock> lock(lock_);
      miss_comm_copy = miss_comm_;
      miss_comm_ = false;
    }
    if (PREDICT_FALSE(miss_comm_copy)) {
      VLOG(2) << this << ": injecting fault on " << request->ShortDebugString();
      SetResponseError(STATUS(IOError, "Artificial error caused by communication "
          "failure injection."), final_response);
    } else {
      final_response->CopyFrom(response_temp);
    }
    Respond(method);
  }

  void SendUpdateRequest(const ConsensusRequestPB* request,
                         ConsensusResponsePB* response) {
    // Copy the request and the response for the other peer so that ownership
    // remains as close to the dist. impl. as possible.
    ConsensusRequestPB other_peer_req;
    other_peer_req.CopyFrom(*request);

    // Give the other peer a clean response object to write to.
    ConsensusResponsePB other_peer_resp;
    scoped_refptr<RaftConsensus> peer;
    Status s = peers_->GetPeerByUuid(peer_uuid_, &peer);

    if (s.ok()) {
      s = peer->Update(&other_peer_req, &other_peer_resp);
      if (s.ok() && !other_peer_resp.has_error()) {
        CHECK(other_peer_resp.has_status());
        CHECK(other_peer_resp.status().IsInitialized());
      }
    }
    if (!s.ok()) {
      LOG(WARNING) << "Could not Update replica with request: "
                   << other_peer_req.ShortDebugString()
                   << " Status: " << s.ToString();
      SetResponseError(s, &other_peer_resp);
    }

    response->CopyFrom(other_peer_resp);
    RespondOrMissResponse(request, other_peer_resp, response, kUpdate);
  }



  void SendVoteRequest(const VoteRequestPB* request,
                       VoteResponsePB* response) {

    // Copy the request and the response for the other peer so that ownership
    // remains as close to the dist. impl. as possible.
    VoteRequestPB other_peer_req;
    other_peer_req.CopyFrom(*request);
    VoteResponsePB other_peer_resp;
    other_peer_resp.CopyFrom(*response);

    scoped_refptr<RaftConsensus> peer;
    Status s = peers_->GetPeerByUuid(peer_uuid_, &peer);

    if (s.ok()) {
      s = peer->RequestVote(&other_peer_req, &other_peer_resp);
    }
    if (!s.ok()) {
      LOG(WARNING) << "Could not RequestVote from replica with request: "
                   << other_peer_req.ShortDebugString()
                   << " Status: " << s.ToString();
      SetResponseError(s, &other_peer_resp);
    }

    response->CopyFrom(other_peer_resp);
    RespondOrMissResponse(request, other_peer_resp, response, kRequestVote);
  }

  void InjectCommFaultLeaderSide() {
    VLOG(2) << this << ": injecting fault next time";
    std::lock_guard<simple_spinlock> lock(lock_);
    miss_comm_ = true;
  }

  const std::string& GetTarget() const {
    return peer_uuid_;
  }

 private:
  const std::string peer_uuid_;
  TestPeerMapManager* const peers_;
  bool miss_comm_;
};

class LocalTestPeerProxyFactory : public PeerProxyFactory {
 public:
  explicit LocalTestPeerProxyFactory(TestPeerMapManager* peers)
    : peers_(peers) {
    CHECK_OK(ThreadPoolBuilder("test-peer-pool").set_max_threads(3).Build(&pool_));
  }

  virtual CHECKED_STATUS NewProxy(const consensus::RaftPeerPB& peer_pb,
                          gscoped_ptr<PeerProxy>* proxy) override {
    LocalTestPeerProxy* new_proxy = new LocalTestPeerProxy(peer_pb.permanent_uuid(),
                                                           pool_.get(),
                                                           peers_);
    proxy->reset(new_proxy);
    proxies_.push_back(new_proxy);
    return Status::OK();
  }

  virtual const vector<LocalTestPeerProxy*>& GetProxies() {
    return proxies_;
  }

 private:
  gscoped_ptr<ThreadPool> pool_;
  TestPeerMapManager* const peers_;
    // NOTE: There is no need to delete this on the dctor because proxies are externally managed
  vector<LocalTestPeerProxy*> proxies_;
};

// A simple implementation of the transaction driver.
// This is usually implemented by OperationDriver but here we
// keep the implementation to the minimally required to have consensus
// work.
class TestDriver {
 public:
  TestDriver(ThreadPool* pool, Log* log, const scoped_refptr<ConsensusRound>& round)
      : round_(round),
        pool_(pool),
        log_(log) {
  }

  void SetRound(const scoped_refptr<ConsensusRound>& round) {
    round_ = round;
  }

  // Does nothing but enqueue the Apply
  void ReplicationFinished(const Status& status) {
    if (status.IsAborted()) {
      Cleanup();
      return;
    }
    CHECK_OK(status);
    CHECK_OK(pool_->SubmitFunc(std::bind(&TestDriver::Apply, this)));
  }

  // Called in all modes to delete the transaction and, transitively, the consensus
  // round.
  void Cleanup() {
    delete this;
  }

  scoped_refptr<ConsensusRound> round_;

 private:
  // The commit message has the exact same type of the replicate message, but
  // no content.
  void Apply() {
    gscoped_ptr<CommitMsg> msg(new CommitMsg);
    msg->set_op_type(round_->replicate_msg()->op_type());
    msg->mutable_commited_op_id()->CopyFrom(round_->id());
    CHECK_OK(log_->AsyncAppendCommit(msg.Pass(),
                                     Bind(&TestDriver::CommitCallback, Unretained(this))));
  }

  void CommitCallback(const Status& s) {
    CHECK_OK(s);
    Cleanup();
  }

  ThreadPool* pool_;
  Log* log_;
};

// Fake ReplicaOperationFactory that allows for instantiating and unit
// testing RaftConsensusState. Does not actually support running transactions.
class MockOperationFactory : public ReplicaOperationFactory {
 public:
  virtual CHECKED_STATUS StartReplicaOperation(const scoped_refptr<ConsensusRound>& round)
      override {
    return StartReplicaOperationMock(round.get());
  }
  MOCK_METHOD1(StartReplicaOperationMock, Status(ConsensusRound* round));
};

// A transaction factory for tests, usually this is implemented by TabletPeer.
class TestOperationFactory : public ReplicaOperationFactory {
 public:
  explicit TestOperationFactory(Log* log) : log_(log) {

    CHECK_OK(ThreadPoolBuilder("test-operation-factory").set_max_threads(1).Build(&pool_));
  }

  void SetConsensus(Consensus* consensus) {
    consensus_ = consensus;
  }

  CHECKED_STATUS StartReplicaOperation(const scoped_refptr<ConsensusRound>& round) override {
    auto txn = new TestDriver(pool_.get(), log_, round);
    txn->round_->SetConsensusReplicatedCallback(Bind(&TestDriver::ReplicationFinished,
                                                     Unretained(txn)));
    return Status::OK();
  }

  void ReplicateAsync(ConsensusRound* round) {
    CHECK_OK(consensus_->Replicate(round));
  }

  void WaitDone() {
    pool_->Wait();
  }

  void ShutDown() {
    WaitDone();
    pool_->Shutdown();
  }

  ~TestOperationFactory() {
    ShutDown();
  }

 private:
  gscoped_ptr<ThreadPool> pool_;
  Consensus* consensus_ = nullptr;
  Log* log_;
};

// Consensus fault hooks impl. that simply counts the number of calls to
// each method.
// Allows passing another hook instance so that we can use both.
// If non-null, the passed hook instance will be called first for all methods.
class CounterHooks : public Consensus::ConsensusFaultHooks {
 public:
  explicit CounterHooks(
      std::shared_ptr<Consensus::ConsensusFaultHooks> current_hook)
      : current_hook_(std::move(current_hook)),
        pre_start_calls_(0),
        post_start_calls_(0),
        pre_config_change_calls_(0),
        post_config_change_calls_(0),
        pre_replicate_calls_(0),
        post_replicate_calls_(0),
        pre_update_calls_(0),
        post_update_calls_(0),
        pre_shutdown_calls_(0),
        post_shutdown_calls_(0) {}

  virtual CHECKED_STATUS PreStart() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PreStart());
    std::lock_guard<simple_spinlock> lock(lock_);
    pre_start_calls_++;
    return Status::OK();
  }

  virtual CHECKED_STATUS PostStart() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PostStart());
    std::lock_guard<simple_spinlock> lock(lock_);
    post_start_calls_++;
    return Status::OK();
  }

  virtual CHECKED_STATUS PreConfigChange() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PreConfigChange());
    std::lock_guard<simple_spinlock> lock(lock_);
    pre_config_change_calls_++;
    return Status::OK();
  }

  virtual CHECKED_STATUS PostConfigChange() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PostConfigChange());
    std::lock_guard<simple_spinlock> lock(lock_);
    post_config_change_calls_++;
    return Status::OK();
  }

  virtual CHECKED_STATUS PreReplicate() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PreReplicate());
    std::lock_guard<simple_spinlock> lock(lock_);
    pre_replicate_calls_++;
    return Status::OK();
  }

  virtual CHECKED_STATUS PostReplicate() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PostReplicate());
    std::lock_guard<simple_spinlock> lock(lock_);
    post_replicate_calls_++;
    return Status::OK();
  }

  virtual CHECKED_STATUS PreUpdate() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PreUpdate());
    std::lock_guard<simple_spinlock> lock(lock_);
    pre_update_calls_++;
    return Status::OK();
  }

  virtual CHECKED_STATUS PostUpdate() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PostUpdate());
    std::lock_guard<simple_spinlock> lock(lock_);
    post_update_calls_++;
    return Status::OK();
  }

  virtual CHECKED_STATUS PreShutdown() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PreShutdown());
    std::lock_guard<simple_spinlock> lock(lock_);
    pre_shutdown_calls_++;
    return Status::OK();
  }

  virtual CHECKED_STATUS PostShutdown() override {
    if (current_hook_.get()) RETURN_NOT_OK(current_hook_->PostShutdown());
    std::lock_guard<simple_spinlock> lock(lock_);
    post_shutdown_calls_++;
    return Status::OK();
  }

  int num_pre_start_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return pre_start_calls_;
  }

  int num_post_start_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return post_start_calls_;
  }

  int num_pre_config_change_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return pre_config_change_calls_;
  }

  int num_post_config_change_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return post_config_change_calls_;
  }

  int num_pre_replicate_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return pre_replicate_calls_;
  }

  int num_post_replicate_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return post_replicate_calls_;
  }

  int num_pre_update_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return pre_update_calls_;
  }

  int num_post_update_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return post_update_calls_;
  }

  int num_pre_shutdown_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return pre_shutdown_calls_;
  }

  int num_post_shutdown_calls() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return post_shutdown_calls_;
  }

 private:
  std::shared_ptr<Consensus::ConsensusFaultHooks> current_hook_;
  int pre_start_calls_;
  int post_start_calls_;
  int pre_config_change_calls_;
  int post_config_change_calls_;
  int pre_replicate_calls_;
  int post_replicate_calls_;
  int pre_update_calls_;
  int post_update_calls_;
  int pre_shutdown_calls_;
  int post_shutdown_calls_;

  // Lock that protects updates to the counters.
  mutable simple_spinlock lock_;
};

class TestRaftConsensusQueueIface : public PeerMessageQueueObserver {
 public:
  bool IsMajorityReplicated(int64_t index) {
    std::lock_guard<simple_spinlock> lock(lock_);
    return index <= majority_replicated_index_;
  }

 protected:
  virtual void UpdateMajorityReplicated(const OpId& majority_replicated,
                                        MonoTime majority_replicated_leader_lease_expiration,
                                        OpId* committed_index) override {
    std::lock_guard<simple_spinlock> lock(lock_);
    majority_replicated_index_ = majority_replicated.index();
    committed_index->CopyFrom(majority_replicated);
  }
  virtual void NotifyTermChange(int64_t term) override {}
  virtual void NotifyFailedFollower(const std::string& uuid,
                                    int64_t term,
                                    const std::string& reason) override {}

 private:
  mutable simple_spinlock lock_;
  int64_t majority_replicated_index_;
};

}  // namespace consensus
}  // namespace yb

#endif /* YB_CONSENSUS_CONSENSUS_TEST_UTIL_H_ */
