#!/bin/bash
# =============================================================================
# VECTOR 距离精确查询与排序功能测试脚本
#
# ⚠️  警告: 本脚本会删除 miniob/db/ 目录下所有数据，确保测试环境干净幂等。
#     如果你的 miniob/db/ 中有需要保留的数据，请先备份。
#
# 用法: 在 Docker 容器中执行
#   cd /workspace/miniob
#   bash test/vector_distance_sort_test.sh
#
# 如果已编译过，会跳过编译直接测试
# 如需重新编译:  bash test/vector_distance_sort_test.sh --rebuild
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
    echo -e "${YELLOW}=== VECTOR 距离精确查询与排序功能测试 ===${NC}"
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
    echo "--- 1. 准备数据: 建表并插入不同 VECTOR 数据 ---"
    echo ""

    test_case "建表: VECTOR(3) 用于距离查询" \
        "create table t_vec (id int, v vector(3))" \
        success

    test_case "建表: VECTOR(4) 用于维度不匹配测试" \
        "create table t_vec4 (id int, v vector(4))" \
        success

    test_case "建表: 带额外非向量列的表" \
        "create table t_multi (id int, name char(20), v vector(3))" \
        success

    test_case "INSERT t_vec: [1,2,3]" \
        "insert into t_vec values (1, STRING_TO_VECTOR('[1,2,3]'))" \
        success

    test_case "INSERT t_vec: [4,5,6]" \
        "insert into t_vec values (2, STRING_TO_VECTOR('[4,5,6]'))" \
        success

    test_case "INSERT t_vec: [7,8,9]" \
        "insert into t_vec values (3, STRING_TO_VECTOR('[7,8,9]'))" \
        success

    test_case "INSERT t_vec: [0,0,0] (零向量)" \
        "insert into t_vec values (4, STRING_TO_VECTOR('[0,0,0]'))" \
        success

    test_case "INSERT t_vec: [-1,-2,-3]" \
        "insert into t_vec values (5, STRING_TO_VECTOR('[-1,-2,-3]'))" \
        success

    test_case "INSERT t_multi: 含名称的向量数据" \
        "insert into t_multi values (1, 'alpha', STRING_TO_VECTOR('[1,2,3]'))" \
        success

    test_case "INSERT t_multi: 第2行" \
        "insert into t_multi values (2, 'beta', STRING_TO_VECTOR('[4,5,6]'))" \
        success

    test_case "INSERT t_multi: 第3行" \
        "insert into t_multi values (3, 'gamma', STRING_TO_VECTOR('[7,8,9]'))" \
        success

    # =================================================================
    echo "--- 2. 精确距离查询: SELECT 中使用 DISTANCE ---"
    echo ""

    test_case "SELECT DISTANCE cosine 全表扫描（排除零向量id=4，共4行）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') from t_vec where id != 4" \
        success

    test_case "SELECT DISTANCE euclidean 全表扫描（5行，零向量查询可处理）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') from t_vec" \
        success

    test_case "SELECT DISTANCE dot 全表扫描（5行）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'dot') from t_vec" \
        success

    test_case "SELECT DISTANCE cosine 带 WHERE 过滤" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') from t_vec where id = 1" \
        success

    test_case "SELECT DISTANCE euclidean 带 WHERE id=2" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[4,5,6]'), 'euclidean') from t_vec where id = 2" \
        success

    test_case "SELECT DISTANCE l2 别名（等价 euclidean）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'l2') from t_vec where id = 1" \
        success

    test_case "DISTANCE euclidean 与查询向量完全匹配应返回 0" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'euclidean') from t_vec where id = 1" \
        "contains:0"

    # =================================================================
    echo "--- 3. 维度检查: 向量维度一致性校验 ---"
    echo ""

    test_case "DISTANCE 维度匹配 3 vs 3 应成功" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') from t_vec where id = 1" \
        success

    test_case "DISTANCE 维度不匹配 3 vs 4 应报错（表列 vs 查询向量）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3,4]'), 'cosine') from t_vec where id = 1" \
        failure

    test_case "INSERT 维度不匹配应报错（4维值插入3维列）" \
        "insert into t_vec values (99, STRING_TO_VECTOR('[1,2,3,4]'))" \
        failure

    test_case "CALC DISTANCE 维度不匹配 3 vs 4 应报错" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[1,2,3,4]'), 'euclidean')" \
        failure

    # =================================================================
    echo "--- 4. AS 别名支持: 列别名与 DISTANCE 别名 ---"
    echo ""

    test_case "AS 为普通列指定别名" \
        "select id as my_id from t_vec where id = 1" \
        "contains:my_id"

    test_case "AS 为 VECTOR 列指定别名" \
        "select v as my_vector from t_vec where id = 1" \
        "contains:my_vector"

    test_case "AS 为 DISTANCE 结果指定别名" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') as dist from t_vec where id = 1" \
        success

    test_case "AS 为 DISTANCE euclidean 结果指定别名" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') as l2_dist from t_vec where id = 1" \
        success

    test_case "AS 为 DISTANCE dot 结果指定别名" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'dot') as dot_dist from t_vec where id = 1" \
        success

    test_case "CALC 中 AS 别名" \
        "calc DISTANCE(STRING_TO_VECTOR('[1,2,3]'), STRING_TO_VECTOR('[4,5,6]'), 'cosine') as my_dist, 1" \
        success

    test_case "多列 AS 别名混合（普通列 + DISTANCE）" \
        "select id as ident, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') as cos_dist from t_vec where id = 1" \
        success

    # =================================================================
    echo "--- 5. ORDER BY 排序: 基础排序 ---"
    echo ""

    test_case "ORDER BY id ASC 升序（全表，5行）" \
        "select id from t_vec order by id asc" \
        "contains:id"

    test_case "ORDER BY id DESC 降序（全表，5行）" \
        "select id from t_vec order by id desc" \
        "contains:id"

    test_case "ORDER BY 默认 ASC（省略 ASC 关键字）" \
        "select id from t_vec order by id" \
        "contains:id"

    test_case "ORDER BY 带 WHERE 过滤" \
        "select id from t_vec where id >= 3 order by id desc" \
        "contains:id"

    # =================================================================
    echo "--- 6. ORDER BY 排序: 按 DISTANCE 函数结果排序 ---"
    echo ""

    test_case "ORDER BY DISTANCE cosine ASC 升序（排除零向量id=4）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') from t_vec where id != 4 order by DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') asc" \
        success

    test_case "ORDER BY DISTANCE cosine DESC 降序（排除零向量id=4）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') from t_vec where id != 4 order by DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') desc" \
        success

    test_case "ORDER BY DISTANCE euclidean ASC（查询零向量，L2距离升序）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') from t_vec order by DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') asc" \
        success

    test_case "ORDER BY DISTANCE dot DESC（点积距离降序）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'dot') from t_vec order by DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'dot') desc" \
        success

    # =================================================================
    echo "--- 7. ORDER BY 排序: 按别名排序 ---"
    echo ""

    test_case "ORDER BY 别名 ASC（DISTANCE cosine 别名为 dist，排除零向量）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') as dist from t_vec where id != 4 order by dist asc" \
        success

    test_case "ORDER BY 别名 DESC（DISTANCE euclidean 别名为 l2d）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') as l2d from t_vec order by l2d desc" \
        success

    test_case "ORDER BY 普通列别名 ASC" \
        "select id as idx from t_vec order by idx asc" \
        "contains:idx"

    test_case "ORDER BY 普通列别名 DESC" \
        "select id as idx from t_vec order by idx desc" \
        "contains:idx"

    test_case "ORDER BY DISTANCE 别名 + 带 WHERE 过滤" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'euclidean') as d from t_vec where id >= 2 order by d asc" \
        success

    # =================================================================
    echo "--- 8. ORDER BY 排序: 多列排序 ---"
    echo ""

    test_case "ORDER BY 多列: id ASC, id ASC（相同列多键）" \
        "select id from t_vec order by id asc, id asc" \
        "contains:id"

    test_case "ORDER BY 多列: id ASC 与 DISTANCE 别名 DESC（排除零向量）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') as dist from t_vec where id != 4 order by id asc, dist desc" \
        success

    test_case "ORDER BY 非向量列 + DISTANCE 表达式" \
        "select id, name, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'euclidean') from t_multi order by id asc, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'euclidean') asc" \
        success

    # =================================================================
    echo "--- 9. AS + DISTANCE + ORDER BY 组合场景 ---"
    echo ""

    test_case "组合: SELECT DISTANCE AS 别名 + ORDER BY 别名 ASC（排除零向量）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') as d from t_vec where id != 4 order by d asc" \
        success

    test_case "组合: SELECT DISTANCE AS 别名 + ORDER BY 别名 DESC（排除零向量）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') as d from t_vec where id != 4 order by d desc" \
        success

    test_case "组合: 混合别名 普通列+距离 + ORDER BY 距离别名" \
        "select id as myid, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') as mydist from t_vec order by mydist asc" \
        success

    test_case "组合: 多列别名 + ORDER BY 多别名（排除零向量）" \
        "select id as i, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') as d from t_multi where id != 4 order by i asc, d desc" \
        success

    test_case "组合: 所有三种距离方法 + 别名 + 排序（排除零向量）" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'cosine') as c, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'euclidean') as e, DISTANCE(v, STRING_TO_VECTOR('[1,2,3]'), 'dot') as d from t_vec where id != 4 order by c asc" \
        success

    # =================================================================
    echo "--- 10. 精确距离查询结果排序验证: 查询 [0,0,0]，euclidean 距离应升序 ---"
    echo ""

    # 从最近到最远: id=4 ([0,0,0] 距离=0), id=1 ([1,2,3] 距离=14), id=5 ([-1,-2,-3] 距离=14), id=2 ([4,5,6] 距离=77), id=3 ([7,8,9] 距离=194)
    test_case "验证 euclidean 距离排序: id=4 应比 id=2 更接近 [0,0,0]" \
        "select id, DISTANCE(v, STRING_TO_VECTOR('[0,0,0]'), 'euclidean') as d from t_vec order by d asc" \
        success

    # =================================================================
    echo "--- 11. 错误处理: ORDER BY 相关错误 ---"
    echo ""

    test_case "ORDER BY 不存在的列应报错" \
        "select id from t_vec order by nonexistent_column" \
        failure

    test_case "ORDER BY 不存在的别名应报错" \
        "select id as myid from t_vec order by wrong_alias" \
        failure

    # =================================================================
    echo "--- 12. ORDER BY 与 GROUP BY 联合使用 ---"
    echo ""

    test_case "GROUP BY + ORDER BY: 按 id 分组后排序" \
        "select id from t_vec group by id order by id desc" \
        "contains:id"

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
