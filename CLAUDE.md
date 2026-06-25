# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目简介

MiniOB 是 OceanBase 团队联合多所高校开发的数据库入门学习项目，目标是帮助零基础的同学学习数据库内核实现。代码简化了并发操作、安全特性和复杂事务管理，聚焦于数据库核心原理。项目使用 **木兰宽松许可证 v2**。

## 构建与运行

### 首次初始化（安装第三方依赖）

```bash
bash build.sh init
```

这会编译 libevent、googletest、benchmark、jsoncpp、replxx 等第三方库到 `deps/3rd/usr/local/`。

### 编译

```bash
# Debug 模式（默认）
bash build.sh debug --make -j$(nproc)

# Release 模式
bash build.sh release --make -j$(nproc)
```

构建目录为 `build_debug/` 或 `build_release/`，默认符号链接 `build -> build_debug`。服务端二进制为 `build/bin/observer`。

### 运行服务端

```bash
# TCP plain 协议（默认 6789 端口）
./build/bin/observer -f etc/observer.ini

# MySQL 协议（兼容 MySQL 客户端连接）
./build/bin/observer -f etc/observer.ini -P mysql

# CLI 模式（标准输入输出，用于调试）
./build/bin/observer -f etc/observer.ini -P cli

# 指定存储引擎为 LSM
./build/bin/observer -f etc/observer.ini -E lsm
```

关键命令行参数：`-p` 端口、`-P` 协议(plain/mysql/cli)、`-t` 事务模型(vacuous/mvcc)、`-E` 存储引擎(heap/lsm)、`-d` 持久化模式(vacuous/disk)。

### 运行客户端

```bash
./build/bin/obclient                 # plain 协议
./build/bin/obclient -P mysql         # MySQL 协议（不指定则为 mysql）
```

### 运行测试

```bash
# 单元测试（需要先编译 Debug 模式）
cd build_debug && ctest

# 运行单个单元测试
./build_debug/unittest/ob_lsm_test

# 功能测试（需要启动 observer 服务端后）
cd test/case && python3 miniob_test.py --test-cases=basic

# 集成测试框架（同时对比 MiniOB 和 MySQL 的结果）
cd test/integration_test
# 详见 test/integration_test/README.md
```

### CMake 配置选项

关键 CMake 选项（可在 `bash build.sh debug --make` 后追加，如 `bash build.sh debug --make -DENABLE_ASAN=OFF`）：

- `ENABLE_ASAN` (ON) — Address Sanitizer
- `WITH_UNIT_TESTS` (ON) — 编译单元测试
- `WITH_BENCHMARK` (OFF) — 编译性能基准测试
- `CONCURRENCY` (OFF) — 启用并发支持
- `USE_SIMD` (OFF) — 启用 SIMD 加速
- `ENABLE_COVERAGE` (OFF) — 启用代码覆盖率

## 架构概览

MiniOB 是一个经典的数据库系统，SQL 查询经过以下流水线处理：

```
客户端请求 → 网络模块 → SQL 解析(Parser) → 语义解析(Resolver) → 查询优化(Optimizer) → 计划执行(Executor) → 存储引擎(Storage Engine)
```

### 源码目录结构

```
src/
├── common/          # 公共基础库（日志、IO、线程、内存管理、数学工具等）
├── observer/        # 数据库服务端核心（main.cpp 入口）
│   ├── net/         # 网络模块：TCP/Unix Socket、MySQL协议、CLI协议
│   ├── session/     # 会话管理
│   ├── sql/         # SQL 处理流水线
│   │   ├── parser/  # 词法/语法解析（flex lex_sql.l + bison yacc_sql.y）
│   │   ├── stmt/    # 语义解析：将语法树转为内部 Stmt 结构
│   │   ├── expr/    # 表达式系统（算数、比较、聚合、元组等）
│   │   ├── operator/# 算子（物理/逻辑算子，含向量化执行）
│   │   ├── optimizer/ # 查询优化器（谓词下推、连接重写、Cascade 优化器框架）
│   │   ├── executor/  # 计划执行器（各类 SQL 语句的执行实现）
│   │   ├── plan_cache/ # 计划缓存
│   │   └── query_cache/ # 查询结果缓存
│   ├── storage/      # 存储引擎
│   │   ├── common/   # 存储公共数据结构（Column、Chunk、Codec、Arena）
│   │   ├── record/   # 记录管理器（堆表/LSM 记录扫描器）
│   │   ├── table/    # 表引擎（HeapTableEngine、LsmTableEngine）
│   │   ├── index/    # 索引（B+ Tree、IVFFlat 向量索引）
│   │   ├── db/       # 数据库实例管理
│   │   ├── buffer/   # Buffer Pool（双层写缓冲区、磁盘缓冲池）
│   │   ├── clog/     # Redo Log（日志缓冲区、日志文件、日志回放）
│   │   ├── trx/      # 事务管理（MVCC、LSM MVCC、空事务）
│   │   ├── persist/  # 持久化
│   │   └── field/    # 字段元数据
│   ├── catalog/      # 元数据管理（表、索引、字段元信息）
│   ├── event/        # 事件系统（SQL 事件、会话事件）
│   └── common/       # observer 专用公共代码（全局上下文、Value 类型系统）
├── obclient/         # 客户端工具（连接 observer）
├── oblsm/            # LSM-Tree 存储引擎实现
│   ├── memtable/     # 内存表（跳表 SkipList）
│   ├── compaction/   # 压缩合并
│   ├── table/        # SSTable
│   ├── wal/          # Write-Ahead Log
│   └── util/         # 工具（Bloom Filter、LRU Cache、Arena）
├── cpplings/         # C++ 练习题（互斥锁、原子变量、智能指针等）
└── memtracer/        # 内存追踪调试工具
```

### SQL 处理流水线

1. **Parser** (`sql/parser/`): Flex 词法分析 + Bison 语法分析 → `ParsedSqlNode` 语法树
2. **Resolver** (`sql/parser/resolve_stage.cpp`): 语法树 → `Stmt` 语义结构（位于 `sql/stmt/`）
3. **Optimizer** (`sql/optimizer/`): 改写规则（谓词下推、连接简化）+ Cascade 成本优化器
4. **Executor** (`sql/executor/`): 每种 SQL 命令对应一个 Executor（如 `CreateTableExecutor`）
5. **Operator** (`sql/operator/`): 物理执行算子（TableScan、IndexScan、Join、Project、Aggregate、GroupBy 等），包含向量化版本（`*_vec_physical_operator`）

### 存储引擎

- **Heap 引擎**（默认）: 简单的堆表存储，记录顺序追加
- **LSM 引擎** (`oblsm/`): Log-Structured Merge Tree，支持 MVCC 事务
- **B+ Tree 索引**: `storage/index/bplus_tree.*` 实现
- **Buffer Pool**: `storage/buffer/` 管理页缓存
- **Redo Log**: `storage/clog/` 实现 WAL，支持磁盘持久化和日志回放
- **事务**: MVCC 模式使用多版本并发控制，默认 vacuous 模式无事务支持

### 关键设计文档

`docs/docs/design/` 目录下有详细的架构设计文档：
- `miniob-architecture.md` — 整体架构
- `miniob-sql-parser.md` — SQL 解析器设计
- `miniob-sql-execution-process.md` — SQL 执行流程
- `miniob-buffer-pool.md` — Buffer Pool 设计
- `miniob-bplus-tree.md` — B+ 树索引
- `miniob-lsm-tree.md` — LSM-Tree 存储
- `miniob-transaction.md` — 事务管理
- `miniob-cascade.md` — Cascade 优化器
- `miniob-how-to-add-new-sql.md` — 如何添加新 SQL 语句
- `miniob-how-to-add-new-datatype.md` — 如何添加新数据类型

### 测试架构

- **单元测试** (`unittest/`): 基于 Google Test，每个模块独立测试（如 `bplus_tree_test`、`parser_test`、`ob_lsm_test`）
- **功能测试** (`test/case/`): Python 自动化测试框架，通过 SQL 文件驱动
- **集成测试** (`test/integration_test/`): 同时在 MiniOB 和 MySQL 上运行相同 SQL 并对比结果，也支持性能测试

## 代码风格

- C++20 标准
- 列宽 120 字符，缩进 2 空格（不使用 Tab）
- 函数/类/结构体/枚举的大括号另起一行，控制流语句不另起
- 指针/引用靠右：`int *p`（但 `PointerBindsToType: true` 实际为 `int* p` 风格）
- 日志宏定义在 `src/common/log/log.h`，使用 `LOG_INFO`、`LOG_WARN`、`LOG_ERROR`、`LOG_DEBUG`、`LOG_TRACE`、`LOG_PANIC`
- `WhitespaceSensitiveMacros` 配置了 LOG_* 和 ASSERT 宏避免格式化错误
- 格式化工具：`clang-format --style=file:.clang-format`

## 添加新 SQL 语句的典型步骤

参考 `docs/docs/design/miniob-how-to-add-new-sql.md`，大致流程：
1. 修改 `yacc_sql.y` 添加语法规则
2. 在 `parse_defs.h` 添加新的 SqlNode 结构体
3. 在 `stmt/` 添加对应的 Stmt 类
4. 在 `resolve_stage.cpp` 中添加 Resolver 逻辑
5. 在 `executor/` 添加 Executor
6. 在 `operator/` 添加相关算子（如需要）
7. 在 `execute_stage.cpp` 中注册新的执行逻辑
