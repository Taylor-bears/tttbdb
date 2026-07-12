# T07_ORDER_BY与LIMIT_交接

## 1. 基本信息

- 题目编号：T07
- 题目名称：ORDER BY 操作符与 LIMIT
- 负责人：邵云慧
- 当前状态：已通过
- 最近更新时间：2026-07-12

## 2. 本题做了什么

本题完成了 RMDB 的 `ORDER BY` 与 `LIMIT` 功能，支持单列和多列排序，每个排序列都可以独立指定 `ASC`、`DESC`，未指定方向时默认升序。排序列不要求出现在 SELECT 投影中，`LIMIT` 可单独使用，也可在 `WHERE` 和 `ORDER BY` 之后限制最终输出行数。希冀平台一次性全部通过。

## 3. 主要修改文件

| 文件路径 | 改动说明 |
|---|---|
| `src/parser/lex.l`、`src/parser/yacc.y` | 增加 `LIMIT` 及多列 ORDER BY 语法 |
| `src/parser/ast.h` | 保存多个排序列、各列方向及 LIMIT 值 |
| `src/analyze/analyze.h`、`src/analyze/analyze.cpp` | 校验排序列、补全表名并传递排序参数 |
| `src/optimizer/plan.h`、`src/optimizer/planner.cpp` | 扩展 SortPlan，生成排序与限制计划 |
| `src/execution/execution_sort.h` | 实现稳定的多键内存排序和结果截断 |
| `src/portal.h` | 根据 SortPlan 构造 SortExecutor |
| `src/query_execution_test.cpp` | 增加 ORDER BY 与 LIMIT 专项测试 |

## 4. 核心实现逻辑

1. Parser 将排序列列表、每列方向和 LIMIT 数量写入 SelectStmt。
2. Analyze 使用统一列解析规则处理未限定列、表限定列及歧义列。
3. Planner 将 SortPlan 放在扫描/连接之后、最终 Projection 之前，因此可按未投影列排序。
4. SortExecutor 读取输入记录，使用稳定排序依次比较多个键；当前键相同时继续比较下一键。
5. 排序完成后按 LIMIT 控制可见记录数；无 ORDER BY 时保留输入顺序并直接截断。

## 5. 关键注意事项

- 多列排序必须按 SQL 中的列顺序逐级比较，每列分别应用升降序方向。
- 排序位于 Projection 之前，否则无法使用未出现在 SELECT 中的列作为排序键。
- 使用稳定排序保证所有排序键相同时维持输入记录的原始顺序。
- `LIMIT 0` 返回空结果，负数在 Parser 阶段拒绝。
- 当前实现为内存排序；若后续处理超大数据集，可扩展为外部排序。

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
- 本地查询执行测试：15/15 通过
- 题目一存储回归：5/5 通过
- Parser 测试：通过
- 覆盖边界：多列混合方向、默认升序、未投影排序列、WHERE 后排序、独立 LIMIT、LIMIT 0、负数 LIMIT
- 截图路径：待补充
