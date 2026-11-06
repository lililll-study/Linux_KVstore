#!/bin/bash
# =====================================================================
# 实时同步 QPS 对比脚本：关闭同步 vs eBPF 转发 vs 网络转发
#
# 用法（在主机 VM 的 kvs_lhy 目录下）：
#   ./test/7_qps_bench.sh solo <轮数>          # 只测"关闭同步"基线，不需要从机
#   ./test/7_qps_bench.sh full <轮数> <master_ip> <master_port>
#                                             # 三模式连续测：模式2/3 需要从机配合
#
# 原理：
#   模式1 关闭同步  : ebpf=false, net.forward=false，从机不连，纯本地写
#   模式2 eBPF 转发 : ebpf=true,  net.forward=false，从机连上后 eBPF 旁路转发
#   模式3 网络转发  : ebpf=false, net.forward=true， 从机连上后命令线程直发
#   每轮压测用 1_RESP_testcase 8 0（echo 写入：10w 条 HSET 单条收发，
#   连接 127.0.0.1:2000，故本脚本须在主机 VM 上运行），抓 qps= 均值
# =====================================================================
set -u

BIN=./build/kvstore
TEST_CLI=./build/1_RESP_testcase
CONF_DIR=./config
RUNS=${2:-5}

MASTER_IP=${3:-192.168.137.139}
MASTER_PORT=${4:-2000}

# 从配置文件读取从机连接参数（压测后恢复从机数据一致性用，非必须）
SLAVE_PORT=$(grep -oP 'slave\.port\s*=\s*\K\d+' $CONF_DIR/master.conf 2>/dev/null || echo 3000)

results=()

# 抓取压测输出的 qps 均值：取所有 "qps=" 行的最后一个数字求平均
run_bench() {
    local label=$1
    local total=0 n=0
    for i in $(seq 1 "$RUNS"); do
        out=$($TEST_CLI 8 0 2>/dev/null)   # mode 8 = echo 写入 10w 条，engine 0 = Hash
        qps=$(echo "$out" | grep -oP 'qps=\K[0-9.]+' | tail -1)
        if [ -z "$qps" ]; then
            echo "[WARN] 第 $i 轮未取到 qps，跳过"
            continue
        fi
        echo "  [$label] 第 $i/$RUNS 轮: qps=$qps"
        total=$(awk "BEGIN{print $total + $qps}")
        n=$((n + 1))
    done
    if [ "$n" -eq 0 ]; then
        echo "[ERROR] $label 未采集到任何 QPS 数据"
        avg=0
    else
        avg=$(awk "BEGIN{printf \"%.0f\", $total / $n}")
    fi
    results+=("$label: $avg qps (avg of $n)")
    echo ""
}

# 生成临时配置：基于 master.conf，替换/追加 ebpf 与 net.forward 两个开关
make_conf() {
    local ebpf=$1 netf=$2
    sed -e "s/^ebpf.enable=.*/ebpf.enable=$ebpf/" \
        "$CONF_DIR/master.conf" > "$CONF_DIR/bench_master.conf"
    if grep -q '^net.forward=' "$CONF_DIR/bench_master.conf"; then
        sed -i "s/^net.forward=.*/net.forward=$netf/" "$CONF_DIR/bench_master.conf"
    else
        echo "net.forward=$netf" >> "$CONF_DIR/bench_master.conf"
    fi
    echo "$CONF_DIR/bench_master.conf"
}

# 起主机进程并等端口就绪
start_master() {
    local conf=$1
    ./$BIN --config "$conf" > /tmp/kvs_master_bench.log 2>&1 &
    MASTER_PID=$!
    sleep 2
}

stop_master() {
    [ -n "${MASTER_PID:-}" ] && kill "$MASTER_PID" 2>/dev/null
    wait "$MASTER_PID" 2>/dev/null
    sleep 1
}

# 提示从机配合（模式2/3 需要）
wait_slave() {
    echo "---------------------------------------------------------------"
    echo ">>> 请在从机终端启动从机并连接主机（sync $MASTER_IP $MASTER_PORT）"
    echo ">>> 从机完成全量同步后，回车继续..."
    read -r
}

# ---------- 入口 ----------
if [ ! -x "$BIN" ] || [ ! -x "$TEST_CLI" ]; then
    echo "[ERROR] 缺少 $BIN 或 $TEST_CLI，请先 make"
    exit 1
fi

MODE=${1:-full}

echo "================= [1/3] 关闭主从同步（基线） ================="
start_master "$(make_conf false false)"
run_bench "sync-off"
stop_master

if [ "$MODE" = "full" ]; then
    echo "================= [2/3] eBPF 转发 ================="
    start_master "$(make_conf true false)"
    echo ">>> 主机已用 ebpf=true 配置启动，等从机连接"
    wait_slave
    run_bench "ebpf-fwd"
    stop_master

    echo "================= [3/3] 网络转发 ================="
    start_master "$(make_conf false true)"
    echo ">>> 主机已用 net.forward=true 配置启动，等从机重连"
    wait_slave
    run_bench "net-fwd"
    stop_master
fi

echo "================= 结果汇总（RUNS=$RUNS） ================="
for r in "${results[@]}"; do
    echo "  $r"
done

rm -f "$CONF_DIR/bench_master.conf"
