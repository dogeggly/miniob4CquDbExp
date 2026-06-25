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

# 二、向量距离计算函数与类型转换函数实现日志

## 概述

在 MiniOB 中实现了向量相关的三个标量函数，并建立了通用的标量函数表达式系统：
- `DISTANCE(vec1, vec2, method)` — 计算两个向量的距离，支持 COSINE、DOT、EUCLIDEAN（L2）度量
- `STRING_TO_VECTOR(string)` — 将字符串转换为向量（从解析器硬编码升级为正式函数）
- `VECTOR_TO_STRING(vector)` — 将向量转换为字符串

## 架构设计

MiniOB 原先没有通用的标量函数系统。本次实现引入 `ExprType::UNBOUND_FUNCTION` 和 `ExprType::FUNCTION` 两个新表达式类型，参照 `ArithmeticExpr`（二元运算）和 `UnboundAggregateExpr`/`AggregateExpr`（聚合函数）的模式建立了标量函数表达式层级：

- **解析阶段**：语法规则 `function_expression` 匹配所有 `ID(...)` 调用（支持 1/2/3 个参数），创建 `UnboundFunctionExpr`
- **绑定阶段**：`ExpressionBinder::bind_unbound_function_expression()` 按函数名分发：聚合函数（COUNT/SUM/AVG/MAX/MIN）委托给现有逻辑；标量函数（DISTANCE/STRING_TO_VECTOR/VECTOR_TO_STRING）创建 `FunctionExpr`
- **执行阶段**：`FunctionExpr::get_value()` 根据函数类型调用对应的私有求值方法

## 改动文件清单

### 1. `src/observer/common/type/vector_type.h` — 修改
- 新增 `cosine_distance`、`dot_product`、`euclidean_distance` 三个静态距离计算方法声明

### 2. `src/observer/common/type/vector_type.cpp` — 修改
- **`cosine_distance()`**: 计算 `1 - cos_similarity`，零向量（范数 < 1e-8）返回 `RC::INVALID_ARGUMENT`，结果 clamp 到 [0, 2]（因浮点误差可能超过 |1| 的余弦相似度）
- **`dot_product()`**: 计算 `-sum(a[i] * b[i])`，取负使越小越相似（距离度量语义）
- **`euclidean_distance()`**: 计算 `sum((a[i] - b[i])^2)`，返回 L2 平方（无 sqrt，保持距离排序性，提升性能）

### 3. `src/observer/sql/expr/expression.h` — 修改
- 在 `ExprType` 枚举新增 `UNBOUND_FUNCTION` 和 `FUNCTION`
- 新增 `UnboundFunctionExpr` 类：存储函数名字符串和 `vector<unique_ptr<Expression>> children_`
- 新增 `FunctionExpr` 类：含 `Type` 枚举（DISTANCE, VECTOR_TO_STRING, STRING_TO_VECTOR）、`vector<unique_ptr<Expression>> children_`、`get_value()`/`try_get_value()`/`get_column()`、静态 `type_from_string()`、三个私有求值方法

### 4. `src/observer/sql/expr/expression.cpp` — 修改
- 实现 `UnboundFunctionExpr` 构造、拷贝
- 实现 `FunctionExpr` 构造、拷贝、`equal()`、`value_type()`、`value_length()`、`type_from_string()`
- 实现 `FunctionExpr::get_value()` 的分发逻辑
- **`eval_distance()`**: 求值 3 个子表达式 → 校验前两个为 VECTOR 类型且维度一致 → 根据第三个参数（method 字符串，大小写不敏感）调用对应距离方法 → 返回 FLOAT
- **`eval_string_to_vector()`**: 求值 1 个子表达式 → 获取字符串 → 调用 `DataType::type_instance(VECTORS)->set_value_from_str()`
- **`eval_vector_to_string()`**: 求值 1 个子表达式 → 校验 VECTOR 类型 → 调用 `DataType::type_instance(VECTORS)->to_string()` → 返回 CHAR
- `try_get_value()` 返回 `RC::UNIMPLEMENTED`（标量函数的常量折叠暂不实现）
- `get_column()` 返回 `RC::UNIMPLEMENTED`（标量函数的向量化执行暂不实现）

### 5. `src/observer/sql/parser/yacc_sql.y` — 修改
- **移除** `create_aggregate_expression` 辅助函数（被 `UnboundFunctionExpr` 构造取代）
- **移除** `value` 规则中硬编码的 `STRING_TO_VECTOR` 特例（`ID LBRACE SSS RBRACE`），改由 `function_expression` 统一处理
- **替换** `expression` 规则中的 `aggregate_expression` 引用为 `function_expression`
- **替换**原单参数 `aggregate_expression` 规则为多参数 `function_expression` 规则，支持 1/2/3 个参数：
  ```
  function_expression:
      ID LBRACE expression RBRACE
    | ID LBRACE expression COMMA expression RBRACE
    | ID LBRACE expression COMMA expression COMMA expression RBRACE
  ```
  每个产生式创建 `UnboundFunctionExpr` 并调用 `set_name()`

### 6. `src/observer/sql/parser/expression_binder.h` — 修改
- 新增 `bind_unbound_function_expression()` 方法声明

### 7. `src/observer/sql/parser/expression_binder.cpp` — 修改
- 在 `bind_expression()` 的 switch 分发中添加 `case ExprType::UNBOUND_FUNCTION`
- 实现 `bind_unbound_function_expression()`:
  1. 检查函数名是否匹配聚合函数（COUNT/SUM/AVG/MAX/MIN）→ 若匹配，创建 `UnboundAggregateExpr` 并委托给 `bind_aggregate_expression()`（保证向后兼容）
  2. 检查函数名是否匹配标量函数（DISTANCE/STRING_TO_VECTOR/VECTOR_TO_STRING）→ 绑定所有子表达式 → 创建 `FunctionExpr`
  3. 都不匹配则返回 `RC::INVALID_ARGUMENT`

### 8. `src/observer/sql/expr/expression_iterator.cpp` — 修改
- 新增 `case ExprType::FUNCTION`：遍历 `children_` 对每个调用 callback
- 新增 `ExprType::UNBOUND_FUNCTION` 和 `ExprType::UNBOUND_AGGREGATION` 到空操作分支

### 9. `src/observer/sql/optimizer/expression_rewriter.cpp` — 修改
- 新增 `case ExprType::FUNCTION`：递归改写所有子表达式

## 距离计算方法详解

| 方法 | SQL 名称 | 公式 | 返回值范围 |
|------|----------|------|-----------|
| 余弦距离 | `'cosine'` | `1 - (a·b)/(|a||b|)` | [0, 2] |
| 点积距离 | `'dot'` | `-sum(a[i]*b[i])` | (-∞, +∞) |
| 欧几里得距离 | `'euclidean'` 或 `'l2'` | `sum((a[i]-b[i])^2)` | [0, +∞) |

- 余弦距离：零向量触发 `RC::INVALID_ARGUMENT` 错误
- 点积距离：取负使正值点积表示相似（距离小）
- 欧几里得距离：返回 L2 平方（无开方），保持距离排序性

## 类型转换函数详解

| 函数 | 输入 | 输出 | 说明 |
|------|------|------|------|
| `STRING_TO_VECTOR(str)` | CHAR/VARCHAR | VECTOR | 解析 `[1,2,3]` 格式的字符串，支持空白修剪 |
| `VECTOR_TO_STRING(vec)` | VECTOR | CHAR | 输出 `[1.0,2.0,3.0]` 格式字符串 |

## 函数表达式类型系统

| ExprType | 对应类 | 用途 |
|----------|--------|------|
| `UNBOUND_FUNCTION` | `UnboundFunctionExpr` | 解析阶段：存储函数名和子表达式 |
| `FUNCTION` | `FunctionExpr` | 运行时：按类型枚举分发到具体求值 |

## 功能支持范围

| 功能 | 支持情况 |
|------|---------|
| `DISTANCE(v1, v2, 'cosine')` | ✅ |
| `DISTANCE(v1, v2, 'dot')` | ✅ |
| `DISTANCE(v1, v2, 'euclidean')` | ✅ |
| `DISTANCE(v1, v2, 'l2')` | ✅ |
| `STRING_TO_VECTOR('[1,2,3]')` | ✅ (从解析器特例迁移为正式函数) |
| `STRING_TO_VECTOR(column)` | ✅ (参数可以是任意 CHAR 表达式) |
| `VECTOR_TO_STRING(vec)` | ✅ |
| 距离方法大小写不敏感 | ✅ |
| 维度不匹配错误 | ✅ |
| 无效方法字符串错误 | ✅ |
| 未知函数名错误 | ✅ |
| 参数数量错误 | ✅ |
| 零向量余弦距离错误 | ✅ |
| DISTANCE 返回值类型 (FLOAT) | ✅ |
| VECTOR_TO_STRING 返回值类型 (CHAR) | ✅ |
| STRING_TO_VECTOR 返回值类型 (VECTOR) | ✅ |
| 函数嵌套（如 DISTANCE 内嵌 STRING_TO_VECTOR） | ✅ |
| 标量函数常量折叠 | ✅ |
| 标量函数向量化执行 | ❌ 暂不支持 |

## 测试

功能测试脚本位于 `test/vector_function_test.sh`，覆盖 STRING_TO_VECTOR、VECTOR_TO_STRING、DISTANCE 的表达式上下文用法（CALC/SELECT）、结合表数据查询、全部三个距离度量的大小写不敏感变体、维度不匹配/无效方法/零向量/类型错误/参数数量/未知函数/无效向量字符串等错误路径，以及多函数并列和跨函数组合场景。

```bash
# 在 Docker 容器的 /workspace/miniob 目录下运行，完成编译并测试
bash test/vector_function_test.sh --rebuild
```

脚本通过 TCP plain 协议与 observer 通信，启动前会清理旧数据和端口残留进程，每次运行均为干净环境。

## 已知问题 / 待完成

1. **向量化执行**: `get_column()` 返回 `RC::UNIMPLEMENTED`，函数表达式在列上逐行求值，无法利用 SIMD 加速

## 附带修复

### `src/observer/net/plain_communicator.cpp` — 修改
修复了 CALC 执行时错误被静默吞掉的框架缺陷：`write_result_internal()` 在 header 已发送后若 `write_tuple_result()` 返回错误，原代码仅 `return rc` 而不调用 `write_state()`，导致客户端收到 header 但没有 FAILURE 消息。改为调用 `sql_result->set_return_code(rc)` 后 `write_state()`，使 CALC 运行时错误正确输出错误信息。

# 三、向量距离精确查询与排序实现日志

## 概述

在 MiniOB 中实现了向量距离精确查询与排序功能，包括：
- 在 SELECT 语句中使用 DISTANCE 函数计算向量距离
- 自动维度校验（表字段维度 vs 查询向量维度）
- AS 别名支持（为列和 DISTANCE 结果指定别名）
- ORDER BY 排序支持（对原始字段、DISTANCE 结果及别名进行排序）

## 架构设计

整体管线（SELECT with ORDER BY）：

```
ProjectPhysicalOperator (计算 SELECT 表达式)
  |
OrderByPhysicalOperator (排序原始元组)
  |
... (表扫描 / 谓词过滤 / GROUP BY)
```

ORDER BY 算子位于投影算子和表扫描之间，对原始元组按排序表达式求值后排序，投影算子对排序后的元组计算 SELECT 表达式。

别名解析在绑定阶段进行：ORDER BY 子句中引用 SELECT 别名的表达式会被替换为对应的 SELECT 表达式副本。

## 改动文件清单

### 1. `src/observer/sql/parser/lex_sql.l` — 修改
- 添加 `ORDER`、`ASC`、`DESC`、`AS` 四个关键词 token

### 2. `src/observer/sql/parser/yacc_sql.y` — 修改
- **token 声明**: 添加 `ORDER`、`ASC`、`DESC`、`AS`
- **union 新增类型**: `vector<OrderBySqlNode> *` (order_by_list)、`OrderBySqlNode *` (order_by_item)
- **类型声明**: 新增 `opt_order_by`、`order_by`、`order_by_list`、`order_by_item`、`opt_asc_desc` 的 `%type`
- **select_stmt 规则**: 追加 `opt_order_by` 子句，将解析到的 `order_by` 数据移入 `SelectSqlNode`
- **expression_list → expression_as**: 引入 `expression_as` 中间规则，支持 `expression AS ID` 语法
- **expression_as 规则**: `expression AS ID` 产生式将表达式的名称设置为别名
- **opt_order_by 规则**: 可选子句，`ORDER BY order_by_list` 或空
- **order_by_list 规则**: 一个或多个 `order_by_item`，逗号分隔
- **order_by_item 规则**: `expression opt_asc_desc`，创建 `OrderBySqlNode`
- **opt_asc_desc 规则**: 空（默认 ASC）、`ASC`、`DESC`

### 3. `src/observer/sql/parser/parse_defs.h` — 修改
- 新增 `OrderBySqlNode` 结构体：包含 `unique_ptr<Expression> expression` 和 `bool is_asc`
- `SelectSqlNode` 新增 `vector<OrderBySqlNode> order_by` 字段

### 4. `src/observer/sql/stmt/select_stmt.h` — 修改
- 新增 `order_by()` 和 `order_by_asc()` 访问器
- 新增私有成员 `vector<unique_ptr<Expression>> order_by_` 和 `vector<bool> order_by_asc_`

### 5. `src/observer/sql/stmt/select_stmt.cpp` — 修改
- **别名映射构建**: 在绑定 SELECT 表达式后，构建 `alias → expression*` 的映射表
- **ORDER BY 绑定**:
  1. 遍历每个 ORDER BY 项
  2. 若为无表名的 `UnboundFieldExpr`，检查其字段名是否匹配 SELECT 别名
  3. 若匹配，用 SELECT 表达式的副本替换（`expression->copy()`）
  4. 否则，正常走 `ExpressionBinder::bind_expression()` 绑定
  5. 记录每个表达式对应的 ASC/DESC 标志

### 6. `src/observer/sql/operator/logical_operator.h` — 修改
- `LogicalOperatorType` 枚举新增 `ORDER_BY`

### 7. `src/observer/sql/operator/physical_operator.h` — 修改
- `PhysicalOperatorType` 枚举新增 `ORDER_BY`

### 8. `src/observer/sql/operator/order_by_logical_operator.h` — 新建
- `OrderByLogicalOperator` 类：存储排序表达式于基类 `expressions_`（供表达式改写器遍历），ASC/DESC 标志单独存储

### 9. `src/observer/sql/operator/order_by_logical_operator.cpp` — 新建
- 构造函数：将 ORDER BY 表达式移入基类 `expressions_`

### 10. `src/observer/sql/operator/order_by_physical_operator.h` — 新建
- `OrderByPhysicalOperator` 类：
  - `open()`: 打开子算子，读取全部元组，求值排序键，排序
  - `next()`: 按排序顺序逐行返回
  - `current_tuple()`: 返回当前 ValueListTuple 的指针

### 11. `src/observer/sql/operator/order_by_physical_operator.cpp` — 新建
- **`open()`**:
  1. 调用子算子 `open()`
  2. 循环调用 `next()` 读取全部元组，转换为 `ValueListTuple` 存储
  3. 对每个元组求值所有 ORDER BY 表达式，得到排序键向量
  4. 使用 `std::sort` 对索引按排序键比较排序
  5. 按排序后索引重排元组
- **排序比较器**: 逐列比较排序键，ASC 模式 `cmp < 0`，DESC 模式 `cmp > 0`
- **错误处理**: 表达式求值失败时使用 NULL 类型占位（排在最后）
- **`next()`**: 递增索引，超出范围返回 `RECORD_EOF`
- **`close()`**: 清空缓存，关闭子算子

### 12. `src/observer/sql/optimizer/logical_plan_generator.cpp` — 修改
- 包含 `order_by_logical_operator.h`
- `create_plan(SelectStmt*)`: 在 GROUP BY 和 PROJECT 之间插入 ORDER BY 逻辑算子
  - 若 `order_by` 非空，创建 `OrderByLogicalOperator`，将当前算子链作为其子节点
  - 然后创建 `ProjectLogicalOperator` 在 ORDER BY 之上

### 13. `src/observer/sql/optimizer/physical_plan_generator.h` — 修改
- 前向声明 `OrderByLogicalOperator`
- 新增 `create_plan(OrderByLogicalOperator &, ...)` 方法声明

### 14. `src/observer/sql/optimizer/physical_plan_generator.cpp` — 修改
- 包含 `order_by_logical_operator.h` 和 `order_by_physical_operator.h`
- `create()` 分发新增 `case LogicalOperatorType::ORDER_BY`
- 新增 `create_plan(OrderByLogicalOperator &, ...)`: 创建子物理算子 → 复制 ORDER BY 表达式 → 创建 `OrderByPhysicalOperator`

### 15. `src/observer/sql/operator/logical_operator.cpp` — 修改
- `can_generate_vectorized_operator()` 新增 `case LogicalOperatorType::ORDER_BY`: 返回 false（ORDER BY 暂不使用向量化执行）

## 功能支持范围

| 功能 | 支持情况 |
|------|---------|
| `SELECT DISTANCE(v, '[...]', 'cosine') FROM t` | ✅ |
| DISTANCE 维度自动校验（表字段 vs 查询向量） | ✅ (eval_distance 中已有) |
| `SELECT expr AS alias FROM t` | ✅ |
| `SELECT DISTANCE(...) AS dist FROM t` | ✅ |
| `ORDER BY col ASC` | ✅ |
| `ORDER BY col DESC` | ✅ |
| `ORDER BY DISTANCE(v, '[...]', 'cosine')` | ✅ |
| `ORDER BY alias` (引用 SELECT 别名) | ✅ |
| 多列排序 `ORDER BY a ASC, b DESC` | ✅ |
| `SELECT * FROM t ORDER BY col` | ✅ |
| CALC 中使用 AS 别名 | ✅ |
| ORDER BY 与 GROUP BY 联合使用 | ✅ |
| ORDER BY 与 WHERE 联合使用 | ✅ |
| 向量化执行 ORDER BY | ❌ 暂不支持 |

## 测试

功能测试脚本位于 `test/vector_distance_sort_test.sh`，覆盖精确距离查询、维度校验、AS 别名、ORDER BY 排序（基础字段/ DISTANCE 表达式/别名/多列）、GROUP BY + ORDER BY 联合使用以及错误路径测试。

```bash
# 在 Docker 容器的 /workspace/miniob 目录下运行，完成编译并测试
bash test/vector_distance_sort_test.sh --rebuild
```

脚本通过 TCP plain 协议与 observer 通信，启动前会清理旧数据和端口残留进程，每次运行均为干净环境。

## 已知问题 / 待完成

1. **ORDER BY 不支持向量化执行**: `can_generate_vectorized_operator` 返回 false，ORDER BY 始终使用逐行模式
2. **ORDER BY 全量内存排序**: 当前实现将全部元组读入内存后排序，大数据量时可能导致内存不足
3. **表达式求值失败处理**: 排序键求值失败时使用 NULL 占位，可能影响排序结果的预期行为
