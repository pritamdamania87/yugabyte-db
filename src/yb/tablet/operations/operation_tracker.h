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

#ifndef YB_TABLET_OPERATIONS_OPERATION_TRACKER_H
#define YB_TABLET_OPERATIONS_OPERATION_TRACKER_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "yb/gutil/gscoped_ptr.h"
#include "yb/gutil/ref_counted.h"
#include "yb/tablet/operations/operation.h"
#include "yb/util/locks.h"

namespace yb {

template<class T>
class AtomicGauge;
class Counter;
class MemTracker;
class MetricEntity;

namespace tablet {
class OperationDriver;

// Each TabletPeer has a OperationTracker which keeps track of pending operations.
// Each "LeaderOperation" will register itself by calling Add().
// It will remove itself by calling Release().
class OperationTracker {
 public:
  OperationTracker();
  ~OperationTracker();

  // Adds a operation to the set of tracked operations.
  //
  // In the event that the tracker's memory limit is exceeded, returns a
  // ServiceUnavailable status.
  CHECKED_STATUS Add(OperationDriver* driver);

  // Removes the operation from the pending list.
  // Also triggers the deletion of the Operation object, if its refcount == 0.
  void Release(OperationDriver* driver);

  // Populates list of currently-running operations into 'pending_out' vector.
  std::vector<scoped_refptr<OperationDriver>> GetPendingOperations() const;

  // Returns number of pending operations.
  int GetNumPendingForTests() const;

  void WaitForAllToFinish() const;
  CHECKED_STATUS WaitForAllToFinish(const MonoDelta& timeout) const;

  void StartInstrumentation(const scoped_refptr<MetricEntity>& metric_entity);
  void StartMemoryTracking(const std::shared_ptr<MemTracker>& parent_mem_tracker);

 private:
  struct Metrics {
    explicit Metrics(const scoped_refptr<MetricEntity>& entity);

    scoped_refptr<AtomicGauge<uint64_t> > all_operations_inflight;
    scoped_refptr<AtomicGauge<uint64_t> > operations_inflight[Operation::kOperationTypes];

    scoped_refptr<Counter> operation_memory_pressure_rejections;
  };

  // Increments relevant metric counters.
  void IncrementCounters(const OperationDriver& driver) const;

  // Decrements relevant metric counters.
  void DecrementCounters(const OperationDriver& driver) const;

  mutable simple_spinlock lock_;

  // Per-operation state that is tracked along with the operation itself.
  struct State {
    State();

    // Approximate memory footprint of the operation.
    int64_t memory_footprint;
  };

  // Protected by 'lock_'.
  typedef std::unordered_map<scoped_refptr<OperationDriver>,
      State,
      ScopedRefPtrHashFunctor<OperationDriver>,
      ScopedRefPtrEqualToFunctor<OperationDriver> > OperationMap;
  OperationMap pending_operations_;

  gscoped_ptr<Metrics> metrics_;

  std::shared_ptr<MemTracker> mem_tracker_;

  DISALLOW_COPY_AND_ASSIGN(OperationTracker);
};

}  // namespace tablet
}  // namespace yb

#endif // YB_TABLET_OPERATIONS_OPERATION_TRACKER_H
