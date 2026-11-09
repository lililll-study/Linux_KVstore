#!/bin/bash
# =====================================================================
# 传输性能对比脚本：iperf3(链路上限) vs RDMA vs TCP+sendfile
#
# 用法（两台终端，都在 kvs_lhy 目录下）：
#   主机终端:  ./test/8_transfer_bench.sh server [file_mb]
#   从机终端:  ./test/8_transfer_bench.sh client <master_ip> [file_mb]
#
# 说明:
#   - 测试文件在 client 端用 dd 生成（/dev/urandom 随机数据，不可压缩）
#   - 测试顺序：iperf3 -> RDMA -> TCP+sendfile
#     把 iperf3 放最前面，是因为它没有落盘、对系统噪声最敏感，
#     放在前面环境最干净；后面每两个测试之间都 sync + sleep，
#     避免上一个测试的脏页回写污染下一个测试
#   - 三个测试统一口径：
#       同一份文件、同一个 IP（必须是 RDMA 那块网卡的 IP）
#       接收端回 ACK 前都强制落盘（RDMA: msync / TCP: fsync）
#       iperf3 用 -n 传与文件等量的字节，并用 -w 4M 与 TCP 测试的
#       4MB socket 缓冲对齐（否则窗口大小不同，数字不可比）
#   - iperf3 连跑 3 次取中位数（它不用重启 server，成本最低）；
#     RDMA / TCP 各跑 1 次，想更准就把整个脚本连续跑 3 次自行取中位数
# =====================================================================
set -u

RDMA_BIN=./rdma/rdma_server
RDMA_CLI=./rdma/rdma_client
TCP_BIN=./rdma/tcp_server
TCP_CLI=./rdma/tcp_client
BENCH_FILE=bench_100m.bin       # 默认 100MB，可被第二个参数覆盖

check_bins() {
    for b in "$@"; do
        if [ ! -x "$b" ]; then
            echo "[ERROR] $b 不存在，请先在主机执行: make bench"
            exit 1
        fi
    done
}

# 从 iperf3 输出里取 sender 的比特速率（Mbits/sec），换算成 MiB/s
# iperf3 的 Mbits/sec 是 10^6 bit/s，除以 8.388608 得到以 1024*1024 为单位的 MiB/s，
# 这样和 RDMA / TCP 两边打印的 MB/s 是同一把尺子
iperf_to_mibs() {
    awk '/sender/ {
            for (i = 1; i <= NF; i++) {
                if ($i == "Mbits/sec") { v = $(i - 1); }
            }
         }
         END { if (v != "") printf "%.2f", v / 8.388608 }'
}

# ---------- server 端（主机） ----------
do_server() {
    check_bins "$RDMA_BIN" "$TCP_BIN"

    echo "[server] 等待 client 连接，输出文件: rdma_recv.bin / tcp_recv.bin"

    # iperf3 server：记录 PID，测试结束统一回收
    # 原来的 wait 会一直等 iperf3 -s（它常驻），导致脚本永远不返回，
    # 残留的 iperf3 还会一直占着 5201 端口，下一轮测试容易串场
    IPERF_PID=""
    if command -v iperf3 >/dev/null 2>&1; then
        pkill -f "iperf3 -s" 2>/dev/null
        # 日志重定向到文件：iperf3 被 kill 时会打印
        # "iperf3: interrupt - the server has terminated"，这行不是错误，
        # 但混在测试输出里容易被误认为失败，所以不往终端打
        iperf3 -s >/tmp/iperf3_server.log 2>&1 &
        IPERF_PID=$!
        echo "[server] iperf3 server 已启动 (pid=$IPERF_PID, 日志: /tmp/iperf3_server.log)"
    else
        echo "[server] 未安装 iperf3，跳过（可选: apt install iperf3）"
    fi

    ./$TCP_BIN tcp_recv.bin &
    TCP_PID=$!
    ./$RDMA_BIN rdma_recv.bin &
    RDMA_PID=$!

    # 只等两个传输 server，不等 iperf3（它常驻）
    wait $TCP_PID
    wait $RDMA_PID
    echo "[server] RDMA / TCP 测试完成"

    if [ -n "$IPERF_PID" ]; then
        kill "$IPERF_PID" 2>/dev/null
        wait "$IPERF_PID" 2>/dev/null
        echo "[server] iperf3 server 已停止"
    fi
}

# ---------- client 端（从机） ----------
do_client() {
    local server_ip=$1
    local file_mb=${2:-100}
    check_bins "$RDMA_CLI" "$TCP_CLI"

    # 生成随机测试文件（不可压缩，保证公平）
    if [ ! -f "$BENCH_FILE" ] || [ "$(stat -c%s "$BENCH_FILE")" != "$((file_mb * 1024 * 1024))" ]; then
        echo "[client] 生成测试文件 $BENCH_FILE (${file_mb}MB)..."
        dd if=/dev/urandom of="$BENCH_FILE" bs=1M count="$file_mb" status=none
    fi

    echo "[client] 目标主机: $server_ip, 文件: ${file_mb}MB"
    echo "[client] 到对端的实际路由（确认三个测试走的是同一块网卡）:"
    ip route get "$server_ip" 2>/dev/null | head -1

    echo ""
    echo "================ [1/3] iperf3 TCP 裸链路（参照上限） ================"
    if command -v iperf3 >/dev/null 2>&1; then
        iperf_runs=()
        for i in 1 2 3; do
            sync
            sleep 1
            echo "[client] --- 第 $i 次: iperf3 -c $server_ip -n ${file_mb}M -w 4M ---"
            out=$(iperf3 -c "$server_ip" -n "${file_mb}M" -w 4M 2>&1)
            echo "$out" | grep -E "Interval|sender|receiver|error"
            mibs=$(echo "$out" | iperf_to_mibs)
            if [ -n "$mibs" ]; then
                echo "[client] 第 $i 次: ${mibs} MB/s"
                iperf_runs+=("$mibs")
            fi
        done
        if [ ${#iperf_runs[@]} -eq 3 ]; then
            med=$(printf '%s\n' "${iperf_runs[@]}" | sort -g | sed -n '2p')
            echo "[client] iperf3 三次中位数: ${med} MB/s"
        fi
    else
        echo "[client] 未安装 iperf3，跳过（可选: apt install iperf3）"
    fi

    echo ""
    echo "================ [2/3] RDMA 传输 ================"
    sleep 2   # 等 server 就绪 + iperf3 的缓冲落定
    ./$RDMA_CLI "$server_ip" "$BENCH_FILE"

    echo ""
    echo "================ [3/3] TCP+sendfile 传输 ================"
    sync
    sleep 2   # 避免 RDMA 测试的脏页回写干扰 TCP 测试
    ./$TCP_CLI "$server_ip" "$BENCH_FILE"
}

# ---------- 入口 ----------
case "${1:-}" in
    server) shift; do_server "$@" ;;
    client)
        if [ -z "${2:-}" ]; then
            echo "用法: $0 client <master_ip> [file_mb]"
            exit 1
        fi
        do_client "$2" "${3:-100}"
        ;;
    *)
        echo "用法:"
        echo "  主机: $0 server [file_mb]"
        echo "  从机: $0 client <master_ip> [file_mb]"
        exit 1
        ;;
esac
