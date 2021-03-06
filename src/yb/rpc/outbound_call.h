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
#ifndef YB_RPC_OUTBOUND_CALL_H_
#define YB_RPC_OUTBOUND_CALL_H_

#include <deque>
#include <string>
#include <vector>

#include <glog/logging.h>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/macros.h"
#include "yb/rpc/rpc_fwd.h"
#include "yb/rpc/constants.h"
#include "yb/rpc/remote_method.h"
#include "yb/rpc/response_callback.h"
#include "yb/rpc/rpc_call.h"
#include "yb/rpc/rpc_header.pb.h"
#include "yb/rpc/service_if.h"
#include "yb/util/locks.h"
#include "yb/util/monotime.h"
#include "yb/util/net/sockaddr.h"
#include "yb/util/ref_cnt_buffer.h"
#include "yb/util/slice.h"
#include "yb/util/status.h"
#include "yb/util/trace.h"

namespace google {
namespace protobuf {
class Message;
}  // namespace protobuf
}  // namespace google

namespace yb {
namespace rpc {

class CallResponse;
class Connection;
class DumpRunningRpcsRequestPB;
class YBInboundTransfer;
class RpcCallInProgressPB;
class RpcController;

// Client-side user credentials, such as a user's username & password.
// In the future, we will add Kerberos credentials.
//
// TODO(mpercy): this is actually used server side too -- should
// we instead introduce a RemoteUser class or something?
class UserCredentials {
 public:
  UserCredentials();

  // Effective user, in cases where impersonation is supported.
  // If impersonation is not supported, this should be left empty.
  bool has_effective_user() const;
  void set_effective_user(const std::string& eff_user);
  const std::string& effective_user() const { return eff_user_; }

  // Real user.
  bool has_real_user() const;
  void set_real_user(const std::string& real_user);
  const std::string& real_user() const { return real_user_; }

  // The real user's password.
  bool has_password() const;
  void set_password(const std::string& password);
  const std::string& password() const { return password_; }

  // Copy state from another object to this one.
  void CopyFrom(const UserCredentials& other);

  // Returns a string representation of the object, not including the password field.
  std::string ToString() const;

  std::size_t HashCode() const;
  bool Equals(const UserCredentials& other) const;

 private:
  // Remember to update HashCode() and Equals() when new fields are added.
  std::string eff_user_;
  std::string real_user_;
  std::string password_;

  DISALLOW_COPY_AND_ASSIGN(UserCredentials);
};

// Used to key on Connection information.
// For use as a key in an unordered STL collection, use ConnectionIdHash and ConnectionIdEqual.
// This class is copyable for STL compatibility, but not assignable (use CopyFrom() for that).
class ConnectionId {
 public:
  ConnectionId();

  // Copy constructor required for use with STL unordered_map.
  ConnectionId(const ConnectionId& other);

  // Convenience constructor.
  ConnectionId(const Endpoint& remote, const UserCredentials& user_credentials);

  // The remote address.
  void set_remote(const Endpoint& remote);
  const Endpoint& remote() const { return remote_; }

  // The credentials of the user associated with this connection, if any.
  void set_user_credentials(const UserCredentials& user_credentials);
  const UserCredentials& user_credentials() const { return user_credentials_; }
  UserCredentials* mutable_user_credentials() { return &user_credentials_; }

  void set_idx(uint8_t idx);
  uint8_t idx() const { return idx_; }

  // Copy state from another object to this one.
  void CopyFrom(const ConnectionId& other);

  // Returns a string representation of the object, not including the password field.
  std::string ToString() const;

  size_t HashCode() const;
  bool Equals(const ConnectionId& other) const;

 private:
  // Remember to update HashCode() and Equals() when new fields are added.
  Endpoint remote_;
  UserCredentials user_credentials_;
  uint8_t idx_ = 0;  // Connection index, used to support multiple connections to the same server.

  // Implementation of CopyFrom that can be shared with copy constructor.
  void DoCopyFrom(const ConnectionId& other);

  // Disable assignment operator.
  void operator=(const ConnectionId&);
};

class ConnectionIdHash {
 public:
  std::size_t operator() (const ConnectionId& conn_id) const;
};

class ConnectionIdEqual {
 public:
  bool operator() (const ConnectionId& cid1, const ConnectionId& cid2) const;
};

// Container for OutboundCall metrics
struct OutboundCallMetrics {
  explicit OutboundCallMetrics(const scoped_refptr<MetricEntity>& metric_entity);

  scoped_refptr<Histogram> queue_time;
  scoped_refptr<Histogram> send_time;
  scoped_refptr<Histogram> time_to_response;
};

// A response to a call, on the client side.
// Upon receiving a response, this is allocated in the reactor thread and filled
// into the OutboundCall instance via OutboundCall::SetResponse.
//
// This may either be a success or error response.
//
// This class takes care of separating out the distinct payload slices sent
// over.
class CallResponse {
 public:
  static constexpr size_t kMaxSidecarSlices = 8;

  CallResponse();

  CallResponse(CallResponse&& rhs);
  void operator=(CallResponse&& rhs);

  // Parse the response received from a call. This must be called before any
  // other methods on this object.
  CHECKED_STATUS ParseFrom(Slice source);

  // Return true if the call succeeded.
  bool is_success() const {
    DCHECK(parsed_);
    return !header_.is_error();
  }

  // Return the call ID that this response is related to.
  int32_t call_id() const {
    DCHECK(parsed_);
    return header_.call_id();
  }

  // Return the serialized response data. This is just the response "body" --
  // either a serialized ErrorStatusPB, or the serialized user response protobuf.
  const Slice &serialized_response() const {
    DCHECK(parsed_);
    return serialized_response_;
  }

  // See RpcController::GetSidecar()
  CHECKED_STATUS GetSidecar(int idx, Slice* sidecar) const;

 private:
  // True once ParseFrom() is called.
  bool parsed_;

  // The parsed header.
  ResponseHeader header_;

  // The slice of data for the encoded protobuf response.
  // This slice refers to memory allocated by transfer_
  Slice serialized_response_;

  // Slices of data for rpc sidecars. They point into memory owned by transfer_.
  // Number of sidecars chould be obtained from header_.
  std::array<Slice, kMaxSidecarSlices> sidecar_slices_;

  // The incoming transfer data - retained because serialized_response_
  // and sidecar_slices_ refer into its data.
  std::vector<uint8_t> response_data_;

  DISALLOW_COPY_AND_ASSIGN(CallResponse);
};

// Tracks the status of a call on the client side.
//
// This is an internal-facing class -- clients interact with the
// RpcController class.
//
// This is allocated by the Proxy when a call is first created,
// then passed to the reactor thread to send on the wire. It's typically
// kept using a shared_ptr because a call may terminate in any number
// of different threads, making it tricky to enforce single ownership.
class OutboundCall : public RpcCall {
 public:
  OutboundCall(const ConnectionId& conn_id, const RemoteMethod& remote_method,
               const std::shared_ptr<OutboundCallMetrics>& outbound_call_metrics,
               google::protobuf::Message* response_storage,
               RpcController* controller, ResponseCallback callback);
  virtual ~OutboundCall();

  // Serialize the given request PB into this call's internal storage.
  //
  // Because the data is fully serialized by this call, 'req' may be
  // subsequently mutated with no ill effects.
  virtual CHECKED_STATUS SetRequestParam(const google::protobuf::Message& req);

  // Assign the call ID for this call. This is called from the reactor
  // thread once a connection has been assigned. Must only be called once.
  void set_call_id(int32_t call_id) {
    DCHECK_EQ(header_.call_id(), kInvalidCallId) << "Already has a call ID";
    header_.set_call_id(call_id);
  }

  // Serialize the call for the wire. Requires that SetRequestParam()
  // is called first. This is called from the Reactor thread.
  void Serialize(std::deque<RefCntBuffer>* output) const override;

  // Callback after the call has been put on the outbound connection queue.
  void SetQueued();

  // Update the call state to show that the request has been sent.
  void SetSent();

  // Update the call state to show that the call has finished.
  void SetFinished();

  // Mark the call as failed. This also triggers the callback to notify
  // the caller. If the call failed due to a remote error, then err_pb
  // should be set to the error returned by the remote server. Takes
  // ownership of 'err_pb'.
  void SetFailed(const Status& status,
                 ErrorStatusPB* err_pb = NULL);

  // Mark the call as timed out. This also triggers the callback to notify
  // the caller.
  void SetTimedOut();
  bool IsTimedOut() const;

  // Is the call finished?
  bool IsFinished() const;

  // Fill in the call response.
  void SetResponse(CallResponse&& resp);

  std::string ToString() const override;

  void DumpPB(const DumpRunningRpcsRequestPB& req, RpcCallInProgressPB* resp);

  ////////////////////////////////////////////////////////////
  // Getters
  ////////////////////////////////////////////////////////////

  const ConnectionId& conn_id() const { return conn_id_; }
  const RemoteMethod& remote_method() const { return remote_method_; }
  const ResponseCallback &callback() const { return callback_; }
  RpcController* controller() { return controller_; }
  const RpcController* controller() const { return controller_; }
  google::protobuf::Message* response() const { return response_; }

  // Return true if a call ID has been assigned to this call.
  bool call_id_assigned() const {
    return header_.call_id() != kInvalidCallId;
  }

  int32_t call_id() const {
    DCHECK(call_id_assigned());
    return header_.call_id();
  }

  Trace* trace() {
    return trace_.get();
  }

 protected:
  friend class RpcController;

  virtual CHECKED_STATUS GetSidecar(int idx, Slice* sidecar) const;

  const ConnectionId conn_id_;
  MonoTime start_;
  RpcController* const controller_;
  // Pointer for the protobuf where the response should be written.
  google::protobuf::Message* response_;

 private:
  friend class RpcController;

  // Various states the call propagates through.
  // NB: if adding another state, be sure to update OutboundCall::IsFinished()
  // and OutboundCall::StateName(State state) as well.
  enum State {
    READY = 0,
    ON_OUTBOUND_QUEUE = 1,
    SENT = 2,
    TIMED_OUT = 3,
    FINISHED_ERROR = 4,
    FINISHED_SUCCESS = 5
  };

  static std::string StateName(State state);

  virtual void NotifyTransferred(const Status& status) override;

  void set_state(State new_state);
  State state() const;

  // Same as set_state, but requires that the caller already holds
  // lock_
  void set_state_unlocked(State new_state);

  // return current status
  CHECKED_STATUS status() const;

  // Return the error protobuf, if a remote error occurred.
  // This will only be non-NULL if status().IsRemoteError().
  const ErrorStatusPB* error_pb() const;

  // Lock for state_ status_, error_pb_ fields, since they
  // may be mutated by the reactor thread while the client thread
  // reads them.
  mutable simple_spinlock lock_;
  State state_;
  Status status_;
  gscoped_ptr<ErrorStatusPB> error_pb_;

  // Call the user-provided callback.
  void CallCallback();

  // The RPC header.
  // Parts of this (eg the call ID) are only assigned once this call has been
  // passed to the reactor thread and assigned a connection.
  RequestHeader header_;

  // The remote method being called.
  const RemoteMethod remote_method_;

  ResponseCallback callback_;

  // Buffers for storing segments of the wire-format request.
  RefCntBuffer buffer_;

  // Once a response has been received for this call, contains that response.
  CallResponse call_response_;

  // The trace buffer.
  scoped_refptr<Trace> trace_;

  std::shared_ptr<OutboundCallMetrics> outbound_call_metrics_;

  DISALLOW_COPY_AND_ASSIGN(OutboundCall);
};

}  // namespace rpc
}  // namespace yb

#endif  // YB_RPC_OUTBOUND_CALL_H_
