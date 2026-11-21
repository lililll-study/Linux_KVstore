#!/bin/bash
# test/9_mem_diag.sh
#
# 内存基线诊断：定位"start 基线"里非内存池的那部分驻留内存（~2MB）到底是谁
# 原理：同一进程在 start 和 after_trim 两个时刻各抓一次完整 pmap，
#       按映射地址 diff ——
#         "start 有、trim 后仍在/消失"的大映射 = 启动后一直驻留的底噪
#       池内存 trim 时会 madvise 归还（RSS 掉、地址段可能被 spare 囤住），
#       非池内存 trim 管不到，RSS 不变。
#
# 用法：bash test/9_mem_diag.sh   （不需要 sudo，eBPF 已关）

set -u

# ===== 切到项目根 =====
cd "$(dirname "$0")/.." || exit 1
ROOT="$(pwd)"

PORT=2000
BIN=./build/kvstore
TEST=./build/1_RESP_testcase
LOG=/tmp/mem_diag/kvstore_diag.log
OUT=/tmp/mem_diag
mkdir -p "$OUT"

# 与 5_mem_test.sh 相同口径：强制关 eBPF，排除 ringbuf 64MB 底噪
CONF=config/mem_diag.conf
sed -e "s/^ebpf.enable=.*/ebpf.enable=false/" config/master.conf > "$CONF"

echo "===== mem baseline diag ====="
echo "[INFO] root = $ROOT"
echo "[INFO] out  = $OUT"

# ===== 0. 确保端口空闲 =====
if ss -tlnp 2>/dev/null | grep -q ":$PORT "; then
    echo "[WARN] port $PORT in use, killing old kvstore..."
    pkill -f build/kvstore 2>/dev/null
    sleep 1
fi

# ===== 1. 清数据，启动服务端 =====
mkdir -p data logs
rm -f data/*.aof data/*.dat data/*.snap data/.tmp

$BIN --config $CONF > "$LOG" 2>&1 &
SERVER_PID=$!
sleep 3

if [ ! -d "/proc/$SERVER_PID" ]; then
    echo "[ERR] server failed to start, log:"
    cat "$LOG"
    exit 1
fi
echo "[INFO] server pid = $SERVER_PID"

# ===== 2. 采样函数：status + 完整 pmap + smaps_rollup =====
snap() {
    local tag=$1
    grep -E 'VmSize|VmRSS' /proc/$SERVER_PID/status > "$OUT/status_$tag.txt" 2>/dev/null
    pmap -x $SERVER_PID > "$OUT/pmap_$tag.txt" 2>/dev/null
    cat /proc/$SERVER_PID/smaps_rollup > "$OUT/rollup_$tag.txt" 2>/dev/null
    local rss vsz
    rss=$(awk '/VmRSS/{print $2}'  "$OUT/status_$tag.txt")
    vsz=$(awk '/VmSize/{print $2}' "$OUT/status_$tag.txt")
    echo "[SAMPLE:$tag] VmSize=${vsz} kB, VmRSS=${rss} kB"
}

# ===== 3. 时刻一：start（空载基线）=====
snap start

# ===== 4. 插 100w → 删 100w → USR2 触发 trim =====
echo "----- insert 1M -----"
$TEST 6 -1 > /dev/null 2>&1
sleep 2
snap after_insert

echo "----- delete 1M -----"
$TEST 7 -1 > /dev/null 2>&1
sleep 2

kill -USR2 $SERVER_PID
sleep 2
snap after_trim

# ===== 5. 关服务端 =====
kill $SERVER_PID 2>/dev/null
wait $SERVER_PID 2>/dev/null
rm -f "$CONF"

# ===== 6. 分析：start vs after_trim 按地址 diff =====
echo ""
echo "===== [A] 仅 start 存在、trim 后消失的映射（启动驻留底噪候选）====="
awk '
NR==FNR {  # 第一份：pmap_start
    if ($1 ~ /^[0-9a-f]{8,}$/) {
        rss[$1]=$3; kb[$1]=$2
        nm=$6; for(i=7;i<=NF;i++) nm=nm" "$i; name[$1]=nm
    }
    next
}
$1 ~ /^[0-9a-f]{8,}$/ { seen[$1]=1 }
END {
    # 第二份读完（pmap_after_trim），start 里没被 seen 的就是消失的
    for (a in rss) if (!(a in seen)) printf "%10d kB  %s  %s\n", rss[a], a, name[a]
}' "$OUT/pmap_start.txt" "$OUT/pmap_after_trim.txt" | sort -rn | head -15

echo ""
echo "===== [B] 两侧都有、但 RSS 下降最多的映射（部分归还）====="
awk '
NR==FNR {
    if ($1 ~ /^[0-9a-f]{8,}$/) { rss[$1]=$3
        nm=$6; for(i=7;i<=NF;i++) nm=nm" "$i; name[$1]=nm }
    next
}
$1 ~ /^[0-9a-f]{8,}$/ && ($1 in rss) {
    d = rss[$1] - $3
    if (d > 0) printf "%10d kB (was %d)  %s  %s\n", d, rss[$1], $1, name[$1]
}' "$OUT/pmap_start.txt" "$OUT/pmap_after_trim.txt" | sort -rn | head -10

echo ""
echo "===== [C] anon 匿名内存总量对比（堆/栈/mmap匿名段 都在这里）====="
for tag in start after_insert after_trim; do
    # pmap -x 中匿名映射的 Mapping 列是 "[ anon ]"
    total=$(awk '/\[ anon \]$/ {s+=$3} END{print s+0}' "$OUT/pmap_$tag.txt")
    echo "  $tag: anon RSS = $total kB"
done

echo ""
echo "===== [D] 132KB 大小段数量（ntyco 协程栈特征段）====="
for tag in start after_insert after_trim; do
    cnt=$(awk '$2==132 {n++} END{print n+0}' "$OUT/pmap_$tag.txt")
    echo "  $tag: $cnt 个"
done

echo ""
echo "===== [E] smaps_rollup 汇总（Anonymous=纯堆栈匿名页）====="
for tag in start after_trim; do
    echo "  --- $tag ---"
    grep -E '^(Rss|Anonymous|Private_Clean|Private_Dirty):' "$OUT/rollup_$tag.txt" 2>/dev/null | sed 's/^/  /'
done

echo ""
echo "===== [F] start 时刻 RSS 最大的 10 个映射 ====="
awk '$1 ~ /^[0-9a-f]{8,}$/ {
    nm=$6; for(i=7;i<=NF;i++) nm=nm" "$i
    printf "%10d kB  %s\n", $3, nm
}' "$OUT/pmap_start.txt" | sort -rn | head -10

echo ""
echo "[INFO] 完整快照在 $OUT/ (pmap_start.txt / pmap_after_insert.txt / pmap_after_trim.txt)"
grep -i "trim\|released\|归还" "$LOG" 2>/dev/null | tail -5
