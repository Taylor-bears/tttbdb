#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include "analyze/analyze.h"
#include "execution/execution_manager.h"
#include "gtest/gtest.h"
#include "index/ix.h"
#include "optimizer/optimizer.h"
#include "parser/parser.h"
#include "portal.h"
#include "recovery/log_manager.h"
#include "system/sm.h"
#include "transaction/transaction_manager.h"

class QueryExecutionTest : public ::testing::Test {
   protected:
    static constexpr const char *DB_NAME = "QueryExecutionTest_db";

    std::unique_ptr<DiskManager> disk_manager;
    std::unique_ptr<BufferPoolManager> buffer_pool_manager;
    std::unique_ptr<RmManager> rm_manager;
    std::unique_ptr<IxManager> ix_manager;
    std::unique_ptr<SmManager> sm_manager;
    std::unique_ptr<LockManager> lock_manager;
    std::unique_ptr<TransactionManager> txn_manager;
    std::unique_ptr<QlManager> ql_manager;
    std::unique_ptr<LogManager> log_manager;
    std::unique_ptr<Planner> planner;
    std::unique_ptr<Optimizer> optimizer;
    std::unique_ptr<Portal> portal;
    std::unique_ptr<Analyze> analyze;
    Transaction *txn{};
    txn_id_t txn_id{};
    char data_send[BUFFER_LENGTH]{};
    int offset{};
    std::unique_ptr<Context> context;

    void SetUp() override {
        std::filesystem::remove_all(DB_NAME);
        disk_manager = std::make_unique<DiskManager>();
        buffer_pool_manager = std::make_unique<BufferPoolManager>(128, disk_manager.get());
        rm_manager = std::make_unique<RmManager>(disk_manager.get(), buffer_pool_manager.get());
        ix_manager = std::make_unique<IxManager>(disk_manager.get(), buffer_pool_manager.get());
        sm_manager = std::make_unique<SmManager>(disk_manager.get(), buffer_pool_manager.get(), rm_manager.get(),
                                                 ix_manager.get());
        lock_manager = std::make_unique<LockManager>();
        txn_manager = std::make_unique<TransactionManager>(lock_manager.get(), sm_manager.get());
        ql_manager = std::make_unique<QlManager>(sm_manager.get(), txn_manager.get());
        log_manager = std::make_unique<LogManager>(disk_manager.get());
        planner = std::make_unique<Planner>(sm_manager.get());
        optimizer = std::make_unique<Optimizer>(sm_manager.get(), planner.get());
        portal = std::make_unique<Portal>(sm_manager.get());
        analyze = std::make_unique<Analyze>(sm_manager.get());

        sm_manager->create_db(DB_NAME);
        sm_manager->open_db(DB_NAME);
        txn = txn_manager->begin(nullptr, log_manager.get());
        txn_id = txn->get_transaction_id();
        context = std::make_unique<Context>(lock_manager.get(), log_manager.get(), txn, data_send, &offset);
    }

    void TearDown() override {
        txn_manager->commit(txn, log_manager.get());
        TransactionManager::txn_map.erase(txn_id);
        delete txn;
        sm_manager->close_db();
        std::filesystem::remove_all(DB_NAME);
    }

    void execute(const std::string &sql) {
        memset(data_send, 0, sizeof(data_send));
        offset = 0;
        YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
        ASSERT_EQ(0, yyparse()) << sql;
        ASSERT_NE(nullptr, ast::parse_tree) << sql;
        auto query = analyze->do_analyze(ast::parse_tree);
        yy_delete_buffer(buffer);
        auto plan = optimizer->plan_query(query, context.get());
        auto stmt = portal->start(plan, context.get());
        portal->run(stmt, ql_manager.get(), &txn_id, context.get());
    }

    bool is_rejected(const std::string &sql) {
        ast::parse_tree = nullptr;
        YY_BUFFER_STATE buffer = yy_scan_string(sql.c_str());
        int parse_result = yyparse();
        if (parse_result != 0 || ast::parse_tree == nullptr) {
            yy_delete_buffer(buffer);
            return true;
        }
        try {
            auto query = analyze->do_analyze(ast::parse_tree);
            auto plan = optimizer->plan_query(query, context.get());
            auto stmt = portal->start(plan, context.get());
            portal->run(stmt, ql_manager.get(), &txn_id, context.get());
        } catch (const RMDBError &) {
            yy_delete_buffer(buffer);
            return true;
        }
        yy_delete_buffer(buffer);
        return false;
    }

    std::string output() {
        std::ifstream input("output.txt");
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
};

TEST_F(QueryExecutionTest, CreateShowAndDropTables) {
    execute("create table t1 (id int, name char(4));");
    execute("show tables;");
    execute("create table t2 (id int);");
    execute("show tables;");
    execute("drop table t1;");
    execute("show tables;");
    execute("drop table t2;");
    execute("show tables;");

    auto text = output();
    EXPECT_NE(std::string::npos, text.find("| Tables |"));
    EXPECT_NE(std::string::npos, text.find("| t1 |"));
    EXPECT_NE(std::string::npos, text.find("| t2 |"));
    EXPECT_FALSE(sm_manager->db_.is_table("t1"));
    EXPECT_FALSE(sm_manager->db_.is_table("t2"));
}

TEST_F(QueryExecutionTest, InsertAndConditionalSelect) {
    execute("create table grade (name char(4), id int, score float);");
    execute("insert into grade values ('Data', 1, 90.5);");
    execute("insert into grade values ('Data', 2, 95.0);");
    execute("insert into grade values ('Calc', 2, 92.0);");
    execute("insert into grade values ('Calc', 1, 88.5);");
    execute("select * from grade;");
    execute("select score, name, id from grade where score > 90;");
    execute("select id from grade where name = 'Data';");
    execute("select name from grade where id = 2 and score > 90;");

    auto text = output();
    EXPECT_NE(std::string::npos, text.find("| Data | 1 | 90.500000 |"));
    EXPECT_NE(std::string::npos, text.find("| 95.000000 | Data | 2 |"));
    EXPECT_NE(std::string::npos, text.find("| id |\n| 1 |\n| 2 |"));
    EXPECT_NE(std::string::npos, text.find("| name |\n| Data |\n| Calc |"));
}

TEST_F(QueryExecutionTest, UpdateWithMultipleAssignments) {
    execute("create table grade (name char(4), id int, score float);");
    execute("insert into grade values ('Data', 1, 90.5);");
    execute("insert into grade values ('Data', 2, 95.0);");
    execute("insert into grade values ('Calc', 2, 92.0);");
    execute("insert into grade values ('Calc', 1, 88.5);");
    execute("update grade set score = 99.0 where name = 'Calc';");
    execute("update grade set name = 'test' where name > 'A';");
    execute("update grade set name = 'test', id = -1, score = 0 where name = 'test' and score > 90;");
    execute("select * from grade;");

    auto text = output();
    EXPECT_NE(std::string::npos, text.find("| test | -1 | 0.000000 |"));
}

TEST_F(QueryExecutionTest, DeleteWithFloatCondition) {
    execute("create table grade (name char(4), id int, score float);");
    execute("insert into grade values ('Data', 1, 90.5);");
    execute("delete from grade where score > 90;");
    execute("select * from grade;");

    auto text = output();
    EXPECT_EQ("| name | id | score |\n", text);
}

TEST_F(QueryExecutionTest, CrossProductAndJoin) {
    execute("create table t (id int, t_name char(3));");
    execute("create table d (d_name char(5), id int);");
    execute("insert into t values (1, 'aaa');");
    execute("insert into t values (2, 'baa');");
    execute("insert into t values (3, 'bba');");
    execute("insert into d values ('12345', 1);");
    execute("insert into d values ('23456', 2);");
    execute("select * from t, d;");
    execute("select t.id, t_name, d_name from t, d where t.id = d.id;");

    auto text = output();
    EXPECT_NE(std::string::npos, text.find("| 3 | bba | 23456 | 2 |"));
    EXPECT_NE(std::string::npos, text.find("| 1 | aaa | 12345 |"));
    EXPECT_NE(std::string::npos, text.find("| 2 | baa | 23456 |"));
}

TEST_F(QueryExecutionTest, RejectsInvalidSqlAndPersistsMetadata) {
    execute("create table t (id int, name char(4), score float);");
    execute("insert into t values (1, 'Data', 90);");

    EXPECT_TRUE(is_rejected("select missing from t;"));
    EXPECT_TRUE(is_rejected("select * from missing;"));
    EXPECT_TRUE(is_rejected("insert into t values ('bad', 'Data', 90);"));
    EXPECT_TRUE(is_rejected("update t set missing = 1;"));
    EXPECT_TRUE(is_rejected("drop table missing;"));
    EXPECT_TRUE(is_rejected("create table t (id int);"));
    EXPECT_TRUE(is_rejected("select * from t, t;"));
    EXPECT_TRUE(is_rejected("select from t;"));

    sm_manager->close_db();
    sm_manager->open_db(DB_NAME);
    execute("select * from t;");
    EXPECT_NE(std::string::npos, output().find("| 1 | Data | 90.000000 |"));
}

TEST_F(QueryExecutionTest, BigIntInsertQueryUpdateDeleteAndBounds) {
    execute("create table t (bid bigint, sid int);");
    execute("insert into t values (372036854775807, 233421);");
    execute("insert into t values (-922337203685477580, 124332);");
    execute("insert into t values (9223372036854775807, 1);");
    execute("insert into t values (-9223372036854775808, 2);");
    execute("select * from t;");

    EXPECT_TRUE(is_rejected("insert into t values (9223372036854775809, 12345);"));
    EXPECT_TRUE(is_rejected("insert into t values (-9223372036854775809, 12345);"));
    EXPECT_TRUE(is_rejected("insert into t values (372036854775807, 999999999999);"));

    execute("update t set bid = 7 where bid = 372036854775807;");
    execute("delete from t where bid < -922337203685477580;");
    execute("select * from t where bid >= -922337203685477580;");

    sm_manager->close_db();
    sm_manager->open_db(DB_NAME);
    execute("select * from t where bid = 7;");

    const auto &bid_col = sm_manager->db_.get_table("t").cols[0];
    EXPECT_EQ(TYPE_BIGINT, bid_col.type);
    EXPECT_EQ(sizeof(int64_t), static_cast<size_t>(bid_col.len));

    auto text = output();
    EXPECT_NE(std::string::npos, text.find("| 372036854775807 | 233421 |"));
    EXPECT_NE(std::string::npos, text.find("| -922337203685477580 | 124332 |"));
    EXPECT_NE(std::string::npos, text.find("| 9223372036854775807 | 1 |"));
    EXPECT_NE(std::string::npos, text.find("| -9223372036854775808 | 2 |"));
    EXPECT_NE(std::string::npos, text.find("| 7 | 233421 |"));
}

TEST_F(QueryExecutionTest, DateTimeCrudComparisonAndPersistence) {
    execute("create table t (id int, time datetime);");
    execute("insert into t values (1, '2023-05-18 09:12:19');");
    execute("insert into t values (2, '2023-05-30 12:34:32');");
    execute("select * from t;");
    execute("delete from t where time = '2023-05-30 12:34:32';");
    execute("update t set id = 2023 where time = '2023-05-18 09:12:19';");
    execute("update t set time = '2024-02-29 23:59:59' where id = 2023;");

    sm_manager->close_db();
    sm_manager->open_db(DB_NAME);
    execute("select * from t where time >= '2024-01-01 00:00:00';");

    const auto &time_col = sm_manager->db_.get_table("t").cols[1];
    EXPECT_EQ(TYPE_DATETIME, time_col.type);
    EXPECT_EQ(sizeof(int64_t), static_cast<size_t>(time_col.len));

    auto text = output();
    EXPECT_NE(std::string::npos, text.find("| 1 | 2023-05-18 09:12:19 |"));
    EXPECT_NE(std::string::npos, text.find("| 2 | 2023-05-30 12:34:32 |"));
    EXPECT_NE(std::string::npos, text.find("| 2023 | 2024-02-29 23:59:59 |"));
}

TEST_F(QueryExecutionTest, DateTimeValidationAndBounds) {
    execute("create table t (time datetime, temperature float);");
    execute("insert into t values ('1999-07-07 12:30:00', 36.0);");
    execute("insert into t values ('2000-02-29 00:00:00', 36.0);");
    execute("insert into t values ('1000-01-01 00:00:00', 36.0);");
    execute("insert into t values ('9999-12-31 23:59:59', 36.0);");

    const std::vector<std::string> invalid_values = {
        "1999-13-07 12:30:00", "1999-1-07 12:30:00", "1999-00-07 12:30:00",
        "1999-07-00 12:30:00", "0001-07-10 12:30:00", "1999-02-30 12:30:00",
        "1999-02-28 12:30:61", "1900-02-29 12:30:00", "1999-04-31 12:30:00",
        "1999-07-07 24:00:00", "1999-07-07 12:60:00", "1999/07/07 12:30:00",
        "1999-07-07T12:30:00", "10000-01-01 00:00:00", "2023-05-18 09:12:19x"};
    for (const auto &value : invalid_values) {
        EXPECT_TRUE(is_rejected("insert into t values ('" + value + "', 36.0);")) << value;
    }
    execute("select * from t;");

    auto text = output();
    EXPECT_NE(std::string::npos, text.find("| 1999-07-07 12:30:00 | 36.000000 |"));
    EXPECT_NE(std::string::npos, text.find("| 2000-02-29 00:00:00 | 36.000000 |"));
    EXPECT_NE(std::string::npos, text.find("| 1000-01-01 00:00:00 | 36.000000 |"));
    EXPECT_NE(std::string::npos, text.find("| 9999-12-31 23:59:59 | 36.000000 |"));
}
