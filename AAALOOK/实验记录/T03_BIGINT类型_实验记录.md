# T03_BIGINT类型_实验记录

## 1. 题目基本信息

- 题目编号：T03
- 题目名称：BIGINT 类型
- 负责人：邵云慧
- 完成时间：2026-07-11
- 当前状态：已通过

## 2. 题目要求概述

本题要求在 RMDB 中增加 MySQL 风格的有符号 BIGINT 类型，以 8 字节存储极大整数，支持完整的 64 位有符号范围，并保证该字段可以参与插入、查询、更新和删除。超出 BIGINT 范围的输入必须输出 `failure`，且不能影响已有数据。

## 3. 测试点情况

| 测试内容 | 是否通过 | 备注 |
|---|---|---|
| 创建 BIGINT 字段 | 是 | 字段长度为 8 字节 |
| 插入正负大整数 | 是 | 希冀平台一次通过 |
| 查询与结果输出 | 是 | 保持完整十进制精度 |
| 越界字面量 | 是 | 输出 `failure` 且不插入 |
| 条件查询、更新、删除 | 是 | 本地强化测试通过 |
| 最大值与最小值 | 是 | `INT64_MAX`、`INT64_MIN` 均通过 |
| 关闭并重新打开数据库 | 是 | 类型元数据与记录正确恢复 |

## 4. 实现方法

### 4.1 修改模块

1. 类型系统：增加 `TYPE_BIGINT`、`SV_TYPE_BIGINT` 和 `BigIntLit`。
2. Parser：识别 `BIGINT` 关键字，解析 64 位整数并检测溢出。
3. Analyze：进行 BIGINT 类型检查，并把小整数常量安全提升为 BIGINT。
4. Execution：完成 8 字节值编码、比较和输出。
5. Test：覆盖题目示例、上下界、越界、增删改查和持久化。

### 4.2 实现流程

```text
1. Lexer 使用 strtoll 解析整数字符串。
2. 若发生 ERANGE，则返回非法 token，服务端写入 failure。
3. 合法整数按数值范围生成 IntLit 或 BigIntLit。
4. Analyze 将 AST 值转换为 Value，并按目标字段完成类型提升。
5. Insert/Update 将 int64_t 写入 8 字节记录空间。
6. SeqScan 按有符号 64 位数计算 WHERE 条件。
7. SELECT 将记录中的 BIGINT 转为完整十进制字符串。
```

### 4.3 关键伪代码

```text
算法：BIGINT 字面量解析

输入：整数字符串 text
输出：IntLit、BigIntLit 或解析失败

1. 清空 errno，调用 strtoll(text)。
2. 若 errno == ERANGE 或存在未解析字符，返回非法 token。
3. 若值位于 INT 范围，创建 IntLit。
4. 否则创建 BigIntLit，保存 int64_t 值。
```

## 5. 关键修改文件

| 文件路径 | 作用 |
|---|---|
| `src/defs.h` | 定义 BIGINT 类型 |
| `src/common/common.h` | BIGINT 内存值与记录编码 |
| `src/parser/ast.h` | BIGINT AST 数据结构 |
| `src/parser/lex.l` | 64 位整数解析和溢出判断 |
| `src/parser/yacc.y` | BIGINT 语法规则 |
| `src/analyze/analyze.cpp` | 语义检查与 INT→BIGINT 提升 |
| `src/execution/executor_abstract.h` | BIGINT 条件比较 |
| `src/execution/execution_manager.cpp` | BIGINT 输出 |

## 6. 测试结果

- 希冀平台测试结果：一次性全部通过
- 通过时间：2026-07-11
- 本地题目二、三端到端测试：7/7 通过
- 本地题目一回归测试：5/5 通过
- 测试截图路径：待补充

## 7. 调试过程记录

| 问题 | 原因 | 解决方法 |
|---|---|---|
| 大整数会在解析阶段截断 | 原 Lexer 使用 `atoi`，只能保存 32 位整数 | 改用 `strtoll` 并在语义值中保存 `int64_t` |
| 越界输入可能抛异常导致服务退出 | `yyparse()` 位于服务端异常捕获范围之外 | Lexer 对溢出返回非法 token，沿现有语法失败路径输出 `failure` |
| BIGINT 字段插入小整数时报类型不匹配 | 小整数字面量仍被识别为 INT | Analyze 对目标 BIGINT 字段执行安全的 INT→BIGINT 提升 |
| 记录偏移不保证 8 字节对齐 | 直接解引用 `int64_t*` 存在未对齐访问风险 | 使用 `memcpy` 完成 BIGINT 编码、比较和读取 |

## 8. 可放入实验报告的总结

本题在 RMDB 中完整加入了 8 字节有符号 BIGINT 类型。实现从词法解析开始检测 64 位整数边界，在 AST 和语义分析阶段区分 INT 与 BIGINT，并在记录层、条件执行和结果输出中统一使用 `int64_t`。同时对越界输入采用现有非法 SQL 路径输出 `failure`，保证失败插入不会污染已有数据。最终题目在希冀平台一次性通过，且题目一、二回归测试保持通过。
