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

#include <mutex>
#include <thread>

#include <gtest/gtest.h>
#include <glog/logging.h>

#include "yb/server/hybrid_clock.h"
#include "yb/server/logical_clock.h"
#include "yb/tablet/mvcc.h"
#include "yb/util/monotime.h"
#include "yb/util/test_util.h"
#include "yb/gutil/strings/substitute.h"

using std::thread;

namespace yb {
namespace tablet {

using server::Clock;
using server::HybridClock;
using strings::Substitute;

class MvccTest : public YBTest {
 public:
  MvccTest()
      : clock_(
          server::LogicalClock::CreateStartingAt(HybridTime::kInitialHybridTime)) {
  }

  void WaitForSnapshotAtTSThread(MvccManager* mgr, HybridTime ht) {
    MvccSnapshot s;
    CHECK_OK(mgr->WaitForCleanSnapshotAtHybridTime(ht, &s, MonoTime::Max()));
    CHECK(s.is_clean()) << "verifying postcondition";
    std::lock_guard<simple_spinlock> lock(lock_);
    result_snapshot_.reset(new MvccSnapshot(s));
  }

  bool HasResultSnapshot() {
    std::lock_guard<simple_spinlock> lock(lock_);
    return result_snapshot_ != nullptr;
  }

 protected:
  scoped_refptr<server::Clock> clock_;

  mutable simple_spinlock lock_;
  gscoped_ptr<MvccSnapshot> result_snapshot_;
};

TEST_F(MvccTest, TestMvccBasic) {
  MvccManager mgr(clock_.get());
  MvccSnapshot snap;

  // Initial state should not have any committed transactions.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(HybridTime(1)));
  ASSERT_FALSE(snap.IsCommitted(HybridTime(2)));

  // Start hybrid_time 1
  HybridTime t = mgr.StartOperation();
  ASSERT_EQ(1, t.value());

  // State should still have no committed transactions, since 1 is in-flight.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(HybridTime(1)));
  ASSERT_FALSE(snap.IsCommitted(HybridTime(2)));

  // Mark hybrid_time 1 as "applying"
  mgr.StartApplyingOperation(t);

  // This should not change the set of committed transactions.
  ASSERT_FALSE(snap.IsCommitted(HybridTime(1)));

  // Commit hybrid_time 1
  mgr.CommitOperation(t);

  // State should show 0 as committed, 1 as uncommitted.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 2}]", snap.ToString());
  ASSERT_TRUE(snap.IsCommitted(HybridTime(1)));
  ASSERT_FALSE(snap.IsCommitted(HybridTime(2)));
}

TEST_F(MvccTest, TestMvccMultipleInFlight) {
  MvccManager mgr(clock_.get());
  MvccSnapshot snap;

  // Start hybrid_time 1, hybrid_time 2
  HybridTime t1 = mgr.StartOperation();
  ASSERT_EQ(1, t1.value());
  HybridTime t2 = mgr.StartOperation();
  ASSERT_EQ(2, t2.value());

  // State should still have no committed transactions, since both are in-flight.

  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 1}]", snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_FALSE(snap.IsCommitted(t2));

  // Commit hybrid_time 2
  mgr.StartApplyingOperation(t2);
  mgr.CommitOperation(t2);

  // State should show 2 as committed, 1 as uncommitted.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed="
            "{T|T < 1 or (T in {2})}]",
            snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));

  // Start another transaction. This gets hybrid_time 3
  HybridTime t3 = mgr.StartOperation();
  ASSERT_EQ(3, t3.value());

  // State should show 2 as committed, 1 and 4 as uncommitted.
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed="
            "{T|T < 1 or (T in {2})}]",
            snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));
  ASSERT_FALSE(snap.IsCommitted(t3));

  // Commit 3
  mgr.StartApplyingOperation(t3);
  mgr.CommitOperation(t3);

  // 2 and 3 committed
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed="
            "{T|T < 1 or (T in {2,3})}]",
            snap.ToString());
  ASSERT_FALSE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));
  ASSERT_TRUE(snap.IsCommitted(t3));

  // Commit 1
  mgr.StartApplyingOperation(t1);
  mgr.CommitOperation(t1);

  // all committed
  mgr.TakeSnapshot(&snap);
  ASSERT_EQ("MvccSnapshot[committed={T|T < 4}]", snap.ToString());
  ASSERT_TRUE(snap.IsCommitted(t1));
  ASSERT_TRUE(snap.IsCommitted(t2));
  ASSERT_TRUE(snap.IsCommitted(t3));
}

TEST_F(MvccTest, TestOutOfOrderTxns) {
  scoped_refptr<Clock> hybrid_clock(new HybridClock());
  ASSERT_OK(hybrid_clock->Init());
  MvccManager mgr(hybrid_clock);

  // Start a normal non-commit-wait txn.
  HybridTime normal_txn = mgr.StartOperation();

  MvccSnapshot s1(mgr);

  // Start a transaction as if it were using commit-wait (i.e. started in future)
  HybridTime cw_txn = mgr.StartOperationAtLatest();

  // Commit the original txn
  mgr.StartApplyingOperation(normal_txn);
  mgr.CommitOperation(normal_txn);

  // Start a new txn
  HybridTime normal_txn_2 = mgr.StartOperation();

  // The old snapshot should not have either txn
  EXPECT_FALSE(s1.IsCommitted(normal_txn));
  EXPECT_FALSE(s1.IsCommitted(normal_txn_2));

  // A new snapshot should have only the first transaction
  MvccSnapshot s2(mgr);
  EXPECT_TRUE(s2.IsCommitted(normal_txn));
  EXPECT_FALSE(s2.IsCommitted(normal_txn_2));

  // Commit the commit-wait one once it is time.
  ASSERT_OK(hybrid_clock->WaitUntilAfter(cw_txn, MonoTime::Max()));
  mgr.StartApplyingOperation(cw_txn);
  mgr.CommitOperation(cw_txn);

  // A new snapshot at this point should still think that normal_txn_2 is uncommitted
  MvccSnapshot s3(mgr);
  EXPECT_FALSE(s3.IsCommitted(normal_txn_2));
}

// Tests starting transaction at a point-in-time in the past and committing them.
// This is disconnected from the current time (whatever is returned from clock->Now())
// for replication/bootstrap.
TEST_F(MvccTest, TestOfflineOperations) {
  MvccManager mgr(clock_.get());

  // set the clock to some time in the "future"
  ASSERT_OK(clock_->Update(HybridTime(100)));

  // now start a transaction in the "past"
  ASSERT_OK(mgr.StartOperationAtHybridTime(HybridTime(50)));

  ASSERT_GE(mgr.GetMaxSafeTimeToReadAt(), HybridTime::kMin);

  // and committing this transaction "offline" this
  // should not advance the MvccManager 'all_committed_before_'
  // watermark.
  mgr.StartApplyingOperation(HybridTime(50));
  mgr.OfflineCommitOperation(HybridTime(50));

  // Now take a snaphsot.
  MvccSnapshot snap1;
  mgr.TakeSnapshot(&snap1);

  // Because we did not advance the watermark, even though the only
  // in-flight transaction was committed at time 50, a transaction at
  // time 40 should still be considered uncommitted.
  ASSERT_FALSE(snap1.IsCommitted(HybridTime(40)));

  // Now advance the watermark to the last committed transaction.
  mgr.OfflineAdjustSafeTime(HybridTime(50));

  ASSERT_GE(mgr.GetMaxSafeTimeToReadAt(), HybridTime(50));

  MvccSnapshot snap2;
  mgr.TakeSnapshot(&snap2);

  ASSERT_TRUE(snap2.IsCommitted(HybridTime(40)));
}

TEST_F(MvccTest, TestScopedOperation) {
  MvccManager mgr(clock_.get());
  MvccSnapshot snap;

  {
    ScopedWriteOperation t1(&mgr);
    ScopedWriteOperation t2(&mgr);

    ASSERT_EQ(1, t1.hybrid_time().value());
    ASSERT_EQ(2, t2.hybrid_time().value());

    t1.StartApplying();
    t1.Commit();

    mgr.TakeSnapshot(&snap);
    ASSERT_TRUE(snap.IsCommitted(t1.hybrid_time()));
    ASSERT_FALSE(snap.IsCommitted(t2.hybrid_time()));
  }

  // t2 going out of scope aborts it.
  mgr.TakeSnapshot(&snap);
  ASSERT_TRUE(snap.IsCommitted(HybridTime(1)));
  ASSERT_FALSE(snap.IsCommitted(HybridTime(2)));
}

TEST_F(MvccTest, TestPointInTimeSnapshot) {
  MvccSnapshot snap(HybridTime(10));

  ASSERT_TRUE(snap.IsCommitted(HybridTime(1)));
  ASSERT_TRUE(snap.IsCommitted(HybridTime(9)));
  ASSERT_FALSE(snap.IsCommitted(HybridTime(10)));
  ASSERT_FALSE(snap.IsCommitted(HybridTime(11)));
}

TEST_F(MvccTest, TestMayHaveCommittedOperationsAtOrAfter) {
  MvccSnapshot snap;
  snap.all_committed_before_ = HybridTime(10);
  snap.committed_hybrid_times_.push_back(11);
  snap.committed_hybrid_times_.push_back(13);
  snap.none_committed_at_or_after_ = HybridTime(14);

  ASSERT_TRUE(snap.MayHaveCommittedOperationsAtOrAfter(HybridTime(9)));
  ASSERT_TRUE(snap.MayHaveCommittedOperationsAtOrAfter(HybridTime(10)));
  ASSERT_TRUE(snap.MayHaveCommittedOperationsAtOrAfter(HybridTime(12)));
  ASSERT_TRUE(snap.MayHaveCommittedOperationsAtOrAfter(HybridTime(13)));
  ASSERT_FALSE(snap.MayHaveCommittedOperationsAtOrAfter(HybridTime(14)));
  ASSERT_FALSE(snap.MayHaveCommittedOperationsAtOrAfter(HybridTime(15)));

  // Test for "all committed" snapshot
  MvccSnapshot all_committed =
      MvccSnapshot::CreateSnapshotIncludingAllOperations();
  ASSERT_TRUE(
      all_committed.MayHaveCommittedOperationsAtOrAfter(HybridTime(1)));
  ASSERT_TRUE(
      all_committed.MayHaveCommittedOperationsAtOrAfter(HybridTime(12345)));

  // And "none committed" snapshot
  MvccSnapshot none_committed =
      MvccSnapshot::CreateSnapshotIncludingNoOperations();
  ASSERT_FALSE(
      none_committed.MayHaveCommittedOperationsAtOrAfter(HybridTime(1)));
  ASSERT_FALSE(
      none_committed.MayHaveCommittedOperationsAtOrAfter(HybridTime(12345)));

  // Test for a "clean" snapshot
  MvccSnapshot clean_snap(HybridTime(10));
  ASSERT_TRUE(clean_snap.MayHaveCommittedOperationsAtOrAfter(HybridTime(9)));
  ASSERT_FALSE(clean_snap.MayHaveCommittedOperationsAtOrAfter(HybridTime(10)));
}

TEST_F(MvccTest, TestMayHaveUncommittedOperationsBefore) {
  MvccSnapshot snap;
  snap.all_committed_before_ = HybridTime(10);
  snap.committed_hybrid_times_.push_back(11);
  snap.committed_hybrid_times_.push_back(13);
  snap.none_committed_at_or_after_ = HybridTime(14);

  ASSERT_FALSE(snap.MayHaveUncommittedOperationsAtOrBefore(HybridTime(9)));
  ASSERT_TRUE(snap.MayHaveUncommittedOperationsAtOrBefore(HybridTime(10)));
  ASSERT_TRUE(snap.MayHaveUncommittedOperationsAtOrBefore(HybridTime(11)));
  ASSERT_TRUE(snap.MayHaveUncommittedOperationsAtOrBefore(HybridTime(13)));
  ASSERT_TRUE(snap.MayHaveUncommittedOperationsAtOrBefore(HybridTime(14)));
  ASSERT_TRUE(snap.MayHaveUncommittedOperationsAtOrBefore(HybridTime(15)));

  // Test for "all committed" snapshot
  MvccSnapshot all_committed =
      MvccSnapshot::CreateSnapshotIncludingAllOperations();
  ASSERT_FALSE(
      all_committed.MayHaveUncommittedOperationsAtOrBefore(HybridTime(1)));
  ASSERT_FALSE(
      all_committed.MayHaveUncommittedOperationsAtOrBefore(HybridTime(12345)));

  // And "none committed" snapshot
  MvccSnapshot none_committed =
      MvccSnapshot::CreateSnapshotIncludingNoOperations();
  ASSERT_TRUE(
      none_committed.MayHaveUncommittedOperationsAtOrBefore(HybridTime(1)));
  ASSERT_TRUE(
      none_committed.MayHaveUncommittedOperationsAtOrBefore(
          HybridTime(12345)));

  // Test for a "clean" snapshot
  MvccSnapshot clean_snap(HybridTime(10));
  ASSERT_FALSE(clean_snap.MayHaveUncommittedOperationsAtOrBefore(HybridTime(9)));
  ASSERT_TRUE(clean_snap.MayHaveUncommittedOperationsAtOrBefore(HybridTime(10)));

  // Test for the case where we have a single transaction in flight. Since this is
  // also the earliest transaction, all_committed_before_ is equal to the txn's
  // hybrid time, but when it gets committed we can't advance all_committed_before_ past it
  // because there is no other transaction to advance it to. In this case we should
  // still report that there can't be any uncommitted transactions before.
  MvccSnapshot snap2;
  snap2.all_committed_before_ = HybridTime(10);
  snap2.committed_hybrid_times_.push_back(10);

  ASSERT_FALSE(snap2.MayHaveUncommittedOperationsAtOrBefore(HybridTime(10)));
}

TEST_F(MvccTest, TestAreAllOperationsCommitted) {
  MvccManager mgr(clock_.get());

  // start several transactions and take snapshots along the way
  HybridTime tx1 = mgr.StartOperation();
  HybridTime tx2 = mgr.StartOperation();
  HybridTime tx3 = mgr.StartOperation();

  ASSERT_FALSE(mgr.AreAllOperationsCommitted(HybridTime(1)));
  ASSERT_FALSE(mgr.AreAllOperationsCommitted(HybridTime(2)));
  ASSERT_FALSE(mgr.AreAllOperationsCommitted(HybridTime(3)));

  // commit tx3, should all still report as having as having uncommitted
  // transactions.
  mgr.StartApplyingOperation(tx3);
  mgr.CommitOperation(tx3);
  ASSERT_FALSE(mgr.AreAllOperationsCommitted(HybridTime(1)));
  ASSERT_FALSE(mgr.AreAllOperationsCommitted(HybridTime(2)));
  ASSERT_FALSE(mgr.AreAllOperationsCommitted(HybridTime(3)));

  // commit tx1, first snap with in-flights should now report as all committed
  // and remaining snaps as still having uncommitted transactions
  mgr.StartApplyingOperation(tx1);
  mgr.CommitOperation(tx1);
  ASSERT_TRUE(mgr.AreAllOperationsCommitted(HybridTime(1)));
  ASSERT_FALSE(mgr.AreAllOperationsCommitted(HybridTime(2)));
  ASSERT_FALSE(mgr.AreAllOperationsCommitted(HybridTime(3)));

  // Now they should all report as all committed.
  mgr.StartApplyingOperation(tx2);
  mgr.CommitOperation(tx2);
  ASSERT_TRUE(mgr.AreAllOperationsCommitted(HybridTime(1)));
  ASSERT_TRUE(mgr.AreAllOperationsCommitted(HybridTime(2)));
  ASSERT_TRUE(mgr.AreAllOperationsCommitted(HybridTime(3)));
}

TEST_F(MvccTest, TestWaitForCleanSnapshot_SnapWithNoInflights) {
  MvccManager mgr(clock_.get());
  thread waiting_thread = thread(
      &MvccTest::WaitForSnapshotAtTSThread, this, &mgr, clock_->Now());

  // join immediately.
  waiting_thread.join();
  ASSERT_TRUE(HasResultSnapshot());
}

TEST_F(MvccTest, TestWaitForCleanSnapshot_SnapWithInFlights) {

  MvccManager mgr(clock_.get());

  HybridTime tx1 = mgr.StartOperation();
  HybridTime tx2 = mgr.StartOperation();

  thread waiting_thread = thread(
      &MvccTest::WaitForSnapshotAtTSThread, this, &mgr, clock_->Now());

  ASSERT_FALSE(HasResultSnapshot());
  mgr.StartApplyingOperation(tx1);
  mgr.CommitOperation(tx1);
  ASSERT_FALSE(HasResultSnapshot());
  mgr.StartApplyingOperation(tx2);
  mgr.CommitOperation(tx2);
  waiting_thread.join();
  ASSERT_TRUE(HasResultSnapshot());
}

TEST_F(MvccTest, TestWaitForApplyingOperationsToCommit) {
  MvccManager mgr(clock_.get());

  HybridTime tx1 = mgr.StartOperation();
  HybridTime tx2 = mgr.StartOperation();

  // Wait should return immediately, since we have no transactions "applying"
  // yet.
  mgr.WaitForApplyingOperationsToCommit();

  mgr.StartApplyingOperation(tx1);

  thread waiting_thread = thread(
      &MvccManager::WaitForApplyingOperationsToCommit, &mgr);
  while (mgr.GetNumWaitersForTests() == 0) {
    SleepFor(MonoDelta::FromMilliseconds(5));
  }
  ASSERT_EQ(mgr.GetNumWaitersForTests(), 1);

  // Aborting the other transaction shouldn't affect our waiter.
  mgr.AbortOperation(tx2);
  ASSERT_EQ(mgr.GetNumWaitersForTests(), 1);

  // Committing our transaction should wake the waiter.
  mgr.CommitOperation(tx1);
  ASSERT_EQ(mgr.GetNumWaitersForTests(), 0);
  waiting_thread.join();
}

TEST_F(MvccTest, TestWaitForCleanSnapshot_SnapAtHybridTimeWithInFlights) {

  MvccManager mgr(clock_.get());

  // Operations with hybrid_time 1 through 3
  HybridTime tx1 = mgr.StartOperation();
  HybridTime tx2 = mgr.StartOperation();
  HybridTime tx3 = mgr.StartOperation();

  // Start a thread waiting for transactions with ht <= 2 to commit
  thread waiting_thread = thread(
      &MvccTest::WaitForSnapshotAtTSThread, this, &mgr, tx2);
  ASSERT_FALSE(HasResultSnapshot());

  // Commit tx 1 - thread should still wait.
  mgr.StartApplyingOperation(tx1);
  mgr.CommitOperation(tx1);
  SleepFor(MonoDelta::FromMilliseconds(1));
  ASSERT_FALSE(HasResultSnapshot());

  // Commit tx 3 - thread should still wait.
  mgr.StartApplyingOperation(tx3);
  mgr.CommitOperation(tx3);
  SleepFor(MonoDelta::FromMilliseconds(1));
  ASSERT_FALSE(HasResultSnapshot());

  // Commit tx 2 - thread can now continue
  mgr.StartApplyingOperation(tx2);
  mgr.CommitOperation(tx2);
  waiting_thread.join();
  ASSERT_TRUE(HasResultSnapshot());
}

// Test that if we abort a transaction we don't advance the safe time and don't
// add the transaction to the committed set.
TEST_F(MvccTest, TestTxnAbort) {

  MvccManager mgr(clock_.get());

  // Operations with hybrid_times 1 through 3
  HybridTime tx1 = mgr.StartOperation();
  HybridTime tx2 = mgr.StartOperation();
  HybridTime tx3 = mgr.StartOperation();

  // Now abort tx1, this shouldn't move the clean time and the transaction
  // shouldn't be reported as committed.
  mgr.AbortOperation(tx1);
  ASSERT_FALSE(mgr.cur_snap_.IsCommitted(tx1));

  // Committing tx3 shouldn't advance the clean time since it is not the earliest
  // in-flight, but it should advance 'no_new_transactions_at_or_before_', the "safe"
  // time, to 3.
  mgr.StartApplyingOperation(tx3);
  mgr.CommitOperation(tx3);
  ASSERT_TRUE(mgr.cur_snap_.IsCommitted(tx3));
  ASSERT_EQ(mgr.no_new_transactions_at_or_before_.CompareTo(tx3), 0);

  // Committing tx2 should advance the clean time to 3.
  mgr.StartApplyingOperation(tx2);
  mgr.CommitOperation(tx2);
  ASSERT_TRUE(mgr.cur_snap_.IsCommitted(tx2));
  ASSERT_GE(mgr.GetMaxSafeTimeToReadAt().CompareTo(tx3), 0);
}

// This tests for a bug we were observing, where a clean snapshot would not
// coalesce to the latest hybrid_time, for offline transactions.
TEST_F(MvccTest, TestCleanTimeCoalescingOnOfflineOperations) {

  MvccManager mgr(clock_.get());
  CHECK_OK(clock_->Update(HybridTime(20)));

  CHECK_OK(mgr.StartOperationAtHybridTime(HybridTime(10)));
  CHECK_OK(mgr.StartOperationAtHybridTime(HybridTime(15)));
  mgr.OfflineAdjustSafeTime(HybridTime(15));

  mgr.StartApplyingOperation(HybridTime(15));
  mgr.OfflineCommitOperation(HybridTime(15));

  mgr.StartApplyingOperation(HybridTime(10));
  mgr.OfflineCommitOperation(HybridTime(10));
  ASSERT_EQ(mgr.cur_snap_.ToString(), "MvccSnapshot[committed={T|T < 16}]");
}

// Various death tests which ensure that we can only transition in one of the following
// valid ways:
//
// - Start() -> StartApplying() -> Commit()
// - Start() -> Abort()
//
// Any other transition should fire a CHECK failure.
TEST_F(MvccTest, TestIllegalStateTransitionsCrash) {
  MvccManager mgr(clock_.get());
  MvccSnapshot snap;

  EXPECT_DEATH({
      mgr.StartApplyingOperation(HybridTime(1));
    }, "Cannot mark hybrid_time 1 as APPLYING: not in the in-flight map");

  // Depending whether this is a DEBUG or RELEASE build, the error message
  // could be different for this case -- the "future hybrid_time" check is only
  // run in DEBUG builds.
  EXPECT_DEATH({
      mgr.CommitOperation(HybridTime(1));
    },
    "Trying to commit a transaction with a future hybrid_time|"
    "Trying to remove hybrid_time which isn't in the in-flight set: 1");

  CHECK_OK(clock_->Update(HybridTime(20)));

  EXPECT_DEATH({
      mgr.CommitOperation(HybridTime(1));
    }, "Trying to remove hybrid_time which isn't in the in-flight set: 1");

  // Start a transaction, and try committing it without having moved to "Applying"
  // state.
  HybridTime t = mgr.StartOperation();
  EXPECT_DEATH({
      mgr.CommitOperation(t);
    }, "Trying to commit a transaction which never entered APPLYING state");

  // Aborting should succeed, since we never moved to Applying.
  mgr.AbortOperation(t);

  // Aborting a second time should fail
  EXPECT_DEATH({
      mgr.AbortOperation(t);
    }, "Trying to remove hybrid_time which isn't in the in-flight set: 21");

  // Start a new transaction. This time, mark it as Applying.
  t = mgr.StartOperation();
  mgr.StartApplyingOperation(t);

  // Can only call StartApplying once.
  EXPECT_DEATH({
      mgr.StartApplyingOperation(t);
    }, "Cannot mark hybrid_time 22 as APPLYING: wrong state: 1");

  // Cannot Abort() a transaction once we start applying it.
  EXPECT_DEATH({
      mgr.AbortOperation(t);
    }, "transaction with hybrid_time 22 cannot be aborted in state 1");

  // We can commit it successfully.
  mgr.CommitOperation(t);
}

TEST_F(MvccTest, TestWaitUntilCleanDeadline) {
  MvccManager mgr(clock_.get());

  // Operations with hybrid_time 1 through 3
  HybridTime tx1 = mgr.StartOperation();

  // Wait until the 'tx1' hybrid_time is clean -- this won't happen because the
  // transaction isn't committed yet.
  MonoTime deadline = MonoTime::Now(MonoTime::FINE);
  deadline.AddDelta(MonoDelta::FromMilliseconds(10));
  MvccSnapshot snap;
  Status s = mgr.WaitForCleanSnapshotAtHybridTime(tx1, &snap, deadline);
  ASSERT_TRUE(s.IsTimedOut()) << s.ToString();
}

TEST_F(MvccTest, TestMaxSafeTimeToReadAt) {
  MvccManager mgr(clock_.get());
  auto apply_and_commit = [&](HybridTime tx_to_commit) {
    mgr.StartApplyingOperation(tx_to_commit);
    mgr.CommitOperation(tx_to_commit);
  };

  // Start four transactions, don't commit them yet.
  for (int i = 1; i <= 4; ++i) {
    ASSERT_EQ(i, mgr.StartOperation().ToUint64());
    // We haven't committed any transactions yet, so the safe time is zero.
    ASSERT_EQ(HybridTime::kMin, mgr.GetMaxSafeTimeToReadAt());
  }

  // Commit previous transactions and start new transactions at the same time (up to 10 total),
  // then just keep committing txns until all but one are committed.
  for (int i = 5; i <= 13; ++i) {
    if (i <= 10) {
      ASSERT_EQ(i, mgr.StartOperation().ToUint64());
    }
    const HybridTime tx_to_commit(i - 4);
    apply_and_commit(tx_to_commit);
    SCOPED_TRACE(Substitute("i=$0", i));
    ASSERT_EQ(tx_to_commit, mgr.GetMaxSafeTimeToReadAt());
  }

  // Commit one more transaction, but now that there are no more transactions in flight, safe time
  // should start returning current time.
  apply_and_commit(HybridTime(10));
  ASSERT_EQ(HybridTime(11), mgr.GetMaxSafeTimeToReadAt());
  ASSERT_EQ(HybridTime(12), mgr.GetMaxSafeTimeToReadAt());
}


} // namespace tablet
} // namespace yb
