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
#ifndef YB_TABLET_LOCAL_TABLET_WRITER_H_
#define YB_TABLET_LOCAL_TABLET_WRITER_H_

#include <vector>

#include "yb/common/partial_row.h"
#include "yb/common/row_operations.h"
#include "yb/consensus/log_anchor_registry.h"
#include "yb/consensus/opid_util.h"
#include "yb/tablet/row_op.h"
#include "yb/tablet/tablet.h"
#include "yb/tablet/operations/write_operation.h"
#include "yb/gutil/macros.h"
#include "yb/gutil/singleton.h"

namespace yb {
namespace tablet {

// This is used for providing OpIds to write operations, which must always be increasing.
class AutoIncrementingCounter {
 public:
  AutoIncrementingCounter() : next_index_(1) {}
  int64_t GetAndIncrement() { return next_index_.fetch_add(1); }
 private:
  std::atomic<int64_t> next_index_;
};

// Helper class to write directly into a local tablet, without going
// through TabletPeer, consensus, etc.
//
// This is useful for unit-testing the Tablet code paths with no consensus
// implementation or thread pools.
class LocalTabletWriter {
 public:
  struct Op {
    Op(RowOperationsPB::Type type,
       const YBPartialRow* row)
      : type(type),
        row(row) {
    }

    RowOperationsPB::Type type;
    const YBPartialRow* row;
  };

  explicit LocalTabletWriter(Tablet* tablet,
                             const Schema* client_schema)
    : tablet_(tablet),
      client_schema_(client_schema) {
    CHECK(!client_schema->has_column_ids());
    CHECK_OK(SchemaToPB(*client_schema, req_.mutable_schema()));
  }

  ~LocalTabletWriter() {}

  CHECKED_STATUS Insert(const YBPartialRow& row) {
    return Write(RowOperationsPB::INSERT, row);
  }

  CHECKED_STATUS Delete(const YBPartialRow& row) {
    return Write(RowOperationsPB::DELETE, row);
  }

  CHECKED_STATUS Update(const YBPartialRow& row) {
    return Write(RowOperationsPB::UPDATE, row);
  }

  // Perform a write against the local tablet.
  // Returns a bad Status if the applied operation had a per-row error.
  CHECKED_STATUS Write(RowOperationsPB::Type type,
                       const YBPartialRow& row) {
    vector<Op> ops;
    ops.push_back(Op(type, &row));
    return WriteBatch(ops);
  }

  CHECKED_STATUS WriteBatch(const std::vector<Op>& ops) {
    req_.mutable_row_operations()->Clear();
    RowOperationsPBEncoder encoder(req_.mutable_row_operations());

    for (const Op& op : ops) {
      encoder.Add(op.type, *op.row);
    }

    tx_state_.reset(new WriteOperationState(nullptr, &req_, nullptr));
    // Note: Order of lock/decode differs for these two table types, anyway temporary as KUDU
    // codepath will be removed once all tests are converted to QL.
    if (tablet_->table_type() != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
      RETURN_NOT_OK(tablet_->AcquireLocksAndPerformDocOperations(tx_state_.get()));
      RETURN_NOT_OK(tablet_->DecodeWriteOperations(client_schema_, tx_state_.get()));
    } else {
      RETURN_NOT_OK(tablet_->DecodeWriteOperations(client_schema_, tx_state_.get()));
      RETURN_NOT_OK(tablet_->AcquireKuduRowLocks(tx_state_.get()));
    }
    tablet_->StartOperation(tx_state_.get());

    // Create a "fake" OpId and set it in the OperationState for anchoring.
    if (tablet_->table_type() != TableType::KUDU_COLUMNAR_TABLE_TYPE) {
      tx_state_->mutable_op_id()->set_term(0);
      tx_state_->mutable_op_id()->set_index(
          Singleton<AutoIncrementingCounter>::get()->GetAndIncrement());
    } else {
      tx_state_->mutable_op_id()->CopyFrom(consensus::MaximumOpId());
    }

    tablet_->ApplyRowOperations(tx_state_.get());

    tx_state_->ReleaseTxResultPB(&result_);
    tx_state_->Commit();
    tx_state_->ReleaseDocDbLocks(tablet_);
    tx_state_->ReleaseSchemaLock();

    // Return the status of first failed op.
    int op_idx = 0;
    for (const OperationResultPB& result : result_.ops()) {
      if (result.has_failed_status()) {
        return StatusFromPB(result.failed_status()).CloneAndPrepend(ops[op_idx].row->ToString());
      }
      op_idx++;
    }
    return Status::OK();
  }

  // Return the result of the last row operation run against the tablet.
  const OperationResultPB& last_op_result() {
    CHECK_GE(result_.ops_size(), 1);
    return result_.ops(result_.ops_size() - 1);
  }

 private:
  Tablet* const tablet_;
  const Schema* client_schema_;

  TxResultPB result_;
  tserver::WriteRequestPB req_;
  std::unique_ptr<WriteOperationState> tx_state_;

  DISALLOW_COPY_AND_ASSIGN(LocalTabletWriter);
};


}  // namespace tablet
}  // namespace yb
#endif  // YB_TABLET_LOCAL_TABLET_WRITER_H_
