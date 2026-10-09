#!/bin/bash
# ============================================================
# qps_aof_bench.sh
# 案例一：AOF 开/关对 QPS 的影响（预热 + 交替测试）
#   - 先跑 1 轮 mode5 预热（页缓存/内存池/CPU 频率热身，不计入统计）
#   - 之后 on/off 交替各跑 RUNS 轮，消除顺序效应
# 用法: ./test/1_qps_aof_bench.sh [基准配置文件] （默认 config/master.conf）
# ============================================================

BASE_CONF=${1:-config/master.conf}
SERVER=./build/kvstore
TESTCASE=./build/1_RESP_testcase
IP=127.0.0.1
PORT=$(grep -E '^port=' "$BASE_CONF" | cut -d= -f2 | tr -d ' ')
RUNS=5

# 临时配置目录
TMPDIR_BENCH=/tmp/kvs_bench_$$
mkdir -p "$TMPDIR_BENCH"

# 生成 aof.enable 开/关两份配置（其余配置项保持原样）
gen_conf() {
    local enable=$1 out=$2
    if grep -qE '^aof.enable=' "$BASE_CONF"; then
        sed "s/^aof.enable=.*/aof.enable=$enable/" "$BASE_CONF" > "$out"
    else
        cp "$BASE_CONF" "$out"
        echo "aof.enable=$enable" >> "$out"
    fi
}
gen_conf false "$TMPDIR_BENCH/aof_off.conf"
gen_conf true  "$TMPDIR_BENCH/aof_on.conf"

start_server() {
    "$SERVER" --config "$1" > /dev/null 2>&1 &
    SERVER_PID=$!
    # 等端口就绪
    for i in $(seq 1 50); do
        if (exec 3<>/dev/tcp/$IP/$PORT) 2>/dev/null; then
            exec 3>&- 3<&-
            return 0
        fi
        sleep 0.1
    done
    echo "ERROR: server failed to start on port $PORT"
    exit 1
}

stop_server() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    SERVER_PID=
    # 清理 AOF 残留，保证每轮干净启动
    rm -f data/*.aof data/aof* 2>/dev/null
    sleep 0.5
}

# 跑一轮 off+on：各启动一次服务端，结果分别追加到两个文件
run_round() {
    local round_idx=$1 raw_off=$2 raw_on=$3

    # ---- off 轮 ----
    stop_server
    start_server "$TMPDIR_BENCH/aof_off.conf"
    echo "--- Round $round_idx [aof=off] ---"
    "$TESTCASE" "$IP" "$PORT" 5 | tee -a "$raw_off"

    # ---- on 轮 ----
    stop_server
    start_server "$TMPDIR_BENCH/aof_on.conf"
    echo "--- Round $round_idx [aof=on] ---"
    "$TESTCASE" "$IP" "$PORT" 5 | tee -a "$raw_on"
}

# 计算均值：所有 qps 值的算术平均（保留 1 位小数）
compute_avg() {
    awk -v total_qps=0 -v n=0 '
        /qps=/ { for (i = 1; i <= NF; i++) if ($i ~ /^qps=/) { total_qps += substr($i, 5); n++ } }
        END { if (n > 0) printf "%.1f (samples=%d)", total_qps / n, n; else print "N/A" }'
}

# 按"模式+引擎"分别求均值
compute_avg_per_engine() {
    awk '
        /^\[.*(single|5ops)/ {
            tag = $1; engine = $2;
            for (i = 1; i <= NF; i++)
                if ($i ~ /^qps=/) { key = tag " " engine; sum[key] += substr($i, 5); cnt[key]++ }
        }
        END { for (k in sum) printf "  %-16s %-10s avg_qps=%.1f\n", k, "", sum[k] / cnt[k] }
    ' | sort
}

bench_phase() { :; }  # 已由交替测试 run_round 取代

# ============ 主流程 ============
if [ ! -x "$SERVER" ] || [ ! -x "$TESTCASE" ]; then
    echo "ERROR: 先编译: make reactor"
    exit 1
fi

echo "基础配置: $BASE_CONF (port=$PORT), 预热 1 轮 + 交替测试 $RUNS 轮"

RAW_OFF="$TMPDIR_BENCH/raw_off.txt"
RAW_ON="$TMPDIR_BENCH/raw_on.txt"
: > "$RAW_OFF"; : > "$RAW_ON"

# ---- 预热轮（不计入统计）----
echo "===== Warmup（不计入统计）====="
run_round 0 /dev/null /dev/null

# ---- 正式交替测试 ----
for i in $(seq 1 "$RUNS"); do
    run_round "$i" "$RAW_OFF" "$RAW_ON"
done
stop_server

echo
echo "=============================================="
echo " 结果汇总（交替测试，消除顺序效应）"
echo "=============================================="
echo "[AOF off] 总 QPS 均值: $(compute_avg < "$RAW_OFF")"
echo "[AOF on ] 总 QPS 均值: $(compute_avg < "$RAW_ON")"
echo
echo "[AOF off] 分项均值:"
compute_avg_per_engine < "$RAW_OFF"
echo
echo "[AOF on ] 分项均值:"
compute_avg_per_engine < "$RAW_ON"

rm -rf "$TMPDIR_BENCH"
echo
echo "===== 测试完成，原始数据已随每轮打印，可重定向保存 ====="