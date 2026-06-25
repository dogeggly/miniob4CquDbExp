#!/bin/bash
# =============================================================================
# VECTOR 函数（DISTANCE / STRING_TO_VECTOR / VECTOR_TO_STRING）功能测试脚本
#
# ⚠️  警告: 本脚本会删除 miniob/db/ 目录下所有数据，确保测试环境干净幂等。
#     如果你的 miniob/db/ 中有需要保留的数据，请先备份。
#
# 用法: 在 Docker 容器中执行
#   cd /workspace/miniob
#   bash test/vector_function_test.sh
#
# 如果已编译过，会跳过编译直接测试
# 如需重新编译:  bash test/vector_function_test.sh --rebuild
#
# 注意: 单表达式 CALC 输出没有 " | " 分隔符（仅多列才有），因此 success
#       匹配不到 pipe。所有 CALC 成功测试追加 ", 1" 作为 dummy 第二列，
#       确保输出中出现 " | " 以便 success 断言能正确匹配。
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
    echo -e "${YELLOW}=== VECTOR 函数功能测试 ===${NC}"
    echo ""

    if [ ! -f "$OBSERVER" ] || $REBUILD; then
        echo "编译 MiniOB (debug 模式)..."
        bash build.sh debug --make -j$(nproc)
        echo ""
    fi

    EXISTING_PID=$(fuser $PORT/tcp 2>/dev/null || true)
    if [ -n "$EXISTING_PID" ]; then
        echo "清理端口 $PORT 上的残留进程 (PID=$EXISTING_PID)..."
        kill $EXISTING_PID 2>/dev/null || true
        sleep 1
        fuser -k $PORT/tcp 2>/dev/null || true
        sleep 1
    fi

    DATA_DIR="miniob/db"
    if [ -d "$DATA_DIR" ]; then
        echo "清理数据目录 $DATA_DIR ..."
        rm -rf "$DATA_DIR"
    fi

    echo "启动 observer (port=$PORT)..."
    $OBSERVER -f "$OBSERVER_INI" -p $PORT -P plain &
    OBSERVER_PID=$!
    sleep 2

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
            if echo "$result" | grep -qiE "SUCCESS|\|"; then
                passed=true
            fi
            ;;
        failure)
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
    echo "--- 1. 准备数据: 建表并插入 VECTOR 数据 ---"
    echo ""

    test_case "建表: VECTOR(3)" \
        "create table t_vec (id int, v vector(3))" \
        success

    test_case "建表: VECTOR(4) (用于维度不匹配测试)" \
        "create table t_vec4 (id int, v vector(4))" \
        success

    test_case "INSERT 第1行: [1,2,3]" \
        "insert into t_vec values (1, STRING_TO_VECTOR('[1,2,3]'))" \
        success

    test_case "INSERT 第2行: [4,5,6]" \
        "insert into t_vec values (2, STRING_TO_VECTOR('[4,5,6]'))" \
        success

    test_case "INSERT 第3行: [0,0,0] (零向量)" \
        "insert into t_vec values (3, STRING_TO_VECTOR('[0,0,0]'))" \
        success

    test_case "INSERT 第4行: [-1,-2,-3] (负值向量)" \
        "insert into t_vec values (4, STRING_TO_VECTOR('[-1,-2,-3]'))" \
        success

    test_case "INSERT t_vec4第1行: [1,2,3,4]" \
        "insert into t_vec4 values (1, STRING_TO_VECTOR('[1,2,3,4]'))" \
        success

    # =================================================================
    echo "--- 2. STRING_TO_VECTOR 函数 ---"
    echo ""

    test_case "CALC STRING_TO_VECTOR 基本用法" \
        "calc STRING_TO_VECTOR('[1,2,3]'), 1" \
        success

    test_case "CALC STRING_TO_VECTOR 含空白字符自动修剪" \
        "calc STRING_TO_VECTOR('[ 1.5 , 2.5 , 3.5 ]'), 1" \
        success

    test_case "CALC STRING_TO_VECTOR 单元素向量" \
        "calc STRING_TO_VECTOR('[42]'), 1" \
        success

    test_case "CALC STRING_TO_VECTOR 含负数和零" \
        "calc STRING_TO_VECTOR('[-1.5,0,3.14]'), 1" \
        success

    # =================================================================
    echo "--- 3. VECTOR_TO_STRING 函数 ---"
    echo ""

    test_case "CALC VECTOR_TO_STRING 基本用法" \
        "calc VECTOR_TO_STRING(STRING_TO_VECTOR('[1,2,3]')), 1" \
        success

    test_case "CALC VECTOR_TO_STRING 含负数和零" \
        "calc VECTOR_TO_STRING(STRING_TO_VECTOR('[-1,0,1]')), 1" \
        success

    # =================================================================
    echo "--- 4. DISTANCE 函数 — COSINE 余弦距离 ---"
    echo ""

    test_case "DISTANCE cosine 相同向量 [1,2,3] vs [1,2,3]" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), 'cosine'), 1" \
        success

    test_case "DISTANCE cosine 正交向量 [1,0,0] vs [0,1,0]" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,0,0]'), STRING_TO_VECTOR('[0,1,0]'), 'cosine'), 1" \
        success

    test_case "DISTANCE cosine 相反向量 [1,2,3] vs [-1,-2,-3]" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[-1,-2,-3]'), 'cosine'), 1" \
        success

    test_case "DISTANCE cosine 大小写不敏感 (COSINE 全大写)" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), 'COSINE'), 1" \
        success

    test_case "DISTANCE cosine 大小写不敏感 (Cosine 混写)" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), 'Cosine'), 1" \
        success

    # =================================================================
    echo "--- 5. DISTANCE 函数 — DOT 点积距离 ---"
    echo ""

    test_case "DISTANCE dot 相同向量 [1,2,3] — 距离=-14" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), 'dot'), 1" \
        success

    test_case "DISTANCE dot 正交向量 [1,0,0] vs [0,1,0] — 距离=0" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,0,0]'), STRING_TO_VECTOR('[0,1,0]'), 'dot'), 1" \
        success

    test_case "DISTANCE dot 相反向量 [1,2,3] vs [-1,-2,-3] — 距离=14" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[-1,-2,-3]'), 'dot'), 1" \
        success

    # =================================================================
    echo "--- 6. DISTANCE 函数 — EUCLIDEAN / L2 欧几里得距离 ---"
    echo ""

    test_case "DISTANCE euclidean 相同向量 [1,2,3] — 距离=0" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), 'euclidean'), 1" \
        success

    test_case "DISTANCE euclidean 正交向量 [1,0,0] vs [0,1,0] — 距离=2" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,0,0]'), STRING_TO_VECTOR('[0,1,0]'), 'euclidean'), 1" \
        success

    test_case "DISTANCE l2 别名等价于 euclidean" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), 'l2'), 1" \
        success

    test_case "DISTANCE L2 大小写不敏感" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), 'L2'), 1" \
        success

    # =================================================================
    echo "--- 7. DISTANCE 函数 — 结合表数据 ---"
    echo ""

    test_case "DISTANCE 表列 vs 常量 STRING_TO_VECTOR (cosine)" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') from t_vec where id = 1" \
        success

    test_case "DISTANCE 表列 vs 常量 STRING_TO_VECTOR (euclidean)" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[7,8,9]'), 'euclidean') from t_vec where id = 1" \
        success

    test_case "DISTANCE dot 表列 vs 表列 (id=1 vs id=1)" \
        "select id, DISTANCE(v, v, 'dot') from t_vec where id = 1" \
        success

    test_case "DISTANCE euclidean 表列 vs 表列 (id=1)" \
        "select id, DISTANCE(v, v, 'euclidean') from t_vec where id = 1" \
        success

    test_case "DISTANCE cosine 表列 vs 表列 (id=1)" \
        "select id, DISTANCE(v, v, 'cosine') from t_vec where id = 1" \
        success

    # =================================================================
    echo "--- 8. SELECT 全表扫描含 DISTANCE ---"
    echo ""

    test_case "SELECT 所有行计算余弦距离（共4行）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'cosine') from t_vec" \
        success

    test_case "SELECT 所有行计算欧几里得距离（共4行）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') from t_vec" \
        success

    # =================================================================
    echo "--- 9. 错误处理: 维度不匹配 ---"
    echo ""

    test_case "DISTANCE cosine 维度不匹配 (3 vs 4) 应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]'), 'cosine')" \
        failure

    test_case "DISTANCE euclidean 维度不匹配 (3 vs 4) 应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]'), 'euclidean')" \
        failure

    test_case "DISTANCE dot 维度不匹配 (3 vs 4) 应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]'), 'dot')" \
        failure

    # =================================================================
    echo "--- 10. 错误处理: 无效 method 参数 ---"
    echo ""

    test_case "DISTANCE 无效方法 'manhattan' 应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), 'manhattan')" \
        failure

    test_case "DISTANCE 空方法名应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), '')" \
        failure

    # =================================================================
    echo "--- 11. 错误处理: 零向量余弦距离 ---"
    echo ""

    test_case "DISTANCE cosine 零向量 vs 非零应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[0,0,0]'), STRING_TO_VECTOR('[1,2,3]'), 'cosine')" \
        failure

    test_case "DISTANCE cosine 两个零向量应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[0,0,0]'), STRING_TO_VECTOR('[0,0,0]'), 'cosine')" \
        failure

    # =================================================================
    echo "--- 12. 错误处理: 类型错误 ---"
    echo ""

    test_case "DISTANCE 第1参数为普通字符串应报错" \
        "calc DISTANCE('hello', STRING_TO_VECTOR('[1,2,3]'), 'cosine')" \
        failure

    test_case "DISTANCE 第2参数为普通字符串应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), 'world', 'cosine')" \
        failure

    test_case "VECTOR_TO_STRING 参数不是向量应报错" \
        "calc VECTOR_TO_STRING('not_a_vector')" \
        failure

    # =================================================================
    echo "--- 13. 错误处理: 参数数量错误 / 未知函数 ---"
    echo ""

    test_case "未知函数名 UNKNOWN_FUNC 应报错" \
        "calc UNKNOWN_FUNC(1)" \
        failure

    test_case "DISTANCE 参数不足（仅2个）应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'))" \
        failure

    test_case "DISTANCE 参数过多（4个）应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3]'), 'cosine', 99)" \
        failure

    test_case "STRING_TO_VECTOR 无参数应报错" \
        "calc STRING_TO_VECTOR()" \
        failure

    test_case "VECTOR_TO_STRING 无参数应报错" \
        "calc VECTOR_TO_STRING()" \
        failure

    # =================================================================
    echo "--- 14. 错误处理: 无效向量字符串 ---"
    echo ""

    test_case "STRING_TO_VECTOR 无反括号应报错" \
        "calc STRING_TO_VECTOR('1,2,3')" \
        failure

    test_case "STRING_TO_VECTOR 右括号不闭合应报错" \
        "calc STRING_TO_VECTOR('[1,2,3')" \
        failure

    test_case "STRING_TO_VECTOR 空括号应报错" \
        "calc STRING_TO_VECTOR('[]')" \
        failure

    test_case "STRING_TO_VECTOR 含非数字内容应报错" \
        "calc STRING_TO_VECTOR('[1,abc,3]')" \
        failure

    # =================================================================
    echo "--- 15. 边界场景与跨函数组合 ---"
    echo ""

    test_case "CALC 多函数同列求值" \
        "calc VECTOR_TO_STRING(STRING_TO_VECTOR('[4,5,6]')), DISTANCE(STRING_TO_VECTOR('[1,0,0]'), STRING_TO_VECTOR('[0,1,0]'), 'dot')" \
        success

    test_case "SELECT 中 STRING_TO_VECTOR 和 VECTOR_TO_STRING 并列" \
        "select id, STRING_TO_VECTOR('[8,8,8]'), VECTOR_TO_STRING(v) from t_vec where id = 1" \
        success

    test_case "DISTANCE euclidean 零向量 vs 负值向量" \
        "calc DISTANCE(STRING_TO_VECTOR('[0,0,0]'), STRING_TO_VECTOR('[-3,-4,0]'), 'euclidean'), 1" \
        success

    test_case "DISTANCE dot 负值向量 vs 正值向量" \
        "calc DISTANCE(STRING_TO_VECTOR('[-1,-2,-3]'), STRING_TO_VECTOR('[1,2,3]'), 'dot'), 1" \
        success

    # =================================================================
    echo "============================================"
    TOTAL=$((PASS + FAIL))
    echo -e "  总计: $TOTAL"
    echo -e "  ${GREEN}通过: $PASS${NC}"
    echo -e "  ${RED}失败: $FAIL${NC}"
    echo "============================================"

    if [ $FAIL -gt 0 ]; then
        exit 1
    fi
}

setup
run_tests
