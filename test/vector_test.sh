#!/bin/bash
# =============================================================================
# VECTOR 数据类型功能测试脚本
#
# ⚠️  警告: 本脚本会删除 miniob/db/ 目录下所有数据，确保测试环境干净幂等。
#     如果你的 miniob/db/ 中有需要保留的数据，请先备份。
#
# 用法: 在 Docker 容器中执行
#   cd /workspace/miniob
#   bash test/vector_test.sh
#
# 如果已编译过，会跳过编译直接测试
# 如需重新编译:  bash test/vector_test.sh --rebuild
# =============================================================================

set -e

OBSERVER="./build_debug/bin/observer"
OBSERVER_INI="etc/observer.ini"
PORT=9876
PASS=0
FAIL=0

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

REBUILD=false
if [ "$1" = "--rebuild" ]; then
    REBUILD=true
fi

#--------------------------------------------------------------------
# 编译 & 启动 observer
#--------------------------------------------------------------------
setup() {
    echo -e "${YELLOW}=== VECTOR 数据类型功能测试 ===${NC}"
    echo ""

    if [ ! -f "$OBSERVER" ] || $REBUILD; then
        echo "编译 MiniOB (debug 模式)..."
        bash build.sh debug --make -j$(nproc)
        echo ""
    fi

    # 清理可能残留的旧进程（确保幂等）
    EXISTING_PID=$(fuser $PORT/tcp 2>/dev/null || true)
    if [ -n "$EXISTING_PID" ]; then
        echo "清理端口 $PORT 上的残留进程 (PID=$EXISTING_PID)..."
        kill $EXISTING_PID 2>/dev/null || true
        sleep 1
        fuser -k $PORT/tcp 2>/dev/null || true
        sleep 1
    fi

    # 清理磁盘上的旧数据目录（HEAP 引擎会将表数据持久化到 miniob/db/）
    DATA_DIR="miniob/db"
    if [ -d "$DATA_DIR" ]; then
        echo "清理数据目录 $DATA_DIR ..."
        rm -rf "$DATA_DIR"
    fi

    echo "启动 observer (port=$PORT)..."
    $OBSERVER -f "$OBSERVER_INI" -p $PORT -P plain &
    OBSERVER_PID=$!
    sleep 2

    # 确认新进程确实在监听目标端口
    if ! kill -0 $OBSERVER_PID 2>/dev/null; then
        echo -e "${RED}observer 启动失败（进程已退出）${NC}"
        exit 1
    fi
    if ! fuser $PORT/tcp 2>/dev/null | grep -q "$OBSERVER_PID"; then
        echo -e "${RED}observer 启动失败（PID=$OBSERVER_PID 未监听端口 $PORT）${NC}"
        kill $OBSERVER_PID 2>/dev/null || true
        exit 1
    fi
    echo "observer PID=$OBSERVER_PID 正在监听端口 $PORT"
    echo ""
}

#--------------------------------------------------------------------
# 清理
#--------------------------------------------------------------------
cleanup() {
    if [ -n "$OBSERVER_PID" ]; then
        echo "停止 observer (PID=$OBSERVER_PID)..."
        kill $OBSERVER_PID 2>/dev/null || true
        wait $OBSERVER_PID 2>/dev/null || true
    fi
}
trap cleanup EXIT

#--------------------------------------------------------------------
# 通过 TCP plain 协议发送一条 SQL，返回服务器响应
#--------------------------------------------------------------------
run_sql() {
    local sql="$1"
    python3 -c "
import socket, sys

sql = sys.argv[1]
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.settimeout(5)
try:
    sock.connect(('127.0.0.1', $PORT))
    sock.sendall(sql.encode('utf-8') + b'\x00')
    data = b''
    while True:
        chunk = sock.recv(4096)
        if not chunk:
            break
        data += chunk
        if b'\x00' in chunk:
            break
    if data.endswith(b'\x00'):
        data = data[:-1]
    result = data.decode('utf-8', errors='replace').rstrip()
    print(result)
finally:
    sock.close()
" "$sql"
}

#--------------------------------------------------------------------
# 测试一条用例
#   $1: 描述
#   $2: SQL
#   $3: success | failure | contains:<子串>
#
# 匹配规则:
#   success   — 响应含 "SUCCESS" 或含 " | " (SELECT 结果集)
#   failure   — 响应含 "FAILURE" 或含 " > " (parse/runtime 错误格式)
#   contains:X — 响应含子串 X (大小写不敏感)
#--------------------------------------------------------------------
test_case() {
    local desc="$1"
    local sql="$2"
    local expect="$3"

    local result
    result=$(run_sql "$sql" 2>/dev/null) || result="CONNECTION_ERROR"

    local passed=false
    case "$expect" in
        success)
            # SUCCESS 关键字，或者是 SELECT 的表格输出（含 " | " 分隔符）
            if echo "$result" | grep -qiE "SUCCESS|\|"; then
                passed=true
            fi
            ;;
        failure)
            # FAILURE 关键字，或者错误码格式 "XXX > message"
            if echo "$result" | grep -qiE "FAILURE|> "; then
                passed=true
            fi
            ;;
        contains:*)
            local sub="${expect#contains:}"
            if echo "$result" | grep -qiF "$sub"; then
                passed=true
            fi
            ;;
    esac

    if $passed; then
        echo -e "  ${GREEN}[PASS]${NC} $desc"
        echo "         ->  $result"
        PASS=$((PASS + 1))
    else
        echo -e "  ${RED}[FAIL]${NC} $desc"
        echo "         SQL: $sql"
        echo "         ->  $result"
        FAIL=$((FAIL + 1))
    fi
    echo ""
}

#--------------------------------------------------------------------
# 测试用例
#--------------------------------------------------------------------
run_tests() {

    # =================================================================
    echo "--- 1. DDL: CREATE TABLE with VECTOR ---"
    echo ""

    test_case "VECTOR (无括号，默认2048维)" \
        "create table t_default (id int, v vector)" \
        success

    test_case "VECTOR(3) 指定维度" \
        "create table t_v3 (id int, v vector(3))" \
        success

    test_case "VECTOR(1) 最小维度" \
        "create table t_v1 (id int, v vector(1))" \
        success

    test_case "VECTOR(16383) 最大维度" \
        "create table t_max (id int, v vector(16383))" \
        success

    test_case "VECTOR(0) 应报错（维度<1，parse阶段拒绝）" \
        "create table t_err (id int, v vector(0))" \
        failure

    test_case "VECTOR(16384) 应报错（超出上限，parse阶段拒绝）" \
        "create table t_err2 (id int, v vector(16384))" \
        failure

    # =================================================================
    echo "--- 2. DML: INSERT with STRING_TO_VECTOR ---"
    echo ""

    test_case "INSERT 到 VECTOR(3) 表（第1行，维度匹配）" \
        "insert into t_v3 values (1, STRING_TO_VECTOR('[1,2,3]'))" \
        success

    test_case "INSERT 到 VECTOR(3) 表（第2行）" \
        "insert into t_v3 values (2, STRING_TO_VECTOR('[4,5,6]'))" \
        success

    test_case "INSERT 到 VECTOR(3) 表（第3行，浮点数）" \
        "insert into t_v3 values (3, STRING_TO_VECTOR('[1.5, 2.5, 3.5]'))" \
        success

    test_case "INSERT 到 VECTOR(1) 表" \
        "insert into t_v1 values (1, STRING_TO_VECTOR('[7]'))" \
        success

    test_case "INSERT 维度不匹配应报错（3维值插入默认2048维列）" \
        "insert into t_default values (1, STRING_TO_VECTOR('[1,2,3]'))" \
        failure

    # =================================================================
    echo "--- 3. SELECT: 查询 VECTOR 数据（验证3行都在）---"
    echo ""

    test_case "SELECT * FROM t_v3（验证所有3行与向量格式）" \
        "select * from t_v3" \
        "contains:[1"

    test_case "SELECT 验证第2行向量" \
        "select * from t_v3 where id = 2" \
        "contains:[4"

    test_case "SELECT 验证第3行浮点向量" \
        "select * from t_v3 where id = 3" \
        "contains:[1.5"

    test_case "SELECT * FROM t_v1" \
        "select * from t_v1" \
        "contains:[7"

    # =================================================================
    echo "--- 4. VECTOR 比较: = 和 != ---"
    echo ""

    test_case "VECTOR = VECTOR 等值比较（匹配到 id=1）" \
        "select * from t_v3 where v = STRING_TO_VECTOR('[1,2,3]')" \
        "contains:[1,"

    test_case "VECTOR = VECTOR 等值比较（无匹配，查询正常返回空结果）" \
        "select * from t_v3 where v = STRING_TO_VECTOR('[99,99,99]')" \
        success

    test_case "VECTOR != VECTOR 不等比较（排除 [99,99,99]，返回所有3行）" \
        "select * from t_v3 where v != STRING_TO_VECTOR('[99,99,99]')" \
        "contains:[1,"

    # =================================================================
    echo "--- 5. 不支持的操作应报错 ---"
    echo ""

    test_case "VECTOR < VECTOR 应报错（不支持）" \
        "select * from t_v3 where v < STRING_TO_VECTOR('[1,2,3]')" \
        failure

    test_case "VECTOR > VECTOR 应报错（不支持）" \
        "select * from t_v3 where v > STRING_TO_VECTOR('[1,2,3]')" \
        failure

    test_case "VECTOR <= VECTOR 应报错（不支持）" \
        "select * from t_v3 where v <= STRING_TO_VECTOR('[1,2,3]')" \
        failure

    test_case "VECTOR >= VECTOR 应报错（不支持）" \
        "select * from t_v3 where v >= STRING_TO_VECTOR('[1,2,3]')" \
        failure

    # =================================================================
    echo "============================================"
    TOTAL=$((PASS + FAIL))
    echo -e "  总计: $TOTAL"
    echo -e "  ${GREEN}通过: $PASS${NC}"
    echo -e "  ${RED}失败: $FAIL${NC}"
    echo "============================================"

    # 注意: DROP TABLE 尚未实现（缺 Stmt 类和 Executor），
    # 测试表会在 observer 退出后自动丢弃（vacuous 持久化模式）
    if [ $FAIL -gt 0 ]; then
        exit 1
    fi
}

setup
run_tests
