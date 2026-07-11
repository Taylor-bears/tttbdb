# T03_BIGINT类型_交接

## 1. 基本信息

- 题目编号：T03
- 题目名称：BIGINT 类型
- 负责人：邵云慧
- 当前状态：已通过
- 最近更新时间：2026-07-11

## 2. 本题做了什么

本题为 RMDB 新增了 8 字节有符号 `BIGINT` 类型，支持范围为 `-9223372036854775808` 至 `9223372036854775807`。系统现可在 BIGINT 字段上执行建表、插入、条件查询、更新和删除，并能正确持久化元数据与记录。超出 64 位有符号整数范围的字面量会被解析器拒绝并输出 `failure`，不会写入错误数据。希冀平台一次性全部通过。

## 3. 主要修改文件

| 文件路径 | 改动说明 |
|---|---|
| `src/defs.h` | 新增 `TYPE_BIGINT` 字段类型 |
| `src/common/common.h` | 新增 BIGINT 值及 8 字节记录编码 |
| `src/parser/ast.h` | 新增 BIGINT 类型和字面量节点 |
| `src/parser/lex.l` | 使用 `strtoll` 解析整数并检查 64 位溢出 |
| `src/parser/yacc.y` | 支持 `BIGINT` 建表语法及整数类型判定 |
| `src/analyze/analyze.cpp` | 支持 BIGINT 语义检查及 INT 常量提升 |
| `src/execution/executor_abstract.h` | 支持 BIGINT 条件比较 |
| `src/execution/execution_manager.cpp` | 支持 BIGINT 查询结果输出 |
| `src/query_execution_test.cpp` | 增加 BIGINT 边界和增删改查测试 |

## 4. 核心实现逻辑

1. 词法分析使用 `strtoll` 读取整数，先判断是否超出 `int64_t` 范围。
2. 处于 INT 范围的字面量生成 `IntLit`，更大的合法整数生成 `BigIntLit`。
3. BIGINT 字段固定占 8 字节，值通过 `memcpy` 编解码，避免未对齐内存访问。
4. 对 BIGINT 字段使用普通 INT 常量时，语义分析阶段自动提升为 BIGINT。
5. 扫描算子按有符号 64 位整数完成比较，查询输出使用十进制完整表示。

## 5. 关键注意事项

- `TYPE_BIGINT` 添加在原字段类型枚举末尾，避免改变已有元数据中 INT、FLOAT 和 STRING 的编号。
- 最大值和最小值必须都能解析；超出任一边界必须返回 `failure`。
- 不能继续使用 `atoi`，否则大整数会在进入执行层之前被截断。
- 后续索引功能若用于 BIGINT 字段，应保留 `ix_compare` 中的 64 位比较逻辑。

## 6. 快速验证方式

```bash
mkdir -p build && cd build
cmake ..
make query_execution_test unit_test -j4
./bin/query_execution_test
./bin/unit_test
```

## 7. 测试结果记录

- 希冀平台：一次性全部通过
- 通过时间：2026-07-11
- 本地题目二、三查询测试：7/7 通过
- 本地题目一存储回归：5/5 通过
- 截图路径：待补充
