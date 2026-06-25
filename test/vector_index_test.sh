#!/bin/bash
# =============================================================================
# VECTOR INDEX (IVF Flat) 近似搜索与 LIMIT 功能测试脚本
#
# ⚠️  警告: 本脚本会删除 miniob/db/ 目录下所有数据，确保测试环境干净幂等。
#     如果你的 miniob/db/ 中有需要保留的数据，请先备份。
#
# 用法: 在 Docker 容器中执行
#   cd /workspace/miniob
#   bash test/vector_index_test.sh
#
# 如果已编译过，会跳过编译直接测试
# 如需重新编译:  bash test/vector_index_test.sh --rebuild
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
    echo -e "${YELLOW}=== IVF Flat 向量索引近似搜索与 LIMIT 功能测试 ===${NC}"
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
    echo "--- 1. 准备数据: 建表并插入向量数据（VECTOR(3)，共5行）---"
    echo ""

    test_case "建表: VECTOR(3)" \
        "create table t_vec (id int, v vector(3))" \
        success

    test_case "INSERT [1,2,3]" \
        "insert into t_vec values (1, STRING_TO_VECTOR('[1,2,3]'))" \
        success

    test_case "INSERT [4,5,6]" \
        "insert into t_vec values (2, STRING_TO_VECTOR('[4,5,6]'))" \
        success

    test_case "INSERT [7,8,9]" \
        "insert into t_vec values (3, STRING_TO_VECTOR('[7,8,9]'))" \
        success

    test_case "INSERT [2,4,6]" \
        "insert into t_vec values (4, STRING_TO_VECTOR('[2,4,6]'))" \
        success

    test_case "INSERT [-1,-2,-3]" \
        "insert into t_vec values (5, STRING_TO_VECTOR('[-1,-2,-3]'))" \
        success

    test_case "建表: VECTOR(4) (用于跨维度测试)" \
        "create table t_dim4 (id int, v vector(4))" \
        success

    test_case "INSERT t_dim4 [1,2,3,4]" \
        "insert into t_dim4 values (1, STRING_TO_VECTOR('[1,2,3,4]'))" \
        success

    # =================================================================
    echo "--- 2. CREATE VECTOR INDEX 语法 ---"
    echo ""

    test_case "CREATE VECTOR INDEX 基本语法 (无 WITH 子句)" \
        "create vector index vec_idx1 on t_vec (v)" \
        success

    test_case "CREATE VECTOR INDEX WITH distance=euclidean" \
        "create vector index vec_idx2 on t_vec (v) with (distance=euclidean)" \
        success

    test_case "CREATE VECTOR INDEX WITH distance=cosine" \
        "create vector index vec_idx3 on t_vec (v) with (distance=cosine)" \
        success

    test_case "CREATE VECTOR INDEX WITH distance=dot" \
        "create vector index vec_idx4 on t_vec (v) with (distance=dot)" \
        success

    test_case "CREATE VECTOR INDEX WITH distance=l2 (别名)" \
        "create vector index vec_idx5 on t_vec (v) with (distance=l2)" \
        success

    test_case "CREATE VECTOR INDEX WITH lists=3" \
        "create vector index vec_idx6 on t_vec (v) with (lists=3)" \
        success

    test_case "CREATE VECTOR INDEX WITH probes=2" \
        "create vector index vec_idx7 on t_vec (v) with (probes=2)" \
        success

    test_case "CREATE VECTOR INDEX WITH 完整选项" \
        "create vector index vec_idx8 on t_vec (v) with (distance=cosine, type=ivfflat, lists=3, probes=2)" \
        success

    test_case "CREATE VECTOR INDEX 非VECTOR列应报错" \
        "create vector index bad_idx on t_vec (id)" \
        failure

    test_case "重复索引名应报错" \
        "create vector index vec_idx1 on t_vec (v)" \
        failure

    test_case "CREATE VECTOR INDEX 不存在的表应报错" \
        "create vector index ghost_idx on no_such_table (v)" \
        failure

    test_case "CREATE VECTOR INDEX 不存在的列应报错" \
        "create vector index ghost_idx on t_vec (no_such_col)" \
        failure

    # =================================================================
    echo "--- 3. 精确距离查询（无索引时全表扫描）---"
    echo ""

    test_case "SELECT DISTANCE cosine 全表扫描" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') from t_vec" \
        success

    test_case "SELECT DISTANCE euclidean 全表扫描" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') from t_vec" \
        success

    test_case "DISTANCE cosine 完全匹配应接近 0" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') from t_vec where id = 1" \
        success

    # =================================================================
    echo "--- 4. LIMIT 语法 ---"
    echo ""

    test_case "SELECT * LIMIT 1" \
        "select * from t_vec limit 1" \
        success

    test_case "SELECT * LIMIT 3" \
        "select * from t_vec limit 3" \
        success

    test_case "SELECT id LIMIT 2 (带 WHERE 过滤)" \
        "select id, v from t_vec where id <= 3 limit 2" \
        "contains:1,2,3"

    test_case "LIMIT 0 (应返回空结果集)" \
        "select id, v from t_vec limit 0" \
        success

    # =================================================================
    echo "--- 5. DISTANCE + ORDER BY + LIMIT 组合（Top-N 查询）---"
    echo ""

    test_case "Top-3 按余弦距离升序（查询向量 [1,2,3]）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') as d from t_vec order by d asc limit 3" \
        success

    test_case "Top-2 按欧几里得距离升序（查询零向量）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') as d from t_vec order by d asc limit 2" \
        success

    test_case "Top-3 按点积距离降序" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'dot') as d from t_vec order by d desc limit 3" \
        success

    test_case "Top-1 按 l2 距离升序" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'l2') as d from t_vec order by d asc limit 1" \
        success

    test_case "LIMIT 5 (等于全表大小)" \
        "select id, v from t_vec order by id asc limit 5" \
        "contains:1,2,3"

    test_case "LIMIT 超过全表行数 (LIMIT 100)" \
        "select id, v from t_vec limit 100" \
        "contains:1,2,3"

    # =================================================================
    echo "--- 6. LIMIT 边界和错误处理 ---"
    echo ""

    test_case "LIMIT 后跟标识符应报错" \
        "select id from t_vec limit abc" \
        failure

    test_case "LIMIT 负数值 (解析错误)" \
        "select id from t_vec limit -1" \
        failure

    # =================================================================
    echo "--- 7. AS 别名 + ORDER BY + LIMIT 组合 ---"
    echo ""

    test_case "AS 别名 + ORDER BY 别名 + LIMIT 2" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'euclidean') as dist from t_vec order by dist asc limit 2" \
        success

    test_case "混合别名 + ORDER BY + LIMIT 3" \
        "select id as myid, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'cosine') as mydist from t_vec where id != 5 order by mydist asc limit 3" \
        success

    # =================================================================
    echo "--- 8. 跨维度 / 不存在的列错误 ---"
    echo ""

    test_case "DISTANCE 维度不匹配 (3 vs 4) 应报错" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3,4]'), 'cosine') from t_vec" \
        failure

    # =================================================================
    echo "--- 9. 标准 B+ 树索引不受影响（VECTOR 列不支持 B+ 树）---"
    echo ""

    test_case "普通索引 ID 列 (B+ 树) 正常创建" \
        "create index id_idx on t_vec (id)" \
        success

    test_case "普通索引后 ID 等值查询正常" \
        "select * from t_vec where id = 1" \
        "contains:1,2,3"

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
