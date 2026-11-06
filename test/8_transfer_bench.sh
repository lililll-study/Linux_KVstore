#!/bin/bash
# =====================================================================
# 传输性能对比脚本：RDMA vs TCP+sendfile vs iperf3(TCP 裸链路上限)
#
# 用法（两台终端，都在 kvs_lhy 目录下）：
#   主机终端:  ./test/8_transfer_bench.sh server [file_mb]
#   从机终端:  ./test/8_transfer_bench.sh client <master_ip> [file_mb]
#
# 说明:
#   - 测试文件在 client 端用 dd 生成（/dev/urandom 随机数据，不可压缩）
#   - client 先跑 RDMA 发送，再跑 TCP+sendfile，最后跑 iperf3（若已安装）
#   - iperf3 是内存到内存的 TCP 裸链路吞吐，作为 sendfile 的参照上限：
#     sendfile 接近 iperf3 -> 瓶颈在网络；明显低于 -> 瓶颈在磁盘写
#   - 两端各自输出耗时与吞吐，client 端最后汇总对比
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

# ---------- server 端（主机） ----------
do_server() {
    check_bins "$RDMA_BIN" "$TCP_BIN"

    echo "[server] 等待 client 连接，输出文件: rdma_recv.bin / tcp_recv.bin"
    echo "[server] Ctrl+C 结束测试"

    # iperf3 server：装了就一起跑（常驻后台，收 iperf3 client 的测速连接）
    if command -v iperf3 >/dev/null 2>&1; then
        iperf3 -s &
        echo "[server] iperf3 server 已启动"
    else
        echo "[server] 未安装 iperf3，跳过（可选: apt install iperf3）"
    fi

    ./$TCP_BIN tcp_recv.bin &
    ./$RDMA_BIN rdma_recv.bin &
    wait
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

    echo "================ [1/3] RDMA 传输 ================"
    sleep 1   # 等 server 就绪
    ./$RDMA_CLI "$server_ip" "$BENCH_FILE"

    echo ""
    echo "================ [2/3] TCP+sendfile 传输 ================"
    ./$TCP_CLI "$server_ip" "$BENCH_FILE"

    echo ""
    echo "================ [3/3] iperf3 TCP 裸链路（参照上限） ================"
    if command -v iperf3 >/dev/null 2>&1; then
        # 内存到内存：无磁盘开销，代表 TCP 链路吞吐上限
        iperf3 -c "$server_ip" -t 5 2>/dev/null | grep -E "sender|receiver|SUM" | tail -4
    else
        echo "[client] 未安装 iperf3，跳过（可选: apt install iperf3）"
    fi
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

