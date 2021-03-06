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

#include <cassandra.h>

#include <tuple>

#include "yb/common/ql_value.h"

#include "yb/client/client-internal.h"
#include "yb/client/client-test-util.h"
#include "yb/client/client.h"
#include "yb/client/session.h"
#include "yb/client/table_alterer.h"
#include "yb/client/table_creator.h"
#include "yb/client/table_handle.h"
#include "yb/client/transaction.h"
#include "yb/client/transaction_manager.h"
#include "yb/client/yb_op.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/strip.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/server/hybrid_clock.h"
#include "yb/server/clock.h"

#include "yb/integration-tests/external_mini_cluster-itest-base.h"
#include "yb/integration-tests/cql_test_util.h"

#include "yb/util/backoff_waiter.h"
#include "yb/util/jsonreader.h"
#include "yb/util/logging.h"
#include "yb/util/metrics.h"
#include "yb/util/random_util.h"
#include "yb/util/size_literals.h"

using namespace std::literals;

using std::string;
using std::vector;
using std::ostringstream;
using std::unique_ptr;
using std::tuple;
using std::get;

using rapidjson::Value;
using strings::Substitute;

using yb::CoarseBackoffWaiter;
using yb::YQLDatabase;
using yb::client::TableHandle;
using yb::client::TransactionManager;
using yb::client::YBTableName;
using yb::client::YBTableInfo;
using yb::client::YBqlWriteOpPtr;
using yb::client::YBSessionPtr;

METRIC_DECLARE_entity(server);
METRIC_DECLARE_histogram(handler_latency_yb_client_write_remote);
METRIC_DECLARE_histogram(handler_latency_yb_client_read_remote);
METRIC_DECLARE_histogram(handler_latency_yb_client_write_local);
METRIC_DECLARE_histogram(handler_latency_yb_client_read_local);

DECLARE_int64(external_mini_cluster_max_log_bytes);

namespace yb {

namespace util {

template<class T>
string type_name() { return "unknown"; } // COMPILATION ERROR: Specialize it for your type!
// Supported types - get type name:
template<> string type_name<string>() { return "text"; }
template<> string type_name<cass_bool_t>() { return "boolean"; }
template<> string type_name<cass_float_t>() { return "float"; }
template<> string type_name<cass_double_t>() { return "double"; }
template<> string type_name<cass_int32_t>() { return "int"; }
template<> string type_name<cass_int64_t>() { return "bigint"; }
template<> string type_name<CassandraJson>() { return "jsonb"; }

} // namespace util

//------------------------------------------------------------------------------

class CppCassandraDriverTest : public ExternalMiniClusterITestBase {
 public:
  void SetUp() override {
    ASSERT_NO_FATALS(ExternalMiniClusterITestBase::SetUp());

    LOG(INFO) << "Starting YB ExternalMiniCluster...";
    // Start up with 3 (default) tablet servers.
    ASSERT_NO_FATALS(StartCluster(ExtraTServerFlags(), ExtraMasterFlags(), 3, NumMasters()));

    std::vector<std::string> hosts;
    for (int i = 0; i < cluster_->num_tablet_servers(); ++i) {
      hosts.push_back(cluster_->tablet_server(i)->bind_host());
    }
    driver_.reset(new CppCassandraDriver(
        hosts, cluster_->tablet_server(0)->cql_rpc_port(), UsePartitionAwareRouting()));

    // Create and use default keyspace.
    session_ = ASSERT_RESULT(driver_->CreateSession());
    ASSERT_OK(session_.ExecuteQuery("CREATE KEYSPACE IF NOT EXISTS test;"));
    ASSERT_OK(session_.ExecuteQuery("USE test;"));
  }

  void SetUpCluster(ExternalMiniClusterOptions* opts) override {
    ASSERT_NO_FATALS(ExternalMiniClusterITestBase::SetUpCluster(opts));

    opts->bind_to_unique_loopback_addresses = true;
    opts->use_same_ts_ports = true;
  }

  void TearDown() override {
    ExternalMiniClusterITestBase::cluster_->AssertNoCrashes();

    // Close the session before we delete the driver.
    session_.Reset();
    driver_.reset();
    LOG(INFO) << "Stopping YB ExternalMiniCluster...";
    ExternalMiniClusterITestBase::TearDown();
  }

  virtual std::vector<std::string> ExtraTServerFlags() {
    return {};
  }

  virtual std::vector<std::string> ExtraMasterFlags() {
    return {};
  }

  virtual int NumMasters() {
    return 1;
  }

  virtual bool UsePartitionAwareRouting() {
    return true;
  }

 protected:
  unique_ptr<CppCassandraDriver> driver_;
  CassandraSession session_;
};

YB_STRONGLY_TYPED_BOOL(PKOnlyIndex);
YB_STRONGLY_TYPED_BOOL(IsUnique);
YB_STRONGLY_TYPED_BOOL(IncludeAllColumns);
YB_STRONGLY_TYPED_BOOL(UserEnforced);

class CppCassandraDriverTestIndex : public CppCassandraDriverTest {
 public:
  std::vector<std::string> ExtraTServerFlags() override {
    return {
        "--TEST_slowdown_backfill_by_ms=150",
        "--client_read_write_timeout_ms=10000",
        "--index_backfill_upperbound_for_user_enforced_txn_duration_ms=12000",
         "--yb_num_total_tablets=18",
         "--num_concurrent_backfills_allowed=1",
         "--allow_index_table_read_write=true"};
  }

  std::vector<std::string> ExtraMasterFlags() override {
    return {
        "--disable_index_backfill=false",
        "--enable_load_balancing=false",
        "--yb_num_total_tablets=18",
        "--index_backfill_rpc_timeout_ms=6000",
        "--index_backfill_rpc_max_delay_ms=1000",
        "--TEST_slowdown_backfill_alter_table_rpcs_ms=200"};
  }

  bool UsePartitionAwareRouting() override {
    // Disable partition aware routing in this test because of TSAN issue (#1837).
    // Should be reenabled when issue is fixed.
    return false;
  }

 protected:
  friend Result<IndexPermissions>
  TestBackfillCreateIndexTableSimple(CppCassandraDriverTestIndex *test);

  friend void TestBackfillIndexTable(CppCassandraDriverTestIndex* test,
                                     PKOnlyIndex is_pk_only, IsUnique is_unique,
                                     IncludeAllColumns include_primary_key,
                                     UserEnforced user_enforced);

  friend void DoTestCreateUniqueIndexWithOnlineWrites(
      CppCassandraDriverTestIndex* test, bool delete_before_insert);

  void TestUniqueIndexCommitOrder(bool commit_txn1, bool use_txn2);
};

class CppCassandraDriverTestIndexMultipleChunks
    : public CppCassandraDriverTestIndex {
 public:
  std::vector<std::string> ExtraTServerFlags() override {
    auto flags = CppCassandraDriverTestIndex::ExtraTServerFlags();
    flags.push_back("--TEST_backfill_paging_size=2");
    return flags;
  }
};

class CppCassandraDriverTestUserEnforcedIndex : public CppCassandraDriverTestIndex {
 public:
  std::vector<std::string> ExtraTServerFlags() override {
    auto flags = CppCassandraDriverTestIndex::ExtraTServerFlags();
    flags.push_back("--client_read_write_timeout_ms=10000");
    flags.push_back(
        "--index_backfill_upperbound_for_user_enforced_txn_duration_ms=12000");
    return flags;
  }
};

class CppCassandraDriverTestIndexNonResponsiveTServers : public CppCassandraDriverTestIndex {
 public:
  std::vector<std::string> ExtraMasterFlags() override {
    return {
        "--disable_index_backfill=false",
        "--enable_load_balancing=false",
        "--yb_num_total_tablets=18",
        // Really aggressive timeouts.
        "--index_backfill_rpc_max_retries=1",
        "--index_backfill_rpc_timeout_ms=1",
        "--index_backfill_rpc_max_delay_ms=1"};
  }
};

//------------------------------------------------------------------------------

template <typename... ColumnsTypes>
class TestTable {
 public:
  typedef vector<string> StringVec;
  typedef tuple<ColumnsTypes...> ColumnsTuple;

  CHECKED_STATUS CreateTable(
      CassandraSession* session, const string& table, const StringVec& columns,
      const StringVec& keys, bool transactional = false,
      const MonoDelta& timeout = MonoDelta::kZero) {
    table_name_ = table;
    column_names_ = columns;
    key_names_ = keys;

    for (string& k : key_names_) {
      TrimString(&k, "()"); // Cut parentheses if available.
    }

    auto deadline = CoarseMonoClock::now() + timeout;
    for (;;) {
      const std::string query = create_table_str(table, columns, keys, transactional);
      auto result = session->ExecuteQuery(query);
      if (result.ok() || CoarseMonoClock::now() >= deadline) {
        return result;
      }
    }
  }

  void Print(const string& prefix, const ColumnsTuple& data) const {
    LOG(INFO) << prefix << ":";

    StringVec types;
    do_get_type_names(&types, data);
    CHECK_EQ(types.size(), column_names_.size());

    StringVec values;
    do_get_values(&values, data);
    CHECK_EQ(values.size(), column_names_.size());

    for (int i = 0; i < column_names_.size(); ++i) {
      LOG(INFO) << ">     " << column_names_[i] << ' ' << types[i] << ": " << values[i];
    }
  }

  void BindInsert(CassandraStatement* statement, const ColumnsTuple& data) const {
    DoBindValues(statement, /* keys_only = */ false, /* values_first = */ false, data);
  }

  void Insert(CassandraSession* session, const ColumnsTuple& data) const {
    const string query = insert_with_bindings_str(table_name_, column_names_);
    Print("Execute: '" + query + "' with data", data);

    CassandraStatement statement(cass_statement_new(query.c_str(), column_names_.size()));
    BindInsert(&statement, data);
    ASSERT_OK(session->Execute(statement));
  }

  Result<CassandraPrepared> PrepareInsert(
      CassandraSession* session, MonoDelta timeout = MonoDelta::kZero) const {
    return session->Prepare(insert_with_bindings_str(table_name_, column_names_), timeout);
  }

  void Update(CassandraSession* session, const ColumnsTuple& data) const {
    const string query = update_with_bindings_str(table_name_, column_names_, key_names_);
    Print("Execute: '" + query + "' with data", data);

    CassandraStatement statement(cass_statement_new(query.c_str(), column_names_.size()));
    DoBindValues(&statement, /* keys_only = */ false, /* values_first = */ true, data);

    ASSERT_OK(session->Execute(statement));
  }

  void SelectOneRow(CassandraSession* session, ColumnsTuple* data) {
    const string query = select_with_bindings_str(table_name_, key_names_);
    Print("Execute: '" + query + "' with data", *data);

    CassandraStatement statement(cass_statement_new(query.c_str(), key_names_.size()));
    DoBindValues(&statement, /* keys_only = */ true, /* values_first = */ false, *data);
    *data = ASSERT_RESULT(ExecuteAndReadOneRow(session, statement));
  }

  Result<ColumnsTuple> SelectByToken(CassandraSession* session, int64_t token) {
    const string query = select_by_token_str(table_name_, key_names_);
    LOG(INFO) << "Execute: '" << query << "' with token: " << token;

    CassandraStatement statement(query, 1);
    statement.Bind(0, token);
    return ExecuteAndReadOneRow(session, statement);
  }

  Result<ColumnsTuple> ExecuteAndReadOneRow(
      CassandraSession* session, const CassandraStatement& statement) {
    ColumnsTuple data;
    RETURN_NOT_OK(session->ExecuteAndProcessOneRow(
        statement, [this, &data](const CassandraRow& row) {
          DoReadValues(row, &data);
        }));
    return data;
  }

 protected:
  // Tuple unrolling methods.
  static void get_type_names(StringVec*, const tuple<>&) {}

  template<typename T, typename... A>
  static void get_type_names(StringVec* types, const tuple<T, A...>& t) {
    types->push_back(util::type_name<T>());
    get_type_names(types, tuple<A...>());
  }

  static void do_get_type_names(StringVec* types, const ColumnsTuple& t) {
    get_type_names<ColumnsTypes...>(types, t);
  }

  template<size_t I>
  static void get_values(StringVec*, const ColumnsTuple&) {}

  template<size_t I, typename T, typename... A>
  static void get_values(StringVec* values, const ColumnsTuple& data) {
    ostringstream ss;
    ss << get<I>(data);
    values->push_back(ss.str());
    get_values<I + 1, A...>(values, data);
  }

  static void do_get_values(StringVec* values, const ColumnsTuple& data) {
    get_values<0, ColumnsTypes...>(values, data);
  }

  template<size_t I>
  typename std::enable_if<I >= std::tuple_size<ColumnsTuple>::value, void>::type
  BindValues(CassandraStatement*, size_t*, bool, bool, const ColumnsTuple&) const {}

  template<size_t I>
  typename std::enable_if<I < std::tuple_size<ColumnsTuple>::value, void>::type
  BindValues(
      CassandraStatement* statement, size_t* index, bool use_values, bool use_keys,
      const ColumnsTuple& data) const {
    const bool this_is_key = is_key(column_names_[I], key_names_);

    if (this_is_key ? use_keys : use_values) {
      statement->Bind((*index)++, get<I>(data));
    }

    BindValues<I + 1>(statement, index, use_values, use_keys, data);
  }

  void DoBindValues(
      CassandraStatement* statement, bool keys_only, bool values_first,
      const ColumnsTuple& data) const {
    size_t i = 0;
    if (keys_only) {
      BindValues<0>(statement, &i, false /* use_values */, true /* use_keys */, data);
    } else if (values_first) {
      // Bind values.
      BindValues<0>(statement, &i, true /* use_values */, false /* use_keys */, data);
      // Bind keys.
      BindValues<0>(statement, &i, false /* use_values */, true /* use_keys */, data);
    } else {
      BindValues<0>(statement, &i, true /* use_values */, true /* use_keys */, data);
    }
  }

  template<size_t I>
  typename std::enable_if<I >= std::tuple_size<ColumnsTuple>::value, void>::type
  ReadValues(const CassandraRow& row, size_t*, bool, ColumnsTuple*) const {}

  template<size_t I>
  typename std::enable_if<I < std::tuple_size<ColumnsTuple>::value, void>::type
  ReadValues(
      const CassandraRow& row, size_t* index, bool use_keys,
      ColumnsTuple* data) const {
    const bool this_is_key = is_key(column_names_[I], key_names_);

    if (this_is_key == use_keys) {
      row.Get(*index, &get<I>(*data));
      ++*index;
    }

    ReadValues<I + 1>(row, index, use_keys, data);
  }

  void DoReadValues(const CassandraRow& row, ColumnsTuple* data) const {
    size_t i = 0;
    // Read keys.
    ReadValues<0>(row, &i, true /* use_keys */, data);
    // Read values.
    ReadValues<0>(row, &i, false /* use_keys */, data);
  }

  // Strings for CQL requests.

  static string create_table_str(
      const string& table, const StringVec& columns, const StringVec& keys, bool transactional) {
    CHECK_GT(columns.size(), 0);
    CHECK_GT(keys.size(), 0);
    CHECK_GE(columns.size(), keys.size());

    StringVec types;
    do_get_type_names(&types, ColumnsTuple());
    CHECK_EQ(types.size(), columns.size());

    for (int i = 0; i < columns.size(); ++i) {
      types[i] = columns[i] + ' ' + types[i];
    }


    return Format("CREATE TABLE IF NOT EXISTS $0 ($1, PRIMARY KEY ($2))$3;",
                  table, JoinStrings(types, ", "), JoinStrings(keys, ", "),
                  transactional ? " WITH transactions = { 'enabled' : true }" : "");
  }

  static string insert_with_bindings_str(const string& table, const StringVec& columns) {
    CHECK_GT(columns.size(), 0);
    const StringVec values(columns.size(), "?");

    return "INSERT INTO " + table + " (" +
        JoinStrings(columns, ", ") + ") VALUES (" + JoinStrings(values, ", ") + ");";
  }

  static string update_with_bindings_str(
      const string& table, const StringVec& columns, const StringVec& keys) {
    CHECK_GT(columns.size(), 0);
    CHECK_GT(keys.size(), 0);
    CHECK_GE(columns.size(), keys.size());
    StringVec values, key_values;

    for (const string& col : columns) {
      (is_key(col, keys) ? key_values : values).push_back(col + " = ?");
    }

    return "UPDATE " + table + " SET " +
        JoinStrings(values, ", ") + " WHERE " + JoinStrings(key_values, ", ") + ";";
  }

  static string select_with_bindings_str(const string& table, const StringVec& keys) {
    CHECK_GT(keys.size(), 0);
    StringVec key_values = keys;

    for (string& k : key_values) {
       k += " = ?";
    }

    return "SELECT * FROM " + table + " WHERE " + JoinStrings(key_values, " AND ") + ";";
  }

  static string select_by_token_str(const string& table, const StringVec& keys) {
    CHECK_GT(keys.size(), 0);
    return "SELECT * FROM " + table + " WHERE TOKEN(" + JoinStrings(keys, ", ") + ") = ?;";
  }

  static bool is_key(const string& name, const StringVec& keys) {
    return find(keys.begin(), keys.end(), name) != keys.end();
  }

 protected:
  string table_name_;
  StringVec column_names_;
  StringVec key_names_;
};

//------------------------------------------------------------------------------

template<size_t I, class Tuple>
typename std::enable_if<I >= std::tuple_size<Tuple>::value, void>::type
ExpectEqualTuplesHelper(const Tuple&, const Tuple&) {}

template<size_t I, class Tuple>
typename std::enable_if<I < std::tuple_size<Tuple>::value, void>::type
ExpectEqualTuplesHelper(const Tuple& t1, const Tuple& t2) {
  EXPECT_EQ(get<I>(t1), get<I>(t2));
  LOG(INFO) << "COMPARE: " << get<I>(t1) << " == " << get<I>(t2);
  ExpectEqualTuplesHelper<I + 1>(t1, t2);
}

template <class Tuple>
void ExpectEqualTuples(const Tuple& t1, const Tuple& t2) {
  ExpectEqualTuplesHelper<0>(t1, t2);
}

void LogResult(const CassandraResult& result) {
  auto iterator = result.CreateIterator();
  int i = 0;
  while (iterator.Next()) {
    ++i;
    std::string line;
    auto row = iterator.Row();
    auto row_iterator = row.CreateIterator();
    bool first = true;
    while (row_iterator.Next()) {
      if (first) {
        first = false;
      } else {
        line += ", ";
      }
      line += row_iterator.Value().ToString();
    }
    LOG(INFO) << i << ") " << line;
  }
}

//------------------------------------------------------------------------------

TEST_F(CppCassandraDriverTest, TestBasicTypes) {
  typedef TestTable<
      string, cass_bool_t, cass_float_t, cass_double_t, cass_int32_t, cass_int64_t, string> MyTable;
  MyTable table;
  ASSERT_OK(table.CreateTable(
      &session_, "test.basic", {"key", "bln", "flt", "dbl", "i32", "i64", "str"}, {"key"}));

  const MyTable::ColumnsTuple input("test", cass_true, 11.01f, 22.002, 3, 4, "text");
  table.Insert(&session_, input);

  MyTable::ColumnsTuple output("test", cass_false, 0.f, 0., 0, 0, "");
  table.SelectOneRow(&session_, &output);
  table.Print("RESULT OUTPUT", output);

  LOG(INFO) << "Checking selected values...";
  ExpectEqualTuples(input, output);
}

TEST_F(CppCassandraDriverTest, TestJsonBType) {
  typedef TestTable<string, CassandraJson> MyTable;
  MyTable table;
  ASSERT_OK(table.CreateTable(&session_, "test.json", {"key", "json"}, {"key"}));

  MyTable::ColumnsTuple input("test", CassandraJson("{\"a\":1}"));
  table.Insert(&session_, input);

  MyTable::ColumnsTuple output("test", CassandraJson(""));
  table.SelectOneRow(&session_, &output);
  table.Print("RESULT OUTPUT", output);

  LOG(INFO) << "Checking selected values...";
  ExpectEqualTuples(input, output);

  get<1>(input) = CassandraJson("{\"b\":1}"); // 'json'
  table.Update(&session_, input);

  MyTable::ColumnsTuple updated_output("test", CassandraJson(""));
  table.SelectOneRow(&session_, &updated_output);
  table.Print("UPDATED RESULT OUTPUT", updated_output);

  LOG(INFO) << "Checking selected values...";
  ExpectEqualTuples(input, updated_output);
}

void VerifyLongJson(const string& json) {
  // Parse JSON.
  JsonReader r(json);
  ASSERT_OK(r.Init());
  const Value* json_obj = nullptr;
  EXPECT_OK(r.ExtractObject(r.root(), NULL, &json_obj));
  EXPECT_EQ(rapidjson::kObjectType, CHECK_NOTNULL(json_obj)->GetType());

  EXPECT_TRUE(json_obj->HasMember("b"));
  EXPECT_EQ(rapidjson::kNumberType, (*json_obj)["b"].GetType());
  EXPECT_EQ(1, (*json_obj)["b"].GetInt());

  EXPECT_TRUE(json_obj->HasMember("a1"));
  EXPECT_EQ(rapidjson::kArrayType, (*json_obj)["a1"].GetType());
  const Value::ConstArray arr = (*json_obj)["a1"].GetArray();

  EXPECT_EQ(rapidjson::kNumberType, arr[2].GetType());
  EXPECT_EQ(3., arr[2].GetDouble());

  EXPECT_EQ(rapidjson::kFalseType, arr[3].GetType());
  EXPECT_EQ(false, arr[3].GetBool());

  EXPECT_EQ(rapidjson::kTrueType, arr[4].GetType());
  EXPECT_EQ(true, arr[4].GetBool());

  EXPECT_EQ(rapidjson::kObjectType, arr[5].GetType());
  const Value::ConstObject obj = arr[5].GetObject();
  EXPECT_TRUE(obj.HasMember("k2"));
  EXPECT_EQ(rapidjson::kArrayType, obj["k2"].GetType());
  EXPECT_EQ(rapidjson::kNumberType, obj["k2"].GetArray()[1].GetType());
  EXPECT_EQ(200, obj["k2"].GetArray()[1].GetInt());

  EXPECT_TRUE(json_obj->HasMember("a"));
  EXPECT_EQ(rapidjson::kObjectType, (*json_obj)["a"].GetType());
  const Value::ConstObject obj_a = (*json_obj)["a"].GetObject();

  EXPECT_TRUE(obj_a.HasMember("q"));
  EXPECT_EQ(rapidjson::kObjectType, obj_a["q"].GetType());
  const Value::ConstObject obj_q = obj_a["q"].GetObject();
  EXPECT_TRUE(obj_q.HasMember("s"));
  EXPECT_EQ(rapidjson::kNumberType, obj_q["s"].GetType());
  EXPECT_EQ(2147483647, obj_q["s"].GetInt());

  EXPECT_TRUE(obj_a.HasMember("f"));
  EXPECT_EQ(rapidjson::kStringType, obj_a["f"].GetType());
  EXPECT_EQ("hello", string(obj_a["f"].GetString()));
}

TEST_F(CppCassandraDriverTest, TestLongJson) {
  const string long_json =
      "{ "
        "\"b\" : 1,"
        "\"a2\" : {},"
        "\"a3\" : \"\","
        "\"a1\" : [1, 2, 3.0, false, true, { \"k1\" : 1, \"k2\" : [100, 200, 300], \"k3\" : true}],"
        "\"a\" :"
        "{"
          "\"d\" : true,"
          "\"q\" :"
            "{"
              "\"p\" : 4294967295,"
              "\"r\" : -2147483648,"
              "\"s\" : 2147483647"
            "},"
          "\"g\" : -100,"
          "\"c\" : false,"
          "\"f\" : \"hello\","
          "\"x\" : 2.0,"
          "\"y\" : 9223372036854775807,"
          "\"z\" : -9223372036854775808,"
          "\"u\" : 18446744073709551615,"
          "\"l\" : 2147483647.123123e+75,"
          "\"e\" : null"
        "}"
      "}";


  typedef TestTable<string, CassandraJson> MyTable;
  MyTable table;
  ASSERT_OK(table.CreateTable(&session_, "basic", {"key", "json"}, {"key"}));

  MyTable::ColumnsTuple input("test", CassandraJson(long_json));
  table.Insert(&session_, input);

  ASSERT_OK(session_.ExecuteQuery(
      "INSERT INTO basic(key, json) values ('test0', '" + long_json + "');"));
  ASSERT_OK(session_.ExecuteQuery(
      "INSERT INTO basic(key, json) values ('test1', '{ \"a\" : 1 }');"));
  ASSERT_OK(session_.ExecuteQuery("INSERT INTO basic(key, json) values ('test2', '\"abc\"');"));
  ASSERT_OK(session_.ExecuteQuery("INSERT INTO basic(key, json) values ('test3', '3');"));
  ASSERT_OK(session_.ExecuteQuery("INSERT INTO basic(key, json) values ('test4', 'true');"));
  ASSERT_OK(session_.ExecuteQuery("INSERT INTO basic(key, json) values ('test5', 'false');"));
  ASSERT_OK(session_.ExecuteQuery("INSERT INTO basic(key, json) values ('test6', 'null');"));
  ASSERT_OK(session_.ExecuteQuery("INSERT INTO basic(key, json) values ('test7', '2.0');"));
  ASSERT_OK(session_.ExecuteQuery("INSERT INTO basic(key, json) values ('test8', '{\"b\" : 1}');"));

  for (const string& key : {"test", "test0"} ) {
    MyTable::ColumnsTuple output(key, CassandraJson(""));
    table.SelectOneRow(&session_, &output);
    table.Print("RESULT OUTPUT", output);

    LOG(INFO) << "Checking selected JSON object for key=" << key;
    const string json = get<1>(output).value(); // 'json'

    ASSERT_EQ(json,
        "{"
          "\"a\":"
          "{"
            "\"c\":false,"
            "\"d\":true,"
            "\"e\":null,"
            "\"f\":\"hello\","
            "\"g\":-100,"
            "\"l\":2.147483647123123e84,"
            "\"q\":"
            "{"
              "\"p\":4294967295,"
              "\"r\":-2147483648,"
              "\"s\":2147483647"
            "},"
            "\"u\":18446744073709551615,"
            "\"x\":2.0,"
            "\"y\":9223372036854775807,"
            "\"z\":-9223372036854775808"
          "},"
          "\"a1\":[1,2,3.0,false,true,{\"k1\":1,\"k2\":[100,200,300],\"k3\":true}],"
          "\"a2\":{},"
          "\"a3\":\"\","
          "\"b\":1"
        "}");

    VerifyLongJson(json);
  }
}

Result<IndexPermissions> GetIndexPermissions(
    client::YBClient* client, const YBTableName& table_name, const YBTableName& index_table_name) {
  Result<YBTableInfo> table_info = client->GetYBTableInfo(table_name);
  if (!table_info) {
    RETURN_NOT_OK_PREPEND(table_info.status(),
                          "Unable to fetch table info for the main table " +
                              table_name.ToString());
  }
  Result<YBTableInfo> index_table_info = client->GetYBTableInfo(index_table_name);
  if (!index_table_info) {
    RETURN_NOT_OK_PREPEND(index_table_info.status(),
        "Unable to fetch table info for the index table " + index_table_name.ToString());
  }

  IndexInfoPB index_info_pb;
  table_info->index_map[index_table_info->table_id].ToPB(&index_info_pb);
  YB_LOG_EVERY_N_SECS(INFO, 1) << "The index info for " << index_table_name.ToString()
                               << " is " << yb::ToString(index_info_pb);

  if (!index_info_pb.has_index_permissions()) {
    return STATUS(NotFound, "IndexPermissions not found in index info.");
  }

  return index_info_pb.index_permissions();
}

IndexPermissions WaitUntilIndexPermissionIsAtLeast(
    client::YBClient* client, const YBTableName& table_name, const YBTableName& index_table_name,
    IndexPermissions min_permission, bool exponential_backoff = true) {
  CoarseBackoffWaiter waiter(CoarseMonoClock::Now() + 90s,
                             (exponential_backoff ? CoarseMonoClock::Duration::max() : 50ms));
  Result<IndexPermissions> result = GetIndexPermissions(client, table_name, index_table_name);
  while (!result || *result < min_permission) {
    YB_LOG_EVERY_N_SECS(INFO, 1)
        << "Waiting since GetIndexPermissions returned "
        << (result ? IndexPermissions_Name(*result) : result.status().ToString());
    waiter.Wait();
    result = GetIndexPermissions(client, table_name, index_table_name);
  }
  return *result;
}

TEST_F_EX(CppCassandraDriverTest, TestCreateIndex, CppCassandraDriverTestIndex) {
  IndexPermissions perm =
      ASSERT_RESULT(TestBackfillCreateIndexTableSimple(this));
  ASSERT_EQ(perm, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
}

TEST_F_EX(CppCassandraDriverTest, TestCreateIndexSlowTServer,
          CppCassandraDriverTestIndexNonResponsiveTServers) {
  IndexPermissions perm =
      ASSERT_RESULT(TestBackfillCreateIndexTableSimple(this));
  ASSERT_EQ(perm, IndexPermissions::INDEX_PERM_INDEX_UNUSED);
}

Result<IndexPermissions>
TestBackfillCreateIndexTableSimple(CppCassandraDriverTestIndex *test) {
  TestTable<cass_int32_t, string> table;
  RETURN_NOT_OK(table.CreateTable(&test->session_, "test.test_table",
                                  {"k", "v"}, {"(k)"}, true, 60s));

  LOG(INFO) << "Inserting one row";
  RETURN_NOT_OK(test->session_.ExecuteQuery(
      "insert into test_table (k, v) values (1, 'one');"));
  LOG(INFO) << "Creating index";
  WARN_NOT_OK(test->session_.ExecuteQuery(
                  "create index test_table_index_by_v on test_table (v);"),
              "create-index failed.");

  LOG(INFO) << "Inserting one row";
  RETURN_NOT_OK(test->session_.ExecuteQuery(
      "insert into test_table (k, v) values (2, 'two');"));
  RETURN_NOT_OK(test->session_.ExecuteQuery(
      "insert into test_table (k, v) values (3, 'three');"));

  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "test_table");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace, "test_table_index_by_v");
  return WaitUntilIndexPermissionIsAtLeast(
      test->client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
}

Result<int64_t> GetTableSize(CassandraSession *session, const std::string& table_name) {
  int64_t size = 0;
  RETURN_NOT_OK(session->ExecuteAndProcessOneRow(
      Format("select count(*) from $0;", table_name),
      [&size](const CassandraRow& row) {
        size = row.Value(0).As<int64_t>();
      }));
  return size;
}

void TestBackfillIndexTable(
    CppCassandraDriverTestIndex* test, PKOnlyIndex is_pk_only,
    IsUnique is_unique = IsUnique::kFalse,
    IncludeAllColumns include_primary_key = IncludeAllColumns::kFalse,
    UserEnforced user_enforced = UserEnforced::kFalse) {
  constexpr int kLoops = 3;
  constexpr int kBatchSize = 10;
  constexpr int kNumBatches = 10;
  constexpr int kExpectedCount = kBatchSize * kNumBatches;

  typedef TestTable<string, string, string> MyTable;
  typedef MyTable::ColumnsTuple ColumnsType;
  MyTable table;
  ASSERT_OK(table.CreateTable(&test->session_, "test.key_value",
                              {"key1", "key2", "value"}, {"(key1, key2)"},
                              !user_enforced, 60s));


  LOG(INFO) << "Creating index";
  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "key_value");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace, "index_by_value");

  std::vector<CassandraFuture> futures;

  int num_failures = 0;
  CassandraFuture create_index_future(nullptr);
  for (int loop = 1; loop <= kLoops; ++loop) {
    for (int batch_idx = 0; batch_idx != kNumBatches; ++batch_idx) {
      CassandraBatch batch(CassBatchType::CASS_BATCH_TYPE_LOGGED);
      auto prepared = table.PrepareInsert(&test->session_);
      if (!prepared.ok()) {
        // Prepare could be failed because cluster has heavy load.
        // It is ok to just retry in this case, because we check that process did not crash.
        continue;
      }
      for (int i = 0; i != kBatchSize; ++i) {
        const int key = batch_idx * kBatchSize + i;
        // For non-unique tests, the value will be of the form v-l0xx where l is
        // the loop number, and xx is the key.
        // For unique index tests, the value will be a permutation of
        //  1 .. kExpectedCount; or -1 .. -kExpectedCount for odd and even
        //  loops.
        const int value =
            (is_unique ? (loop % 2 ? 1 : -1) *
                             ((loop * 1000 + key) % kExpectedCount + 1)
                       : loop * 1000 + key);
        ColumnsType tuple(Format("k-$0", key), Format("k-$0", key),
                          Format("v-$0", value));
        auto statement = prepared->Bind();
        table.BindInsert(&statement, tuple);
        batch.Add(&statement);
      }
      futures.push_back(test->session_.SubmitBatch(batch));
    }

    // For unique index tests, we want to make sure each loop of writes is
    // complete before issuing the next one. For non-unique index tests,
    // we only wait for the writes to persist before issuing the
    // create index command.
    if (is_unique || loop == 2) {
      // Let us make sure that the writes so far have persisted.
      for (auto& future : futures) {
        if (!future.Wait().ok()) {
          num_failures++;
        }
      }
      futures.clear();
    }

    // At the end of the second loop, we will issue the create index.
    // The remaining loop(s) of writes will be concurrent with the create index.
    if (loop == 2) {
      create_index_future = test->session_.ExecuteGetFuture(
          Format("create $0 index index_by_value on test.key_value ($1) $2 $3;",
                 (is_unique ? "unique" : ""), (is_pk_only ? "key2" : "value"),
                 (include_primary_key ? "include (key1, key2, value)" : " "),
                 (user_enforced ? "with transactions = { 'enabled' : false,"
                                  "'consistency_level' : 'user_enforced' }"
                                : "")));
    }
  }

  for (auto& future : futures) {
    if (!future.Wait().ok()) {
      num_failures++;
    }
  }
  if (num_failures > 0) {
    LOG(INFO) << num_failures << " write batches failed.";
  }

  // It is fine for user enforced create index to timeout because
  // index_backfill_upperbound_for_user_enforced_txn_duration_ms is longer than
  // client_read_write_timeout_ms
  auto s = create_index_future.Wait();
  WARN_NOT_OK(s, "Create index failed.");

  IndexPermissions perm = WaitUntilIndexPermissionIsAtLeast(
      test->client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  ASSERT_TRUE(perm == IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);

  auto main_table_size = ASSERT_RESULT(GetTableSize(&test->session_, "key_value"));
  auto index_table_size = ASSERT_RESULT(GetTableSize(&test->session_, "index_by_value"));

  EXPECT_GE(main_table_size, kExpectedCount - kBatchSize * num_failures);
  EXPECT_LE(main_table_size, kExpectedCount + kBatchSize * num_failures);
  EXPECT_GE(index_table_size, kExpectedCount - kBatchSize * num_failures);
  EXPECT_LE(index_table_size, kExpectedCount + kBatchSize * num_failures);
  if (!user_enforced || num_failures == 0) {
    EXPECT_EQ(main_table_size, index_table_size);
  }
}

TEST_F_EX(CppCassandraDriverTest, TestTableCreateIndex, CppCassandraDriverTestIndex) {
  TestBackfillIndexTable(this, PKOnlyIndex::kFalse, IsUnique::kFalse,
                         IncludeAllColumns::kFalse);
}

TEST_F_EX(CppCassandraDriverTest, TestTableCreateIndexPKOnly, CppCassandraDriverTestIndex) {
  TestBackfillIndexTable(this, PKOnlyIndex::kTrue, IsUnique::kFalse,
                         IncludeAllColumns::kFalse);
}

TEST_F_EX(CppCassandraDriverTest, TestTableCreateIndexCovered, CppCassandraDriverTestIndex) {
  TestBackfillIndexTable(this, PKOnlyIndex::kFalse, IsUnique::kFalse,
                         IncludeAllColumns::kTrue);
}

TEST_F_EX(CppCassandraDriverTest, TestTableCreateIndexUserEnforced,
          CppCassandraDriverTestUserEnforcedIndex) {
  TestBackfillIndexTable(this, PKOnlyIndex::kFalse, IsUnique::kFalse,
                         IncludeAllColumns::kTrue, UserEnforced::kTrue);
}

TEST_F_EX(CppCassandraDriverTest, TestTableCreateUniqueIndex,
          CppCassandraDriverTestIndex) {
  TestBackfillIndexTable(this, PKOnlyIndex::kFalse, IsUnique::kTrue,
                         IncludeAllColumns::kFalse);
}

TEST_F_EX(CppCassandraDriverTest, TestTableCreateUniqueIndexCovered,
          CppCassandraDriverTestIndex) {
  TestBackfillIndexTable(this, PKOnlyIndex::kFalse, IsUnique::kTrue,
                         IncludeAllColumns::kTrue);
}

TEST_F_EX(CppCassandraDriverTest, TestTableCreateUniqueIndexUserEnforced,
          CppCassandraDriverTestUserEnforcedIndex) {
  TestBackfillIndexTable(this, PKOnlyIndex::kFalse, IsUnique::kTrue,
                         IncludeAllColumns::kTrue, UserEnforced::kTrue);
}

TEST_F_EX(CppCassandraDriverTest, TestCreateJsonbIndex,
          CppCassandraDriverTestIndex) {
  TestTable<cass_int32_t, CassandraJson> table;
  ASSERT_OK(table.CreateTable(&session_, "test.test_table", {"k", "v"}, {"(k)"},
                              true, 60s));

  LOG(INFO) << "Inserting three rows";
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (1, '{\"f1\": \"one\", \"f2\": \"one\"}');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (2, '{\"f1\": \"two\", \"f2\": \"two\"}');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (3, '{\"f1\": \"three\", \"f2\": \"three\"}');"));

  LOG(INFO) << "Creating index";
  auto s = session_.ExecuteQuery(
      "create unique index test_table_index_by_v_f1 on test_table (v->>'f1');");
  ASSERT_TRUE(s.ok() || s.IsTimedOut());
  WARN_NOT_OK(s, "Create index command failed. " + s.ToString());

  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "test_table");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace,
                                     "test_table_index_by_v_f1");
  IndexPermissions perm = WaitUntilIndexPermissionIsAtLeast(
      client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  ASSERT_TRUE(perm == IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);

  auto main_table_size =
      ASSERT_RESULT(GetTableSize(&session_, "test_table"));
  auto index_table_size =
      ASSERT_RESULT(GetTableSize(&session_, "test_table_index_by_v_f1"));
  ASSERT_EQ(main_table_size, index_table_size);
}

TEST_F_EX(CppCassandraDriverTest, TestCreateUniqueIndexPasses,
          CppCassandraDriverTestIndex) {
  TestTable<cass_int32_t, string> table;
  ASSERT_OK(table.CreateTable(&session_, "test.test_table", {"k", "v"}, {"(k)"},
                              true, 60s));

  LOG(INFO) << "Inserting three rows";
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (1, 'one');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (2, 'two');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (3, 'three');"));

  LOG(INFO) << "Creating index";
  auto s = session_.ExecuteQuery(
      "create unique index test_table_index_by_v on test_table (v);");
  ASSERT_TRUE(s.ok() || s.IsTimedOut());
  WARN_NOT_OK(s, "Create index command failed. " + s.ToString());

  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "test_table");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace,
                                     "test_table_index_by_v");
  IndexPermissions perm = WaitUntilIndexPermissionIsAtLeast(
      client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  ASSERT_TRUE(perm == IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);

  LOG(INFO) << "Inserting more rows -- collisions will be detected.";
  ASSERT_TRUE(!session_
                   .ExecuteGetFuture(
                       "insert into test_table (k, v) values (-1, 'one');")
                   .Wait()
                   .ok());
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (4, 'four');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (5, 'five');"));
  ASSERT_TRUE(!session_
                   .ExecuteGetFuture(
                       "insert into test_table (k, v) values (-4, 'four');")
                   .Wait()
                   .ok());
}

TEST_F_EX(CppCassandraDriverTest, TestCreateUniqueIndexIntent,
          CppCassandraDriverTestIndex) {
  TestTable<cass_int32_t, cass_int32_t> table;
  ASSERT_OK(table.CreateTable(&session_, "test.test_table", {"k", "v"}, {"(k)"},
                              true, 60s));

  constexpr int kNumRows = 10;
  LOG(INFO) << "Inserting " << kNumRows << " rows";
  for (int i = 1; i <= kNumRows; i++) {
    ASSERT_OK(session_.ExecuteQuery(
        Substitute("insert into test_table (k, v) values ($0, $0);", i)));
  }

  LOG(INFO) << "Creating index";
  auto session2 = CHECK_RESULT(driver_->CreateSession());
  ASSERT_OK(session2.ExecuteQuery("USE test;"));
  CassandraFuture create_index_future = session2.ExecuteGetFuture(
      "create unique index test_table_index_by_v on test_table (v);");

  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "test_table");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace,
                                     "test_table_index_by_v");
  IndexPermissions perm = WaitUntilIndexPermissionIsAtLeast(
      client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_WRITE_AND_DELETE, false);
  if (perm != IndexPermissions::INDEX_PERM_WRITE_AND_DELETE) {
    LOG(WARNING) << "IndexPermissions is already past WRITE_AND_DELETE. "
                 << "This run of the test may not actually be doing anything "
                    "non-trivial.";
  }

  const size_t kSleepTimeMs = 20;
  LOG(INFO) << "Inserting " << kNumRows / 2 << " rows again.";
  for (int i = 1; i < kNumRows / 2; i++) {
    if (session_
            .ExecuteQuery(Substitute("delete from test_table where k=$0;", i))
            .ok()) {
      WARN_NOT_OK(session_.ExecuteQuery(Substitute(
                      "insert into test_table (k, v) values ($0, $0);", i)),
                  "Overwrite failed");
      SleepFor(MonoDelta::FromMilliseconds(kSleepTimeMs));
    } else {
      LOG(ERROR) << "Deleting & Inserting failed for " << i;
    }
  }

  perm = WaitUntilIndexPermissionIsAtLeast(
      client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_DO_BACKFILL, false);
  if (perm != IndexPermissions::INDEX_PERM_DO_BACKFILL) {
    LOG(WARNING) << "IndexPermissions already past DO_BACKFILL";
  }

  LOG(INFO) << "Inserting " << kNumRows / 2 << " more rows again.";
  for (int i = kNumRows / 2; i <= kNumRows; i++) {
    if (session_
            .ExecuteQuery(Substitute("delete from test_table where k=$0;", i))
            .ok()) {
      WARN_NOT_OK(session_.ExecuteQuery(Substitute(
                      "insert into test_table (k, v) values (-$0, $0);", i)),
                  "Overwrite failed");
      SleepFor(MonoDelta::FromMilliseconds(kSleepTimeMs));
    } else {
      LOG(ERROR) << "Deleting & Inserting failed for " << i;
    }
  }

  LOG(INFO) << "Waited on the Create Index to finish. Status  = "
            << create_index_future.Wait();

  perm = WaitUntilIndexPermissionIsAtLeast(
      client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  ASSERT_EQ(perm, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
}

TEST_F_EX(CppCassandraDriverTest, TestCreateUniqueIndexPassesManyWrites,
          CppCassandraDriverTestIndex) {
  TestTable<cass_int32_t, string> table;
  ASSERT_OK(table.CreateTable(&session_, "test.test_table", {"k", "v"}, {"(k)"},
                              true, 60s));

  constexpr int kNumRows = 100;
  LOG(INFO) << "Inserting " << kNumRows << " rows";
  for (int i = 1; i <= kNumRows; i++) {
    ASSERT_OK(session_.ExecuteQuery(
        Substitute("insert into test_table (k, v) values ($0, 'v-$0');", i)));
  }

  LOG(INFO) << "Creating index";
  auto session2 = CHECK_RESULT(driver_->CreateSession());
  ASSERT_OK(session2.ExecuteQuery("USE test;"));
  CassandraFuture create_index_future = session2.ExecuteGetFuture(
      "create unique index test_table_index_by_v on test_table (v);");

  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "test_table");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace,
                                     "test_table_index_by_v");
  IndexPermissions perm = WaitUntilIndexPermissionIsAtLeast(
      client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_WRITE_AND_DELETE, false);
  if (perm != IndexPermissions::INDEX_PERM_WRITE_AND_DELETE) {
    LOG(WARNING) << "IndexPermissions is already past WRITE_AND_DELETE. "
                 << "This run of the test may not actually be doing anything "
                    "non-trivial.";
  }

  const size_t kSleepTimeMs = 20;
  LOG(INFO) << "Inserting " << kNumRows / 2 << " rows again.";
  for (int i = 1; i < kNumRows / 2; i++) {
    if (session_
            .ExecuteQuery(Substitute("delete from test_table where k=$0;", i))
            .ok()) {
      WARN_NOT_OK(
          session_.ExecuteQuery(Substitute(
              "insert into test_table (k, v) values (-$0, 'v-$0');", i)),
          "Overwrite failed");
      SleepFor(MonoDelta::FromMilliseconds(kSleepTimeMs));
    } else {
      LOG(ERROR) << "Deleting & Inserting failed for " << i;
    }
  }

  perm = WaitUntilIndexPermissionIsAtLeast(
      client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_DO_BACKFILL, false);
  if (perm != IndexPermissions::INDEX_PERM_DO_BACKFILL) {
    LOG(WARNING) << "IndexPermissions already past DO_BACKFILL";
  }

  LOG(INFO) << "Inserting " << kNumRows / 2 << " more rows again.";
  for (int i = kNumRows / 2; i <= kNumRows; i++) {
    if (session_
            .ExecuteQuery(Substitute("delete from test_table where k=$0;", i))
            .ok()) {
      WARN_NOT_OK(
          session_.ExecuteQuery(Substitute(
              "insert into test_table (k, v) values (-$0, 'v-$0');", i)),
          "Overwrite failed");
      SleepFor(MonoDelta::FromMilliseconds(kSleepTimeMs));
    } else {
      LOG(ERROR) << "Deleting & Inserting failed for " << i;
    }
  }

  LOG(INFO) << "Waited on the Create Index to finish. Status  = "
            << create_index_future.Wait();

  perm = WaitUntilIndexPermissionIsAtLeast(
      client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  ASSERT_EQ(perm, IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
}

TEST_F_EX(CppCassandraDriverTest, TestCreateIdxTripleCollisionTest,
          CppCassandraDriverTestIndex) {
  TestTable<cass_int32_t, string> table;
  ASSERT_OK(table.CreateTable(&session_, "test.test_table", {"k", "v"}, {"(k)"},
                              true, 60s));

  ASSERT_OK(
      session_.ExecuteQuery("insert into test_table (k, v) values (1, 'a')"));
  ASSERT_OK(
      session_.ExecuteQuery("insert into test_table (k, v) values (3, 'a')"));
  ASSERT_OK(
      session_.ExecuteQuery("insert into test_table (k, v) values (4, 'a')"));

  LOG(INFO) << "Creating index";
  // session_.ExecuteQuery("create unique index test_table_index_by_v on
  // test_table (v);");
  auto session2 = CHECK_RESULT(driver_->CreateSession());
  ASSERT_OK(session2.ExecuteQuery("USE test;"));
  CassandraFuture create_index_future = session2.ExecuteGetFuture(
      "create unique index test_table_index_by_v on test_table (v);");

  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "test_table");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace,
                                     "test_table_index_by_v");
  {
    IndexPermissions perm = WaitUntilIndexPermissionIsAtLeast(
        client_.get(), table_name, index_table_name,
        IndexPermissions::INDEX_PERM_DELETE_ONLY, false);
    EXPECT_EQ(perm, IndexPermissions::INDEX_PERM_DELETE_ONLY);
  }

  CoarseBackoffWaiter waiter(CoarseMonoClock::Now() + 90s,
                             CoarseMonoClock::Duration::max());
  auto res = session_.ExecuteQuery("DELETE from test_table WHERE k=4");
  LOG(INFO) << "Got " << yb::ToString(res);
  while (!res.ok()) {
    waiter.Wait();
    res = session_.ExecuteQuery("DELETE from test_table WHERE k=4");
    LOG(INFO) << "Got " << yb::ToString(res);
  }

  LOG(INFO) << "Waited on the Create Index to finish. Status  = "
            << create_index_future.Wait();
  {
    IndexPermissions perm = WaitUntilIndexPermissionIsAtLeast(
        client_.get(), table_name, index_table_name, IndexPermissions::INDEX_PERM_INDEX_UNUSED,
        false);
    ASSERT_EQ(perm, IndexPermissions::INDEX_PERM_INDEX_UNUSED);
  }
}

TEST_F_EX(CppCassandraDriverTest, TestCreateUniqueIndexFails,
          CppCassandraDriverTestIndex) {
  TestTable<cass_int32_t, string> table;
  ASSERT_OK(table.CreateTable(&session_, "test.test_table", {"k", "v"}, {"(k)"},
                              true, 60s));

  LOG(INFO) << "Inserting three rows";
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (1, 'one');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (2, 'two');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (3, 'three');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (-2, 'two');"));
  LOG(INFO) << "Creating index";

  auto s = session_.ExecuteQuery(
      "create unique index test_table_index_by_v on test_table (v);");
  ASSERT_TRUE(s.ok() || s.IsTimedOut());
  WARN_NOT_OK(s, "Create index command failed. " + s.ToString());

  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "test_table");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace,
                                     "test_table_index_by_v");
  IndexPermissions perm = WaitUntilIndexPermissionIsAtLeast(
      client_.get(), table_name, index_table_name, IndexPermissions::INDEX_PERM_INDEX_UNUSED);
  ASSERT_EQ(perm, IndexPermissions::INDEX_PERM_INDEX_UNUSED);

  LOG(INFO)
      << "Inserting more rows -- No collision checking for a failed index.";
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (-1, 'one');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (-3, 'three');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (4, 'four');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (-4, 'four');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (5, 'five');"));
  ASSERT_OK(session_.ExecuteQuery(
      "insert into test_table (k, v) values (-5, 'five');"));
}

TEST_F_EX(CppCassandraDriverTest, TestCreateUniqueIndexWithOnlineWriteFails,
          CppCassandraDriverTestIndex) {
  DoTestCreateUniqueIndexWithOnlineWrites(this,
                                          /* delete_before_insert */ false);
}

TEST_F_EX(CppCassandraDriverTest, TestCreateUniqueIndexWithOnlineWriteSuccess,
          CppCassandraDriverTestIndex) {
  DoTestCreateUniqueIndexWithOnlineWrites(this,
                                          /* delete_before_insert */ true);
}

void DoTestCreateUniqueIndexWithOnlineWrites(CppCassandraDriverTestIndex* test,
                                             bool delete_before_insert) {
  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "test_table");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace,
                                     "test_table_index_by_v");
  IndexInfoPB index_info_pb;
  YBTableInfo index_table_info;

  TestTable<cass_int32_t, string> table;
  ASSERT_OK(table.CreateTable(&test->session_, "test.test_table", {"k", "v"},
                              {"(k)"}, true, 60s));

  LOG(INFO) << "Inserting three rows";
  ASSERT_OK(test->session_.ExecuteQuery(
      "insert into test_table (k, v) values (1, 'one');"));
  ASSERT_OK(test->session_.ExecuteQuery(
      "insert into test_table (k, v) values (2, 'two');"));
  ASSERT_OK(test->session_.ExecuteQuery(
      "insert into test_table (k, v) values (3, 'three');"));
  LOG(INFO) << "Creating index";

  bool create_index_failed = false;
  bool duplicate_insert_failed = false;
  {
    auto session2 = CHECK_RESULT(test->driver_->CreateSession());
    ASSERT_OK(session2.ExecuteQuery("USE test;"));

    CassandraFuture create_index_future = session2.ExecuteGetFuture(
        "create unique index test_table_index_by_v on test_table (v);");

    auto session3 = CHECK_RESULT(test->driver_->CreateSession());
    ASSERT_OK(session3.ExecuteQuery("USE test;"));
    WaitUntilIndexPermissionIsAtLeast(
        test->client_.get(), table_name, index_table_name,
        IndexPermissions::INDEX_PERM_WRITE_AND_DELETE);
    CoarseBackoffWaiter waiter(CoarseMonoClock::Now() + 90s,
                               CoarseMonoClock::Duration::max());
    if (delete_before_insert) {
      while (true) {
        auto res = session3
                       .ExecuteGetFuture(
                           "update test_table set v = 'foo' where  k = 2;")
                       .Wait();
        LOG(INFO) << "Got " << yb::ToString(res);
        if (res.ok()) {
          break;
        }
        waiter.Wait();
      }
      LOG(INFO) << "Successfully deleted the old value before inserting the "
                   "duplicate value";
    }
    int retries = 0;
    const int kMaxRetries = 12;
    Status res;
    while (++retries < kMaxRetries) {
      res = session3.ExecuteGetFuture(
                        "insert into test_table (k, v) values (-2, 'two');")
                    .Wait();
      LOG(INFO) << "Got " << yb::ToString(res);
      if (res.ok()) {
        break;
      }
      waiter.Wait();
    }
    duplicate_insert_failed = !res.ok();
    if (!duplicate_insert_failed) {
      LOG(INFO) << "Successfully inserted the duplicate value";
    } else {
      LOG(ERROR) << "Giving up on inserting the duplicate value after "
                 << kMaxRetries << " tries.";
    }

    LOG(INFO) << "Waited on the Create Index to finish. Status  = "
              << create_index_future.Wait();
  }

  IndexPermissions perm = WaitUntilIndexPermissionIsAtLeast(
      test->client_.get(), table_name, index_table_name,
      IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);

  create_index_failed = (perm > IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  LOG(INFO) << "create_index_failed  = " << create_index_failed
            << ", duplicate_insert_failed = " << duplicate_insert_failed;

  auto main_table_size =
      ASSERT_RESULT(GetTableSize(&test->session_, "test_table"));
  auto index_table_size =
      ASSERT_RESULT(GetTableSize(&test->session_, "test_table_index_by_v"));

  if (!create_index_failed) {
    EXPECT_EQ(main_table_size, index_table_size);
  } else {
    LOG(INFO) << "create index failed. "
              << "main_table_size " << main_table_size
              << " is allowed to differ from "
              << "index_table_size " << index_table_size;
  }
  if (delete_before_insert) {
    // Expect both the create index, and the duplicate insert to succeed.
    ASSERT_TRUE(!create_index_failed && !duplicate_insert_failed);
  } else {
    // Expect exactly one of create index or the duplicate insert to succeed.
    ASSERT_TRUE((create_index_failed && !duplicate_insert_failed) ||
                (!create_index_failed && duplicate_insert_failed));
  }
}

TEST_F_EX(CppCassandraDriverTest, TestTableBackfillInChunks,
          CppCassandraDriverTestIndexMultipleChunks) {
  TestBackfillIndexTable(this, PKOnlyIndex::kFalse, IsUnique::kFalse,
                         IncludeAllColumns::kTrue, UserEnforced::kFalse);
}

TEST_F_EX(CppCassandraDriverTest, TestTableBackfillUniqueInChunks,
          CppCassandraDriverTestIndexMultipleChunks) {
  TestBackfillIndexTable(this, PKOnlyIndex::kFalse, IsUnique::kTrue,
                         IncludeAllColumns::kTrue, UserEnforced::kFalse);
}

TEST_F_EX(CppCassandraDriverTest, TestIndexUpdateConcurrentTxn, CppCassandraDriverTestIndex) {
  constexpr auto kNamespace = "test";
  const YBTableName table_name(YQL_DATABASE_CQL, kNamespace, "test_table");
  const YBTableName index_table_name(YQL_DATABASE_CQL, kNamespace, "test_table_index_by_v");
  IndexInfoPB index_info_pb;
  YBTableInfo index_table_info;

  TestTable<cass_int32_t, string> table;
  ASSERT_OK(table.CreateTable(&session_, "test.test_table", {"k", "v"}, {"(k)"}, true, 60s));

  LOG(INFO) << "Inserting rows";
  ASSERT_OK(session_.ExecuteQuery("insert into test_table (k, v) values (1, 'one');"));
  ASSERT_OK(session_.ExecuteQuery("insert into test_table (k, v) values (2, 'two');"));

  LOG(INFO) << "Creating index";
  {
    auto session2 = CHECK_RESULT(driver_->CreateSession());
    ASSERT_OK(session2.ExecuteQuery("USE test;"));

    CassandraFuture create_index_future =
        session2.ExecuteGetFuture("create index test_table_index_by_v on test_table (v);");

    auto session3 = CHECK_RESULT(driver_->CreateSession());
    ASSERT_OK(session3.ExecuteQuery("USE test;"));
    WaitUntilIndexPermissionIsAtLeast(client_.get(), table_name, index_table_name,
                                      IndexPermissions::INDEX_PERM_DELETE_ONLY);

    WARN_NOT_OK(session_.ExecuteQuery("insert into test_table (k, v) values (1, 'foo');"),
                "updating k = 1 failed.");
    WARN_NOT_OK(session3.ExecuteQuery("update test_table set v = 'bar' where  k = 2;"),
                "updating k =2 failed.");

    WaitUntilIndexPermissionIsAtLeast(client_.get(), table_name, index_table_name,
                                      IndexPermissions::INDEX_PERM_READ_WRITE_AND_DELETE);
  }

  auto main_table_size = ASSERT_RESULT(GetTableSize(&session_, "test_table"));
  auto index_table_size = ASSERT_RESULT(GetTableSize(&session_, "test_table_index_by_v"));
  EXPECT_EQ(main_table_size, index_table_size);
}

TEST_F_EX(CppCassandraDriverTest, ConcurrentIndexUpdate, CppCassandraDriverTestIndex) {
  constexpr int kLoops = 20;
  constexpr int kKeys = 30;

  typedef TestTable<int, int> MyTable;
  typedef MyTable::ColumnsTuple ColumnsType;
  MyTable table;
  ASSERT_OK(table.CreateTable(&session_, "test.key_value",
                              {"key", "value"}, {"(key)"},
                              true, 60s));

  LOG(INFO) << "Creating index";
  ASSERT_OK(session_.ExecuteQuery("create index index_by_value on test.key_value (value)"));

  std::vector<CassandraFuture> futures;
  int num_failures = 0;
  auto prepared = ASSERT_RESULT(table.PrepareInsert(&session_, 10s));
  for (int loop = 1; loop <= kLoops; ++loop) {
    for (int key = 0; key != kKeys; ++key) {
      auto statement = prepared.Bind();
      ColumnsType tuple(key, loop * 1000 + key);
      table.BindInsert(&statement, tuple);
      futures.push_back(session_.ExecuteGetFuture(statement));
    }
  }

  for (auto it = futures.begin(); it != futures.end();) {
    while (it != futures.end() && it->Ready()) {
      auto status = it->Wait();
      if (!status.ok()) {
        LOG(WARNING) << "Failure: " << status;
        num_failures++;
      }
      ++it;
    }
    for (;;) {
      auto result = session_.ExecuteWithResult("select * from index_by_value");
      if (!result.ok()) {
        LOG(WARNING) << "Read failed: " << result.status();
        continue;
      }
      auto iterator = result->CreateIterator();
      std::unordered_map<int, int> table_content;
      while (iterator.Next()) {
        auto row = iterator.Row();
        auto key = row.Value(0).As<int>();
        auto value = row.Value(1).As<int>();
        auto p = table_content.emplace(key, value);
        ASSERT_TRUE(p.second)
            << "Duplicate key: " << key << ", value: " << value
            << ", existing value: " << p.first->second;
      }
      break;
    }
  }

  {
    constexpr int kBatchKey = 42;
    CassandraBatch batch(CassBatchType::CASS_BATCH_TYPE_LOGGED);
    auto statement1 = prepared.Bind();
    table.BindInsert(&statement1, ColumnsType(kBatchKey, -100));
    batch.Add(&statement1);
    auto statement2 = prepared.Bind();
    table.BindInsert(&statement2, ColumnsType(kBatchKey, -200));
    batch.Add(&statement2);
    ASSERT_OK(session_.ExecuteBatch(batch));

    auto result = ASSERT_RESULT(session_.ExecuteWithResult("select * from index_by_value"));
    auto iterator = result.CreateIterator();
    while (iterator.Next()) {
      auto row = iterator.Row();
      auto key = row.Value(0).As<int>();
      auto value = row.Value(1).As<int>();
      if (value < 0) {
        ASSERT_EQ(key, kBatchKey);
        ASSERT_EQ(value, -200);
      }
    }
  }
}

TEST_F(CppCassandraDriverTest, TestPrepare) {
  typedef TestTable<cass_bool_t, cass_int32_t, string, cass_int32_t, string> MyTable;
  MyTable table;
  ASSERT_OK(table.CreateTable(
      &session_, "test.basic", {"b", "val", "key", "int_key", "str"}, {"key", "int_key"}));

  const MyTable::ColumnsTuple input(cass_true, 0xAABBCCDD, "key1test", 0xDEADBEAF, "mystr");
  {
    auto prepared = ASSERT_RESULT(table.PrepareInsert(&session_));
    auto statement = prepared.Bind();
    // Prepared object can now be used to create new statement.

    table.Print("Execute prepared INSERT with INPUT", input);
    table.BindInsert(&statement, input);
    ASSERT_OK(session_.Execute(statement));
  }

  MyTable::ColumnsTuple output(cass_false, 0, "key1test", 0xDEADBEAF, "");
  table.SelectOneRow(&session_, &output);
  table.Print("RESULT OUTPUT", output);
  LOG(INFO) << "Checking selected values...";
  ExpectEqualTuples(input, output);
}

template <typename... ColumnsTypes>
void TestTokenForTypes(
    CassandraSession* session,
    const vector<string>& columns,
    const vector<string>& keys,
    const tuple<ColumnsTypes...>& input_data,
    const tuple<ColumnsTypes...>& input_keys,
    const tuple<ColumnsTypes...>& input_empty,
    int64_t exp_token = 0) {
  typedef TestTable<ColumnsTypes...> MyTable;
  typedef typename MyTable::ColumnsTuple ColumnsTuple;

  MyTable table;
  ASSERT_OK(table.CreateTable(session, "test.basic", columns, keys));

  auto prepared = ASSERT_RESULT(table.PrepareInsert(session));
  auto statement = prepared.Bind();

  const ColumnsTuple input(input_data);
  table.Print("Execute prepared INSERT with INPUT", input);
  table.BindInsert(&statement, input);

  int64_t token = 0;
  bool token_available = cass_partition_aware_policy_get_yb_hash_code(
      statement.get(), &token);
  LOG(INFO) << "Got token: " << (token_available ? "OK" : "ERROR") << " token=" << token
            << " (0x" << std::hex << token << ")";
  ASSERT_TRUE(token_available);

  if (exp_token > 0) {
    ASSERT_EQ(exp_token, token);
  }

  ASSERT_OK(session->Execute(statement));

  ColumnsTuple output_by_key(input_keys);
  table.SelectOneRow(session, &output_by_key);
  table.Print("RESULT OUTPUT", output_by_key);
  LOG(INFO) << "Checking selected values...";
  ExpectEqualTuples(input, output_by_key);

  ColumnsTuple output = ASSERT_RESULT(table.SelectByToken(session, token));
  table.Print("RESULT OUTPUT", output);
  LOG(INFO) << "Checking selected by TOKEN values...";
  ExpectEqualTuples(input, output);
}

template <typename KeyType>
void TestTokenForType(
    CassandraSession* session, const KeyType& key, int64_t exp_token = 0) {
  typedef tuple<KeyType, cass_double_t> Tuple;
  TestTokenForTypes(session,
                    {"key", "value"}, // column names
                    {"(key)"}, // key names
                    Tuple(key, 0.56789), // data
                    Tuple(key, 0.), // only keys
                    Tuple((KeyType()), 0.), // empty
                    exp_token);
}

TEST_F(CppCassandraDriverTest, TestTokenForText) {
  TestTokenForType<string>(&session_, "test", 0x8753000000000000);
}

TEST_F(CppCassandraDriverTest, TestTokenForInt) {
  TestTokenForType<int32_t>(&session_, 0xDEADBEAF);
}

TEST_F(CppCassandraDriverTest, TestTokenForBigInt) {
  TestTokenForType<int64_t>(&session_, 0xDEADBEAFDEADBEAFULL);
}

TEST_F(CppCassandraDriverTest, TestTokenForBoolean) {
  TestTokenForType<cass_bool_t>(&session_, cass_true);
}

TEST_F(CppCassandraDriverTest, TestTokenForFloat) {
  TestTokenForType<cass_float_t>(&session_, 0.123f);
}

TEST_F(CppCassandraDriverTest, TestTokenForDouble) {
  TestTokenForType<cass_double_t>(&session_, 0.12345);
}

TEST_F(CppCassandraDriverTest, TestTokenForDoubleKey) {
  typedef tuple<string, cass_int32_t, cass_double_t> Tuple;
  TestTokenForTypes(&session_,
                    {"key", "int_key", "value"}, // column names
                    {"(key", "int_key)"}, // key names
                    Tuple("test", 0xDEADBEAF, 0.123), // data
                    Tuple("test", 0xDEADBEAF, 0.), // only keys
                    Tuple("", 0, 0.)); // empty
}

//------------------------------------------------------------------------------

struct IOMetrics {
  IOMetrics() = default;
  IOMetrics(const IOMetrics&) = default;

  explicit IOMetrics(const ExternalMiniCluster& cluster) : IOMetrics() {
    load(cluster);
  }

  static void load_value(
      const ExternalMiniCluster& cluster, int ts_index,
      const MetricPrototype* metric_proto, int64_t* value) {
    const ExternalTabletServer& ts = *CHECK_NOTNULL(cluster.tablet_server(ts_index));
    const auto result = ts.GetInt64CQLMetric(
        &METRIC_ENTITY_server, "yb.cqlserver", CHECK_NOTNULL(metric_proto),
        "total_count");

    if (!result.ok()) {
      LOG(ERROR) << "Failed to get metric " << metric_proto->name() << " from TS"
          << ts_index << ": " << ts.bind_host() << ":" << ts.cql_http_port()
          << " with error " << result.status();
    }
    ASSERT_OK(result);
    *CHECK_NOTNULL(value) = *result;
  }

  void load(const ExternalMiniCluster& cluster) {
    *this = IOMetrics();
    for (int i = 0; i < cluster.num_tablet_servers(); ++i) {
      IOMetrics m;
      load_value(cluster, i, &METRIC_handler_latency_yb_client_write_remote, &m.remote_write);
      load_value(cluster, i, &METRIC_handler_latency_yb_client_read_remote, &m.remote_read);
      load_value(cluster, i, &METRIC_handler_latency_yb_client_write_local, &m.local_write);
      load_value(cluster, i, &METRIC_handler_latency_yb_client_read_local, &m.local_read);
      *this += m;
    }
  }

  IOMetrics& operator +=(const IOMetrics& m) {
    local_read += m.local_read;
    local_write += m.local_write;
    remote_read += m.remote_read;
    remote_write += m.remote_write;
    return *this;
  }

  IOMetrics& operator -=(const IOMetrics& m) {
    local_read -= m.local_read;
    local_write -= m.local_write;
    remote_read -= m.remote_read;
    remote_write -= m.remote_write;
    return *this;
  }

  IOMetrics operator +(const IOMetrics& m) const {
    return IOMetrics(*this) += m;
  }

  IOMetrics operator -(const IOMetrics& m) const {
    return IOMetrics(*this) -= m;
  }

  int64_t local_read = 0;
  int64_t local_write = 0;
  int64_t remote_read = 0;
  int64_t remote_write = 0;
};

std::ostream& operator <<(std::ostream& s, const IOMetrics& m) {
  return s << "LocalRead=" << m.local_read << " LocalWrite=" << m.local_write
      << " RemoteRead=" << m.remote_read << " RemoteWrite=" << m.remote_write;
}

//------------------------------------------------------------------------------

TEST_F(CppCassandraDriverTest, TestInsertLocality) {
  typedef TestTable<string, string> MyTable;
  typedef typename MyTable::ColumnsTuple ColumnsTuple;

  MyTable table;
  ASSERT_OK(table.CreateTable(&session_, "test.basic", {"id", "data"}, {"(id)"}));

  LOG(INFO) << "Wait 5 sec to refresh metadata in driver by time";
  SleepFor(MonoDelta::FromMicroseconds(5*1000000));

  IOMetrics pre_metrics(*cluster_);

  auto prepared = ASSERT_RESULT(table.PrepareInsert(&session_));
  const int total_keys = 100;
  ColumnsTuple input("", "test_value");

  for (int i = 0; i < total_keys; ++i) {
    get<0>(input) = Substitute("key_$0", i);

    // Prepared object can now be used to create new statement.
    auto statement = prepared.Bind();
    table.BindInsert(&statement, input);
    ASSERT_OK(session_.Execute(statement));
  }

  IOMetrics post_metrics(*cluster_);
  const IOMetrics delta_metrics = post_metrics - pre_metrics;
  LOG(INFO) << "DELTA Metrics: " << delta_metrics;

  // Expect minimum 70% of all requests to be local.
  ASSERT_GT(delta_metrics.local_write*10, total_keys*7);
}

class CppCassandraDriverLowSoftLimitTest : public CppCassandraDriverTest {
 public:
  std::vector<std::string> ExtraTServerFlags() override {
    return {"--memory_limit_soft_percentage=0"s};
  }

  bool UsePartitionAwareRouting() override {
    // TODO: Disable partition aware routing in this test because of TSAN issue (#1837).
    // Should be reenabled when issue is fixed.
    return false;
  }
};

TEST_F_EX(CppCassandraDriverTest, BatchWriteDuringSoftMemoryLimit,
          CppCassandraDriverLowSoftLimitTest) {
  FLAGS_external_mini_cluster_max_log_bytes = 512_MB;

  constexpr int kBatchSize = 500;
  constexpr int kWriters = 4;
  constexpr int kNumMetrics = 5;

  typedef TestTable<std::string, int64_t, std::string> MyTable;
  typedef MyTable::ColumnsTuple ColumnsType;
  MyTable table;
  ASSERT_OK(table.CreateTable(
      &session_, "test.batch_ts_metrics_raw", {"metric_id", "ts", "value"},
      {"(metric_id, ts)"}));

  TestThreadHolder thread_holder;
  std::array<std::atomic<int>, kNumMetrics> metric_ts;
  std::atomic<int> total_writes(0);
  for (int i = 0; i != kNumMetrics; ++i) {
    metric_ts[i].store(0);
  }

  for (int i = 0; i != kWriters; ++i) {
    thread_holder.AddThreadFunctor(
        [this, &stop = thread_holder.stop_flag(), &table, &metric_ts, &total_writes] {
      SetFlagOnExit set_flag_on_exit(&stop);
      auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
      std::vector<CassandraFuture> futures;
      while (!stop.load()) {
        CassandraBatch batch(CassBatchType::CASS_BATCH_TYPE_LOGGED);
        auto prepared = table.PrepareInsert(&session);
        if (!prepared.ok()) {
          // Prepare could be failed because cluster has heavy load.
          // It is ok to just retry in this case, because we expect total number of writes.
          continue;
        }
        int metric_idx = RandomUniformInt(1, kNumMetrics);
        auto metric = "metric_" + std::to_string(metric_idx);
        int ts = metric_ts[metric_idx - 1].fetch_add(kBatchSize);
        for (int i = 0; i != kBatchSize; ++i) {
          ColumnsType tuple(metric, ts, "value_" + std::to_string(ts));
          auto statement = prepared->Bind();
          table.BindInsert(&statement, tuple);
          batch.Add(&statement);
          ++ts;
        }
        futures.push_back(session.SubmitBatch(batch));
        ++total_writes;
      }
    });
  }

  thread_holder.WaitAndStop(30s);
  auto total_writes_value = total_writes.load();
  LOG(INFO) << "Total writes: " << total_writes_value;
  ASSERT_GE(total_writes_value, RegularBuildVsSanitizers(1500, 50));
}

class CppCassandraDriverBackpressureTest : public CppCassandraDriverTest {
 public:
  std::vector<std::string> ExtraTServerFlags() override {
    return {"--tablet_server_svc_queue_length=10"s, "--max_time_in_queue_ms=-1"s};
  }

  bool UsePartitionAwareRouting() override {
    // TODO: Disable partition aware routing in this test because of TSAN issue (#1837).
    // Should be reenabled when issue is fixed.
    return false;
  }
};

TEST_F_EX(CppCassandraDriverTest, LocalCallBackpressure, CppCassandraDriverBackpressureTest) {
  constexpr int kBatchSize = 30;
  constexpr int kNumBatches = 300;

  typedef TestTable<int64_t, int64_t> MyTable;
  typedef MyTable::ColumnsTuple ColumnsType;
  MyTable table;
  ASSERT_OK(table.CreateTable(&session_, "test.key_value", {"key", "value"}, {"(key)"}));

  std::vector<CassandraFuture> futures;

  for (int batch_idx = 0; batch_idx != kNumBatches; ++batch_idx) {
    CassandraBatch batch(CassBatchType::CASS_BATCH_TYPE_LOGGED);
    auto prepared = table.PrepareInsert(&session_);
    if (!prepared.ok()) {
      // Prepare could be failed because cluster has heavy load.
      // It is ok to just retry in this case, because we check that process did not crash.
      continue;
    }
    for (int i = 0; i != kBatchSize; ++i) {
      ColumnsType tuple(batch_idx * kBatchSize + i, -1);
      auto statement = prepared->Bind();
      table.BindInsert(&statement, tuple);
      batch.Add(&statement);
    }
    futures.push_back(session_.SubmitBatch(batch));
  }

  for (auto& future : futures) {
    WARN_NOT_OK(future.Wait(), "Write failed");
  }
}

class CppCassandraDriverTransactionalWriteTest : public CppCassandraDriverTest {
 public:
  std::vector<std::string> ExtraTServerFlags() override {
    return {"--TEST_transaction_inject_flushed_delay_ms=10"s};
  }

  bool UsePartitionAwareRouting() override {
    // TODO: Disable partition aware routing in this test because of TSAN issue (#1837).
    // Should be reenabled when issue is fixed.
    return false;
  }
};

TEST_F_EX(CppCassandraDriverTest, TransactionalWrite, CppCassandraDriverTransactionalWriteTest) {
  const std::string kTableName = "test.key_value";
  typedef TestTable<int32_t, int32_t> MyTable;
  MyTable table;
  ASSERT_OK(table.CreateTable(
      &session_, kTableName, {"key", "value"}, {"(key)"}, true /* transactional */));

  constexpr int kIterations = 20;
  auto prepared = ASSERT_RESULT(session_.Prepare(Format(
      "BEGIN TRANSACTION"
      "  INSERT INTO $0 (key, value) VALUES (?, ?);"
      "  INSERT INTO $0 (key, value) VALUES (?, ?);"
      "END TRANSACTION;", kTableName)));
  for (int i = 1; i <= kIterations; ++i) {
    auto statement = prepared.Bind();
    statement.Bind(0, i);
    statement.Bind(1, i * 3);
    statement.Bind(2, -i);
    statement.Bind(3, i * -4);
    ASSERT_OK(session_.Execute(statement));
  }
}

class CppCassandraDriverTestThreeMasters : public CppCassandraDriverTest {
 private:
  int NumMasters() override {
    return 3;
  }

  bool UsePartitionAwareRouting() override {
    // TODO: Disable partition aware routing in this test because of TSAN issue (#1837).
    // Should be reenabled when issue is fixed.
    return false;
  }
};

TEST_F_EX(CppCassandraDriverTest, ManyTables, CppCassandraDriverTestThreeMasters) {
  FLAGS_external_mini_cluster_max_log_bytes = 512_MB;

  constexpr int kThreads = RegularBuildVsSanitizers(5, 2);
  constexpr int kTables = RegularBuildVsSanitizers(15, 5);
  constexpr int kReads = 20;

  const std::string kTableNameFormat = "test.key_value_$0_$1";
  typedef TestTable<int32_t, int32_t> MyTable;

  TestThreadHolder thread_holder;
  std::atomic<int> tables(0);

  for (int i = 0; i != kThreads; ++i) {
    thread_holder.AddThreadFunctor(
        [this, &stop = thread_holder.stop_flag(), thread = i, &kTableNameFormat, &tables] {
          SetFlagOnExit set_flag_on_exit(&stop);
          auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
          int idx = 0;
          while (!stop.load(std::memory_order_acquire)) {
            MyTable table;
            auto status = table.CreateTable(
                &session, Format(kTableNameFormat, thread, idx), {"key", "value"}, {"(key)"});
            if (status.ok()) {
              LOG(INFO) << "Created table " << thread << ", " << idx;
              // We need at least kTables tables.
              if (tables.fetch_add(1, std::memory_order_acq_rel) >= kTables) {
                break;
              }
            } else {
              LOG(INFO) << "Failed to create table " << thread << ", " << idx << ": " << status;
            }
            ++idx;
          }
        });
  }

  thread_holder.WaitAndStop(180s);

  ASSERT_GE(tables.load(std::memory_order_acquire), kTables);

  CassandraStatement statement("SELECT * FROM system.partitions");
  std::vector<MonoDelta> read_times;
  read_times.reserve(kReads);
  int i = 0;
  for (;;) {
    auto start = MonoTime::Now();
    auto result = session_.ExecuteWithResult(statement);
    auto finish = MonoTime::Now();
    if (!result.ok()) {
      LOG(INFO) << "Read failed: " << result.status();
      continue;
    }
    read_times.push_back(finish - start);
    ++i;
    if (i == kReads) {
      LogResult(*result);
      break;
    }
  }

  LOG(INFO) << "Read times: " << AsString(read_times);
  std::sort(read_times.begin(), read_times.end());

  if (!IsSanitizer()) {
    ASSERT_LE(read_times.front() * 2, read_times.back()); // Check that cache works
  }
}

class CppCassandraDriverRejectionTest : public CppCassandraDriverTest {
 public:
  std::vector<std::string> ExtraTServerFlags() override {
    return {"--TEST_write_rejection_percentage=15"s,
            "--linear_backoff_ms=10"};
  }

  bool UsePartitionAwareRouting() override {
    // Disable partition aware routing in this test because of TSAN issue (#1837).
    // Should be reenabled when issue is fixed.
    return false;
  }
};

TEST_F_EX(CppCassandraDriverTest, Rejection, CppCassandraDriverRejectionTest) {
  constexpr int kBatchSize = 50;
  constexpr int kWriters = 21;

  typedef TestTable<int64_t, int64_t> MyTable;
  typedef MyTable::ColumnsTuple ColumnsType;
  MyTable table;
  ASSERT_OK(table.CreateTable(&session_, "test.key_value", {"key", "value"}, {"(key)"}));

  TestThreadHolder thread_holder;
  std::atomic<int64_t> key(0);
  std::atomic<int> pending_writes(0);
  std::atomic<int> max_pending_writes(0);

  for (int i = 0; i != kWriters; ++i) {
    thread_holder.AddThreadFunctor(
        [this, &stop = thread_holder.stop_flag(), &table, &key, &pending_writes,
         &max_pending_writes] {
      SetFlagOnExit set_flag_on_exit(&stop);
      auto session = ASSERT_RESULT(EstablishSession(driver_.get()));
      while (!stop.load()) {
        CassandraBatch batch(CassBatchType::CASS_BATCH_TYPE_LOGGED);
        auto prepared = table.PrepareInsert(&session);
        if (!prepared.ok()) {
          // Prepare could be failed because cluster has heavy load.
          // It is ok to just retry in this case, because we expect total number of writes.
          continue;
        }
        for (int i = 0; i != kBatchSize; ++i) {
          auto current_key = key++;
          ColumnsType tuple(current_key, -current_key);
          auto statement = prepared->Bind();
          table.BindInsert(&statement, tuple);
          batch.Add(&statement);
        }
        auto future = session.SubmitBatch(batch);
        auto status = future.WaitFor(kCassandraTimeOut / 2);
        if (status.IsTimedOut()) {
          auto pw = ++pending_writes;
          auto mpw = max_pending_writes.load();
          while (pw > mpw) {
            if (max_pending_writes.compare_exchange_weak(mpw, pw)) {
              // Assert that we don't have too many pending writers.
              ASSERT_LE(pw, kWriters / 3);
              break;
            }
          }
          auto wait_status = future.Wait();
          ASSERT_TRUE(wait_status.ok() || wait_status.IsTimedOut()) << wait_status;
          --pending_writes;
        } else {
          ASSERT_OK(status);
        }
      }
    });
  }

  thread_holder.WaitAndStop(30s);
  LOG(INFO) << "Max pending writes: " << max_pending_writes.load();
}

TEST_F(CppCassandraDriverTest, BigQueryExpr) {
  const std::string kTableName = "test.key_value";
  typedef TestTable<std::string> MyTable;
  MyTable table;
  ASSERT_OK(table.CreateTable(&session_, kTableName, {"key"}, {"(key)"}));

  constexpr size_t kRows = 400;
  constexpr size_t kValueSize = RegularBuildVsSanitizers(256_KB, 4_KB);

  auto prepared = ASSERT_RESULT(session_.Prepare(
      Format("INSERT INTO $0 (key) VALUES (?);", kTableName)));

  for (int32_t i = 0; i != kRows; ++i) {
    auto statement = prepared.Bind();
    statement.Bind(0, RandomHumanReadableString(kValueSize));
    ASSERT_OK(session_.Execute(statement));
  }

  auto start = MonoTime::Now();
  auto result = ASSERT_RESULT(session_.ExecuteWithResult(Format(
      "SELECT MAX(key) FROM $0", kTableName)));
  auto finish = MonoTime::Now();
  LOG(INFO) << "Time: " << finish - start;

  auto iterator = result.CreateIterator();
  ASSERT_TRUE(iterator.Next());
  LOG(INFO) << "Result: " << iterator.Row().Value(0).ToString();
  ASSERT_FALSE(iterator.Next());
}

}  // namespace yb
