#!/bin/bash
# test/mem_test.sh <mode>
#   mode: nopool | pool | jemalloc
#
# 自动完成：启动服务端 → 采样 → 插 100w → 采样 → 删 100w → 采样 → 关服务端 → 输出结果
# 结果追加到 /tmp/mem_results.txt

set -u

# ===== 切到项目根 =====
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"

MODE=${1:-pool}
PORT=2000
BIN=./build/kvstore
TEST=./build/1_RESP_testcase
LOG=/tmp/kvstore_$MODE.log

# 测试专用配置：基于 master.conf 生成，强制关闭 eBPF
# ringbuf 会固定吃掉 ~64MB RSS + ~128MB VSZ（内核分配时清零全部页），
# 抬高所有状态的基线；内存池测试只关心池自身的增长/归还，须排除这个底噪
CONF=config/mem_test.conf
sed -e "s/^ebpf.enable=.*/ebpf.enable=false/" config/master.conf > "$CONF"

# jemalloc 路径
JEMALLOC=$(find /usr/lib /usr/local/lib -name "libjemalloc.so*" 2>/dev/null | head -1)

case $MODE in
    nopool)    PRELOAD="" ;;
    pool)      PRELOAD="" ;;
    jemalloc)
        if [ -z "$JEMALLOC" ]; then
            echo "[ERR] libjemalloc not found, install: sudo apt install libjemalloc-dev"
            exit 1
        fi
        PRELOAD="$JEMALLOC"
        # jemalloc 立刻归还内存，便于观察 after_delete 的 RSS 回落
        export MALLOC_CONF="dirty_decay_ms:0,muzzy_decay_ms:0"
        ;;
    *)
        echo "Usage: $0 <nopool|pool|jemalloc>"
        exit 1
        ;;
esac

echo "===== mem test: mode=$MODE ====="
echo "[INFO] root    = $ROOT"
echo "[INFO] binary  = $BIN"
echo "[INFO] preload = ${PRELOAD:-none}"

# ===== 0. 确保端口空闲 =====
if ss -tlnp 2>/dev/null | grep -q ":$PORT "; then
    echo "[WARN] port $PORT already in use, killing old kvstore..."
    pkill -f build/kvstore 2>/dev/null
    sleep 1
fi

# ===== 1. 清数据，启动服务端 =====
# 三种持久化产物全清（snap/aof/dat）+ 全量同步临时文件，
# 否则启动时 load_snapshots 会把上次测试残留的数据加载进内存，污染 start 基线
mkdir -p data logs
rm -f data/*.aof data/*.dat data/*.snap data/.tmp

if [ -n "$PRELOAD" ]; then
    LD_PRELOAD="$PRELOAD" $BIN --config $CONF > "$LOG" 2>&1 &
else
    $BIN --config $CONF > "$LOG" 2>&1 &
fi
SERVER_PID=$!
sleep 2

if [ ! -d "/proc/$SERVER_PID" ]; then
    echo "[ERR] server failed to start, log:"
    cat "$LOG"
    exit 1
fi
echo "[INFO] server pid = $SERVER_PID"

# ===== 2. 采样函数 =====
sample_rss() {
    grep VmRSS /proc/$SERVER_PID/status 2>/dev/null | awk '{print $2}'
}
sample_vsz() {
    grep VmSize /proc/$SERVER_PID/status 2>/dev/null | awk '{print $2}'
}

# ===== 3. 开始状态 =====
START_RSS=$(sample_rss)
START_VSZ=$(sample_vsz)
echo "[START] VmSize=${START_VSZ} kB, VmRSS=${START_RSS} kB"

# ===== 4. 后台采样（记录峰值）=====
PEAK_FILE=/tmp/peak_$MODE.txt
echo "0 0" > "$PEAK_FILE"
(
    PEAK_RSS=0
    PEAK_VSZ=0
    while [ -d "/proc/$SERVER_PID" ]; do
        R=$(sample_rss); [ -z "$R" ] && break
        V=$(sample_vsz); [ -z "$V" ] && break
        [ "$R" -gt "$PEAK_RSS" ] && PEAK_RSS=$R
        [ "$V" -gt "$PEAK_VSZ" ] && PEAK_VSZ=$V
        echo "$PEAK_RSS $PEAK_VSZ" > "$PEAK_FILE"
        sleep 0.02
    done
) &
SAMPLER_PID=$!

# ===== 5. 插 100w（2 引擎 × 50w）=====
echo "----- insert 1M -----"
$TEST 6 -1
sleep 2
AFTER_INSERT_RSS=$(sample_rss)
AFTER_INSERT_VSZ=$(sample_vsz)
echo "[AFTER_INSERT] VmSize=${AFTER_INSERT_VSZ} kB, VmRSS=${AFTER_INSERT_RSS} kB"

# ===== 6. 删 100w =====
echo "----- delete 1M -----"
$TEST 7 -1
sleep 2
AFTER_DELETE_RSS=$(sample_rss)
AFTER_DELETE_VSZ=$(sample_vsz)
echo "[AFTER_DELETE] VmSize=${AFTER_DELETE_VSZ} kB, VmRSS=${AFTER_DELETE_RSS} kB"


# ===== 6.5 手动触发内存池回收 =====
kill -USR2 $SERVER_PID
sleep 1
AFTER_TRIM_RSS=$(sample_rss)
AFTER_TRIM_VSZ=$(sample_vsz)
echo "[AFTER_TRIM] VmSize=${AFTER_TRIM_VSZ} kB, VmRSS=${AFTER_TRIM_RSS} kB"


# ===== 7. 停采样 =====
kill $SAMPLER_PID 2>/dev/null
wait $SAMPLER_PID 2>/dev/null
read PEAK_RSS PEAK_VSZ < "$PEAK_FILE"

# ===== 8. 关服务端 =====
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null

# ===== 9. 输出 =====
echo ""
echo "===== result: $MODE ====="
printf "%-14s %12s %12s\n" "state" "VmSize(kB)" "VmRSS(kB)"
printf "%-14s %12s %12s\n" "start"        "$START_VSZ"        "$START_RSS"
printf "%-14s %12s %12s\n" "peak"         "$PEAK_VSZ"         "$PEAK_RSS"
printf "%-14s %12s %12s\n" "after_insert" "$AFTER_INSERT_VSZ" "$AFTER_INSERT_RSS"
printf "%-14s %12s %12s\n" "after_delete" "$AFTER_DELETE_VSZ" "$AFTER_DELETE_RSS"
printf "%-14s %12s %12s\n" "after_trim"   "$AFTER_TRIM_VSZ"   "$AFTER_TRIM_RSS"
# 追加到汇总文件
echo "$MODE $START_VSZ $START_RSS $PEAK_VSZ $PEAK_RSS $AFTER_INSERT_VSZ $AFTER_INSERT_RSS $AFTER_DELETE_VSZ $AFTER_DELETE_RSS" \
    >> /tmp/mem_results.txt

# 清理测试专用配置
rm -f "$CONF"