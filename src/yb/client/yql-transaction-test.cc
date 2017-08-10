//
// Copyright (c) YugaByte, Inc.
//

#include <thread>

#include <boost/optional/optional.hpp>

#include "yb/client/transaction.h"
#include "yb/client/transaction_manager.h"
#include "yb/client/yql-dml-test-base.h"

#include "yb/sql/util/statement_result.h"

#include "yb/tablet/transaction_coordinator.h"

#include "yb/tserver/mini_tablet_server.h"
#include "yb/tserver/ts_tablet_manager.h"
#include "yb/tserver/tablet_server.h"

using namespace std::literals;

namespace yb {
namespace client {

class YqlTransactionTest : public YqlDmlTestBase {
 protected:
  void SetUp() override {
    YqlDmlTestBase::SetUp();
    DontVerifyClusterBeforeNextTearDown(); // TODO(dtxn) temporary

    YBSchemaBuilder builder;
    builder.AddColumn("k")->Type(INT32)->HashPrimaryKey()->NotNull();
    builder.AddColumn("v")->Type(INT32);

    table_.Create(kTableName, client_.get(), &builder);

    transaction_manager_.emplace(client_);
  }

  // Insert a full, single row, equivalent to the insert statement below. Return a YB write op that
  // has been applied.
  //   insert into t values (h1, h2, r1, r2, c1, c2);
  shared_ptr<YBqlWriteOp> InsertRow(const YBSessionPtr& session, int32_t key, int32_t value) {
    const auto op = table_.NewWriteOp(YQLWriteRequestPB::YQL_STMT_INSERT);
    auto* const req = op->mutable_request();
    YBPartialRow *prow = op->mutable_row();
    table_.SetInt32ColumnValue(req->add_hashed_column_values(), "k", key, prow, 0);
    table_.SetInt32ColumnValue(req->add_column_values(), "v", value);
    CHECK_OK(session->Apply(op));
    return op;
  }

  // Select the specified columns of a row using a primary key, equivalent to the select statement
  // below. Return a YB read op that has been applied.
  //   select <columns...> from t where h1 = <h1> and h2 = <h2> and r1 = <r1> and r2 = <r2>;
  Result<int32_t> SelectRow(const YBSessionPtr& session, int32_t key) {
    const shared_ptr<YBqlReadOp> op = table_.NewReadOp();
    auto* const req = op->mutable_request();
    YBPartialRow *prow = op->mutable_row();
    table_.SetInt32ColumnValue(req->add_hashed_column_values(), "k", key, prow, 0);
    table_.AddColumns({"v"}, req);
    RETURN_NOT_OK(session->Apply(op));
    auto rowblock = yb::sql::RowsResult(op.get()).GetRowBlock();
    if (rowblock->row_count() == 0) {
      return STATUS_FORMAT(NotFound, "Row not found for key $0", key);
    }
    return rowblock->row(0).column(0).int32_value();
  }

  void WriteData() {
    CountDownLatch latch(1);
    {
      auto tc = std::make_shared<YBTransaction>(transaction_manager_.get_ptr(), SNAPSHOT_ISOLATION);
      auto session = std::make_shared<YBSession>(client_, false /* read_only */, tc);
      session->SetTimeout(5s);
      InsertRow(session, 1, 3);
      InsertRow(session, 2, 4);
      tc->Commit([&latch](const Status& status) {
          ASSERT_OK(status);
          latch.CountDown(1);
      });
    }
    latch.Wait();
    LOG(INFO) << "Committed";
  }

  void VerifyData() {
    auto session = client_->NewSession(true /* read_only */);
    session->SetTimeout(5s);
    auto row1 = SelectRow(session, 1);
    ASSERT_OK(row1);
    ASSERT_EQ(3, *row1);
    auto row2 = SelectRow(session, 2);
    ASSERT_OK(row2);
    ASSERT_EQ(4, *row2);
  }

  TableHandle table_;
  boost::optional<TransactionManager> transaction_manager_;
};

TEST_F(YqlTransactionTest, Simple) {
  WriteData();
  std::this_thread::sleep_for(1s); // TODO(dtxn)
  VerifyData();
  CHECK_OK(cluster_->RestartSync());
}

TEST_F(YqlTransactionTest, Cleanup) {
  WriteData();
  std::this_thread::sleep_for(1s); // TODO(dtxn)
  for (int i = 0; i != cluster_->num_tablet_servers(); ++i) {
    auto* tablet_manager = cluster_->mini_tablet_server(i)->server()->tablet_manager();
    std::vector<tablet::TabletPeerPtr> peers;
    tablet_manager->GetTabletPeers(&peers);
    for (const auto& peer : peers) {
      ASSERT_EQ(0, peer->tablet()->transaction_coordinator().test_count_transactions());
    }
  }
  VerifyData();
  CHECK_OK(cluster_->RestartSync());
}

} // namespace client
} // namespace yb