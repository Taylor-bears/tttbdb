# T10_可串行化与NoWait_交接

## 1. 基本信息

- 题目编号：T10
- 题目名称：基于死锁预防的可串行化隔离级别
- 负责人：邵云慧
- 当前状态：已通过
- 最近更新时间：2026-07-12

## 2. 本题做了什么

本题为 RMDB 实现了严格两阶段封锁与 no-wait 死锁预防。SELECT 在扫描前获取表级共享锁，INSERT、DELETE、UPDATE 在读取或修改前获取表级排他锁，所有锁一直持有到 commit 或 abort。锁冲突时事务不等待，立即中止并由服务器向客户端返回 `abort\n`；表级锁同时阻止范围查询期间的插入，从而避免幻读。希冀平台一次性全部通过。

## 3. 主要修改文件

| 文件路径 | 改动说明 |
|---|---|
| `src/transaction/concurrency/lock_manager.h` | 增加统一加锁、兼容判断及锁队列维护接口 |
| `src/transaction/concurrency/lock_manager.cpp` | 实现 S/X/意向锁、锁升级、解锁和 no-wait 冲突处理 |
| `src/execution/executor_seq_scan.h` | 顺序扫描开始前申请表级 S 锁 |
| `src/execution/executor_index_scan.h` | 索引扫描开始前申请表级 S 锁 |
| `src/portal.h` | 三类写语句在扫描或执行前申请表级 X 锁 |
| `src/transaction/transaction_manager.cpp` | commit/abort 时安全释放事务持有的全部锁 |
| `src/rmdb.cpp` | 避免已中止的隐式事务再次自动提交 |
| `src/query_execution_test.cpp` | 增加多事务交错执行及五类异常专项测试 |

## 4. 核心实现逻辑

1. LockManager 使用互斥锁保护全局锁表和各数据项的请求队列。
2. S 锁之间兼容；涉及 X 锁的不同事务请求均冲突，同一事务可在条件允许时由 S 升级到 X。
3. 冲突请求不进入等待队列，直接抛出 TransactionAbortException，实现 no-wait。
4. SELECT 对涉及的每张表持有 S 锁；写语句持有 X 锁，因此读写和写写冲突可串行化。
5. commit/abort 复制锁集合后逐个解锁，最后清空集合并更新事务状态。

## 5. 关键注意事项

- 写语句必须在收集待修改 RID 之前取得 X 锁，否则扫描与实际写入之间仍可能发生竞态。
- BNLJ 重置右输入时会重复申请 S 锁，LockManager 必须识别本事务已持有的锁并直接成功。
- 表级 S 锁覆盖整个范围查询，可阻止其他事务插入、删除或更新，避免幻读。
- 锁冲突抛出异常后由服务器统一执行 abort，客户端只收到 `abort\n`，不能附加调试文本。
- 严格 2PL 要求普通执行阶段不提前释放锁，只能在 commit/abort 中统一释放。

## 6. 快速验证方式

```bash
mkdir -p build && cd build
cmake ..
make query_execution_test unit_test test_parser -j4
./bin/query_execution_test
./bin/unit_test
./bin/test_parser
```

## 7. 测试结果记录

- 希冀平台：一次性全部通过
- 通过时间：2026-07-12
- 本地查询执行测试：18/18 通过
- 题目一存储回归：5/5 通过
- Parser 测试：通过
- S/S 并发读取：通过
- S/X、X/X no-wait 中止：通过
- 脏读、脏写、丢失更新、不可重复读和幻读防护：通过
- 唯一索引回滚一致性：通过
- 截图路径：待补充
