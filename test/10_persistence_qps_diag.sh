#!/usr/bin/env bash
# Run on the Linux benchmark host after building kvstore and 1_RESP_testcase.
# Usage: bash test/10_persistence_qps_diag.sh [baseline snap aof aof-no-crc]
set -euo pipefail
export LC_ALL=C

root=$(cd -- "$(dirname -- "$0")/.." && pwd)
server=$root/build/kvstore
client=$root/build/1_RESP_testcase

# Optional affinity. The server's background threads inherit its CPU mask.
check_affinity() {
    local side=$1 cpus=$2
    if ! taskset -c "$cpus" true; then
        printf 'Invalid %s CPU list: %s\n' "$side" "$cpus" >&2
        taskset -pc "$$" >&2
        exit 1
    fi
}
server_cmd=("$server")
client_cmd=("$client")
if [[ -n ${KVS_DIAG_SERVER_CPUS:-} ]]; then
    check_affinity server "$KVS_DIAG_SERVER_CPUS"
    server_cmd=(taskset -c "$KVS_DIAG_SERVER_CPUS" "$server")
fi
if [[ -n ${KVS_DIAG_CLIENT_CPUS:-} ]]; then
    check_affinity client "$KVS_DIAG_CLIENT_CPUS"
    client_cmd=(taskset -c "$KVS_DIAG_CLIENT_CPUS" "$client")
fi
if [[ ! -x $server || ! -x $client ]]; then
    echo 'Build kvstore and 1_RESP_testcase first.' >&2
    exit 1
fi
if (( $# == 0 )); then
    set -- baseline snap aof aof-no-crc
fi
for mode in "$@"; do
    case "$mode" in
        baseline|snap|aof|aof-no-crc) ;;
        *) echo "Unknown mode: $mode" >&2; exit 1 ;;
    esac
done

results=$(mktemp -d "${TMPDIR:-/tmp}/kvs-persistence-qps.XXXXXX")
results=$(cd -- "$results" && pwd)
echo "Results: $results"
printf 'server_cpus=%s\nclient_cpus=%s\n' "${KVS_DIAG_SERVER_CPUS:-default}" "${KVS_DIAG_CLIENT_CPUS:-default}" > "$results/affinity.txt"
printf 'mode\tround\tqps\n' > "$results/qps.tsv"

pid=
stop_server() {
    if [[ -n $pid ]]; then
        kill -TERM "$pid" 2>/dev/null || true
        for ((i = 0; i < 100; i++)); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        if kill -0 "$pid" 2>/dev/null; then
            echo 'Benchmark server did not stop within 10 seconds.' >&2
            kill -KILL "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            pid=
            return 1
        fi
        wait "$pid"
        pid=
    fi
}
trap stop_server EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

port_open() {
    (exec 3<>/dev/tcp/127.0.0.1/2000) 2>/dev/null
}

for mode in "$@"; do
    if port_open; then
        echo 'Port 2000 is occupied. Stop the existing server before benchmarking.' >&2
        exit 1
    fi

    snap=false
    aof=false
    crc=false
    case "$mode" in
        snap) snap=true ;;
        aof) aof=true; crc=true ;;
        aof-no-crc) aof=true ;;
    esac

    run_dir=$(mktemp -d "$results/$mode.XXXXXX")
    mkdir "$run_dir/data" "$run_dir/logs"
    cat > "$run_dir/server.conf" <<EOF
role=master
master_port=2000
snap.enable=$snap
aof.enable=$aof
aof.crc=$crc
rdma.enable=false
ebpf.enable=false
leak.enable=false
log.level=info
EOF
    (
        cd -- "$run_dir"
        exec "${server_cmd[@]}" --config server.conf
    ) > "$run_dir/server.out" 2>&1 &
    pid=$!

    ready=false
    for ((attempt = 0; attempt < 100; attempt++)); do
        kill -0 "$pid" 2>/dev/null || break
        if port_open && kill -0 "$pid" 2>/dev/null; then
            ready=true
            break
        fi
        sleep 0.05
    done
    if [[ $ready != true ]] || grep -q 'async save FAILED\|AOF io_uring init FAILED' "$run_dir/server.out"; then
        echo "Server initialization failed: $run_dir/server.out" >&2
        exit 1
    fi

    echo "[$mode] warm-up"
    for round in 0 1 2 3 4 5; do
        log=$run_dir/client-$round.out
        timeout 120s "${client_cmd[@]}" 8 0 | tee "$log"
        if ! qps=$(awk '
            /^\[Echo insert\]/ {
                pass = fail = qps = ""
                for (i = 1; i <= NF; i++) {
                    field = $i
                    sub(/[,\r]+$/, "", field)
                    if (field ~ /^qps=[0-9]+$/) qps = substr(field, 5)
                    if (field ~ /^pass=[0-9]+$/) pass = substr(field, 6)
                    if (field ~ /^fail=[0-9]+$/) fail = substr(field, 6)
                }
                if (pass == "100000" && fail == "0" && qps + 0 > 0) {
                    sample = qps
                    matches++
                }
            }
            END { if (matches != 1) exit 1; print sample }
        ' "$log"); then
            echo "Invalid sample: $log" >&2
            exit 1
        fi
        if (( round > 0 )); then
            printf '%s\t%s\t%s\n' "$mode" "$round" "$qps" >> "$results/qps.tsv"
        fi
    done
    stop_server
done

awk 'NR > 1 { sum[$1] += $3; count[$1]++ }
     END { for (mode in sum) printf "%-12s mean=%8.1f QPS  n=%d\n", mode, sum[mode]/count[mode], count[mode] }' "$results/qps.tsv"
echo "Results: $results"
