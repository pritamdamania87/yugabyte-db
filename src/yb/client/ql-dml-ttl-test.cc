// Copyright (c) YugaByte, Inc.
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

#include "yb/client/ql-dml-test-base.h"
#include "yb/ql/util/statement_result.h"

namespace yb {
namespace client {

using yb::ql::RowsResult;


namespace {

const std::vector<std::string> kAllColumns = {"k", "c1", "c2", "c3", "c4"};

}

class QLDmlTTLTest : public QLDmlTestBase {
 public:
  void SetUp() override {
    QLDmlTestBase::SetUp();

    YBSchemaBuilder b;
    b.AddColumn("k")->Type(INT32)->HashPrimaryKey()->NotNull();
    b.AddColumn("c1")->Type(INT32);
    b.AddColumn("c2")->Type(STRING);
    b.AddColumn("c3")->Type(INT32);
    b.AddColumn("c4")->Type(STRING);

    table_.Create(kTableName, client_.get(), &b);
  }

  TableHandle table_;
};

TEST_F(QLDmlTTLTest, TestInsertWithTTL) {
  {
    // insert into t (k, c1, c2) values (1, 1, "yuga-hello") using ttl 2;
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    YBPartialRow *prow = op->mutable_row();
    table_.SetInt32ColumnValue(req->add_hashed_column_values(), "k", 1, prow, 0);
    table_.SetInt32ColumnValue(req->add_column_values(), "c1", 1);
    table_.SetStringColumnValue(req->add_column_values(), "c2", "yuga-hello");
    req->set_ttl(2 * 1000);
    const shared_ptr<YBSession> session(client_->NewSession(false /* read_only */));
    CHECK_OK(session->Apply(op));

    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // insert into t (k, c3, c4) values (1, 2, "yuga-hi") using ttl 4;
    const shared_ptr<YBqlWriteOp> op = table_.NewWriteOp(QLWriteRequestPB::QL_STMT_INSERT);
    auto* const req = op->mutable_request();
    YBPartialRow *prow = op->mutable_row();
    table_.SetInt32ColumnValue(req->add_hashed_column_values(), "k", 1, prow, 0);
    table_.SetInt32ColumnValue(req->add_column_values(), "c3", 2);
    table_.SetStringColumnValue(req->add_column_values(), "c4", "yuga-hi");
    req->set_ttl(4 * 1000);
    const shared_ptr<YBSession> session(client_->NewSession(false /* read_only */));
    CHECK_OK(session->Apply(op));

    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
  }

  {
    // select * from t where k = 1;
    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    YBPartialRow *prow = op->mutable_row();
    table_.SetInt32ColumnValue(req->add_hashed_column_values(), "k", 1, prow, 0);
    table_.AddColumns(kAllColumns, req);

    const shared_ptr<YBSession> session(client_->NewSession(true /* read_only */));
    CHECK_OK(session->Apply(op));

    // Expect all 4 columns (c1, c2, c3, c4) to be valid right now.
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(row.column(0).int32_value(), 1);
    EXPECT_EQ(row.column(1).int32_value(), 1);
    EXPECT_EQ(row.column(2).string_value(), "yuga-hello");
    EXPECT_EQ(row.column(3).int32_value(), 2);
    EXPECT_EQ(row.column(4).string_value(), "yuga-hi");
  }

  LOG(INFO) << "Sleep for 2.5 seconds..";
  SleepFor(MonoDelta::FromMilliseconds(2500));

  {
    // select * from t where k = 1;
    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    YBPartialRow *prow = op->mutable_row();
    table_.SetInt32ColumnValue(req->add_hashed_column_values(), "k", 1, prow, 0);
    table_.AddColumns(kAllColumns, req);

    const shared_ptr<YBSession> session(client_->NewSession(true /* read_only */));
    CHECK_OK(session->Apply(op));

    // Expect columns (c1, c2) to be null and (c3, c4) to be valid right now.
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 1);
    const auto& row = rowblock->row(0);
    EXPECT_EQ(row.column(0).int32_value(), 1);
    EXPECT_TRUE(row.column(1).IsNull());
    EXPECT_TRUE(row.column(2).IsNull());
    EXPECT_EQ(row.column(3).int32_value(), 2);
    EXPECT_EQ(row.column(4).string_value(), "yuga-hi");
  }

  LOG(INFO) << "Sleep for 2.5 seconds..";
  SleepFor(MonoDelta::FromMilliseconds(2500));

  {
    // select * from t where k = 1;
    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    YBPartialRow *prow = op->mutable_row();
    table_.SetInt32ColumnValue(req->add_hashed_column_values(), "k", 1, prow, 0);
    table_.AddColumns(kAllColumns, req);

    const shared_ptr<YBSession> session(client_->NewSession(true /* read_only */));
    CHECK_OK(session->Apply(op));

    // Expect all 4 columns (c1, c2, c3, c4) to be null.
    EXPECT_EQ(op->response().status(), QLResponsePB::YQL_STATUS_OK);
    unique_ptr<QLRowBlock> rowblock(RowsResult(op.get()).GetRowBlock());
    EXPECT_EQ(rowblock->row_count(), 0);
  }

}
}  // namespace client
}  // namespace yb
