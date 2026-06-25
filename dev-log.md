# 一、VECTOR 数据类型实现日志

## 概述

在 MiniOB 中实现了 VECTOR（向量）数据类型的存储支持。向量类型用于存储浮点数数组，默认维度为 2048，最大维度为 16383。

## 改动文件清单

### 1. `src/observer/common/type/vector_type.h` — 修改
- 扩展 `VectorType` 类声明，添加 `compare`、`to_string`、`set_value_from_str`、`cast_cost` 方法声明
- 添加 `DEFAULT_DIM = 2048` 和 `MAX_DIM = 16383` 常量

### 2. `src/observer/common/type/vector_type.cpp` — 新建
- **`compare()`**: 两个 VECTOR 逐元素比较（使用浮点 epsilon=1e-6），维度不同直接返回 -1；全部元素相等返回 0
- **`to_string()`**: 将向量转换为 `[1.0,2.0,3.0]` 格式的字符串
- **`set_value_from_str()`**: 从 `[1,2,3]` 格式的字符串解析向量，支持浮点数和空白
- **`cast_cost()`**: 仅支持 VECTOR → VECTOR（代价 0），其他类型返回 INT32_MAX

### 3. `src/observer/common/value.h` — 修改
- 添加 `set_vector(const float *data, int dim)` 方法声明

### 4. `src/observer/common/value.cpp` — 修改
- **拷贝构造 / 拷贝赋值**: 添加 VECTORS 分支，深拷贝浮点数组
- **`reset()`**: 添加 VECTORS 分支，`delete[]` 释放浮点数组内存
- **`set_data()`**: 添加 VECTORS 分支，从字节数据构造向量（length 为字节长度，除以 sizeof(float) 得到维度）
- **`data()`**: VECTORS 返回 `pointer_value_`（指向浮点数组）
- **`set_value()`**: 添加 VECTORS 分支
- **`set_vector()`**: 新增方法，分配并拷贝浮点数组，`length_` 存储维度（元素个数）

### 5. `src/observer/sql/parser/yacc_sql.y` — 修改
- **头文件**: 添加 `#include "common/type/data_type.h"`
- **`attr_def` 规则**: 新增两个 VECTOR 专用产生式：
  - `ID VECTOR_T LBRACE number RBRACE`: VECTOR(N)，校验 N 在 [1, 16383] 范围，`length = N * sizeof(float)`
  - `ID VECTOR_T`: VECTOR（无括号），默认维度 2048，`length = 2048 * sizeof(float)`
- **`type` 规则**: 移除 `VECTOR_T`（因为 VECTOR 由专用规则处理，避免歧义）
- **`value` 规则**: 新增 `ID LBRACE SSS RBRACE` 产生式，识别 `STRING_TO_VECTOR('[1,2,3]')` 语法，调用 `VectorType::set_value_from_str` 解析向量值

### 6. `src/observer/sql/stmt/create_table_stmt.cpp` — 修改
- 在 `CreateTableStmt::create()` 中添加 VECTOR 维度校验：`dim = length / sizeof(float)` 必须在 [1, 16383] 范围内

### 7. `src/observer/storage/table/table.cpp` — 修改
- 在 `Table::make_record()` 中添加 VECTOR 维度校验：插入向量的维度（`value.length()`）必须与字段定义的维度（`field->len() / sizeof(float)`）一致

### 8. `src/observer/sql/optimizer/logical_plan_generator.cpp` — 修改
- 在 `create_plan(FilterStmt*)` 中添加 VECTOR 比较限制：如果比较的任一侧是 VECTOR 类型，只允许 `=` 和 `!=` 操作，`<`、`>`、`<=`、`>=` 将返回 `RC::UNSUPPORTED`

### 9. `src/observer/sql/expr/expression.cpp` — 修改
- 在 `ComparisonExpr::eval()` 中，将 VECTORS 与 CHARS 一起处理为逐行比较模式（因为 VECTOR 是变长类型，不适合 SIMD 批量比较）

### 10. `src/observer/storage/common/column.cpp` — 修改
- **`Column::init(const Value&, size_t)`**: VECTOR 类型时，`attr_len_` 设置为 `value.length() * sizeof(float)`（字节长度），而非元素个数
- **`Column::append_value()`**: VECTOR 类型时，正确计算字节长度进行 memcpy

## 数据存储方案

- **记录中**: VECTOR(N) 在记录中占用 `N * sizeof(float)` 字节，连续存储 N 个 float
- **Value 类**: `length_` 存储维度（元素个数），`value_.pointer_value_` 指向堆分配的 float 数组，`own_data_ = true`
- **Column 类**: `attr_len_` 存储字节长度（`N * sizeof(float)`）

## 功能支持范围

| 功能 | 支持情况 |
|------|---------|
| `VECTOR` (无括号，默认 2048 维) | ✅ |
| `VECTOR(N)` (N ∈ [1, 16383]) | ✅ |
| `VECTOR()` (空括号) | ✅ 解析错误 |
| `VECTOR(0)` 或 `VECTOR(>16383)` | ✅ 报错 |
| CREATE TABLE with VECTOR | ✅ |
| INSERT with STRING_TO_VECTOR | ✅ |
| 插入向量维度校验 | ✅ |
| VECTOR = VECTOR | ✅ |
| VECTOR != VECTOR | ✅ |
| VECTOR < VECTOR | ❌ 不支持 |
| VECTOR 与 INT/FLOAT/CHAR 比较 | ❌ 不支持 |
| VECTOR 算术运算 | ❌ 不支持 |
| VECTOR 聚合函数 | ❌ 不支持 |
| LOAD DATA 导入 VECTOR | ✅ (文件值需为 `[1,2,3]` 格式) |

## 测试

功能测试脚本位于 `test/vector_test.sh`，覆盖 DDL（建表/维度校验）、DML（INSERT/维度校验）、SELECT 查询、VECTOR =/!= 比较及不支持操作（<、>、<=、>=）的错误处理。

```bash
# 在 Docker 容器的 /workspace/miniob 目录下运行，完成编译并测试
bash test/vector_test.sh --rebuild
```

脚本通过 TCP plain 协议与 observer 通信，启动前会清理旧数据和端口残留进程，每次运行均为干净环境。

## 已知问题 / 待完成

1. **VECTOR 不支持索引**: `codec.h` 中 `encode_value` 未处理 VECTORS，尝试对 VECTOR 列建索引会返回 `RC::INVALID_ARGUMENT`
2. **向量长度上限与页面大小的关系**: VECTOR(16383) 在记录中占用 65532 字节，可能超过默认页面大小，需要在实际使用中注意
