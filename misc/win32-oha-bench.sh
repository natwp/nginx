#!/usr/bin/env bash

set -euo pipefail

bench_repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
bench_scratch="$bench_repo/.git/benchmark-work"
mkdir -p "$bench_scratch"

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
    echo "usage: $0 nginx.exe [iocp|select|poll] [label]" >&2
    exit 2
fi

binary=$(realpath "$1")
backend=$2
label=${3:-$(basename "$(dirname "$binary")")-$backend-oha}
script_dir=$(cd "$(dirname "$0")" && pwd)
node_bin=${NODE_BIN:-$script_dir/win32-node22-wrapper.sh}
oha_bin=${OHA_BIN:-$script_dir/../oha-windows-amd64-pgo.exe}
scratch_root=${BENCH_SCRATCH_ROOT:-$bench_scratch}
results_dir=${RESULTS_DIR:-$bench_scratch/nginx-oha-results-20260819}
paths=${BENCH_PATHS:-/empty.gif /64k.bin}
connections=${CONNECTIONS:-32}
client_processes=${CLIENT_PROCESSES:-1}
worker_connections=${WORKER_CONNECTIONS:-2048}
worker_processes=${WORKER_PROCESSES:-1}
warmup_duration=${OHA_WARMUP_DURATION:-2s}
duration=${OHA_DURATION:-5s}
sendfile=${SENDFILE:-off}
proxy_upstream=${BENCH_PROXY_UPSTREAM_URL:-}
proxy_buffering=${BENCH_PROXY_BUFFERING:-off}
proxy_keepalive=${BENCH_PROXY_KEEPALIVE:-64}
master_process=${MASTER_PROCESS:-on}
daemon=${DAEMON:-on}
direct_quit=${BENCH_DIRECT_QUIT:-0}
error_log_level=${ERROR_LOG_LEVEL:-notice}
gprof_dir=${NGX_GPROF_DIR:-}
msys_bash=${MSYS_BASH:-/usr/bin/bash}
cmd_bin=${CMD_BIN:-$(command -v cmd.exe || true)}
cpu_gate_linux_avg=${CPU_GATE_LINUX_AVG:-8}
cpu_gate_linux_max=${CPU_GATE_LINUX_MAX:-15}
cpu_gate_windows=${CPU_GATE_WINDOWS:-12}
cpu_gate_streak=${CPU_GATE_STREAK:-1}
cpu_gate_attempts=${CPU_GATE_ATTEMPTS:-120}
cpu_gate_retry_delay=${CPU_GATE_RETRY_DELAY:-0}

case "$backend" in
    iocp|select|poll) ;;
    *) echo "unsupported event backend: $backend" >&2; exit 2 ;;
esac

case "$sendfile" in
    on|off) ;;
    *) echo "SENDFILE must be on or off" >&2; exit 2 ;;
esac

case "$proxy_buffering" in
    on|off) ;;
    *) echo "BENCH_PROXY_BUFFERING must be on or off" >&2; exit 2 ;;
esac

if [ -n "$proxy_upstream" ]; then
    case "$proxy_upstream" in
        http://*) ;;
        *) echo "BENCH_PROXY_UPSTREAM_URL must be an http:// URL" >&2; exit 2 ;;
    esac

    if ! [[ "$proxy_keepalive" =~ ^[0-9]+$ ]]; then
        echo "BENCH_PROXY_KEEPALIVE must be a non-negative integer" >&2
        exit 2
    fi

    proxy_authority=${proxy_upstream#http://}
    proxy_authority=${proxy_authority%%/*}
    proxy_path=${proxy_upstream#http://$proxy_authority}
    if [ -z "$proxy_path" ]; then
        proxy_path=/
    fi
fi

case "$master_process" in
    on|off) ;;
    *) echo "MASTER_PROCESS must be on or off" >&2; exit 2 ;;
esac

case "$daemon" in
    on|off) ;;
    *) echo "DAEMON must be on or off" >&2; exit 2 ;;
esac

case "$direct_quit" in
    0|1) ;;
    *) echo "BENCH_DIRECT_QUIT must be 0 or 1" >&2; exit 2 ;;
esac

case "$scratch_root" in
    "$bench_scratch"|"$bench_scratch"/*) ;;
    *) echo "BENCH_SCRATCH_ROOT must be $bench_scratch or below it" >&2; exit 2 ;;
esac

case "$results_dir" in
    "$bench_scratch"|"$bench_scratch"/*) ;;
    *) echo "RESULTS_DIR must be $bench_scratch or below it" >&2; exit 2 ;;
esac

if [ -n "$gprof_dir" ]; then
    case "$gprof_dir" in
        "$bench_scratch"/*) ;;
        *) echo "NGX_GPROF_DIR must be below $bench_scratch" >&2; exit 2 ;;
    esac
fi

if [ ! -x "$binary" ] || [ ! -x "$node_bin" ] || [ ! -x "$oha_bin" ]; then
    echo "nginx, Node wrapper, or oha binary is not executable" >&2
    exit 2
fi

if ! [[ "$client_processes" =~ ^[1-9][0-9]*$ ]] \
   || [ "$client_processes" -gt "$connections" ]
then
    echo "CLIENT_PROCESSES must be between 1 and CONNECTIONS" >&2
    exit 2
fi

if [ ! -x "$msys_bash" ] || [ ! -x "$cmd_bin" ]; then
    echo "MSYS2 bash or cmd.exe is unavailable" >&2
    exit 2
fi

if ! [[ "$cpu_gate_streak" =~ ^[1-9][0-9]*$ ]] \
   || ! [[ "$cpu_gate_attempts" =~ ^[1-9][0-9]*$ ]]
then
    echo "CPU_GATE_STREAK and CPU_GATE_ATTEMPTS must be positive integers" >&2
    exit 2
fi

if ! [[ "$cpu_gate_retry_delay" =~ ^[0-9]+$ ]]; then
    echo "CPU_GATE_RETRY_DELAY must be a non-negative integer" >&2
    exit 2
fi

mkdir -p "$results_dir"

if [ -n "$gprof_dir" ]; then
    mkdir -p "$gprof_dir"
fi

wait_for_idle_cpu() {
    local attempt pass_streak sample linux_avg linux_max windows_load

    pass_streak=0

    for attempt in $(seq 1 "$cpu_gate_attempts"); do
        sample=$(LC_ALL=C mpstat 1 2)
        read -r linux_avg linux_max < <(
            awk '$2 == "all" {
                     used = 100 - $NF;
                     if ($1 == "Average:") average = used;
                     else if (used > maximum) maximum = used;
                 }
                 END { printf "%.2f %.2f\n", average, maximum }' \
                 <<<"$sample"
        )

        windows_load=$(
            "$cmd_bin" /d /s /c 'wmic cpu get LoadPercentage /value' \
                2>/dev/null | tr -d '\r' | awk -F= '
                    $1 == "LoadPercentage" && $2 ~ /^[0-9.]+$/ {
                        total += $2; count++
                    }
                    END { if (count) printf "%.2f\n", total / count }'
        )

        if awk -v a="$linux_avg" -v m="$linux_max" -v w="$windows_load" \
               -v ga="$cpu_gate_linux_avg" -v gm="$cpu_gate_linux_max" \
               -v gw="$cpu_gate_windows" \
               'BEGIN { exit !(a <= ga && m <= gm && w != "" && w <= gw) }'
        then
            pass_streak=$((pass_streak + 1))
            printf 'CPU gate pass %d/%d: WSL average=%s%% max=%s%% Windows=%s%%\n' \
                "$pass_streak" "$cpu_gate_streak" "$linux_avg" \
                "$linux_max" "$windows_load" >&2

            if [ "$pass_streak" -ge "$cpu_gate_streak" ]; then
                return 0
            fi

            continue
        fi

        pass_streak=0
        printf 'CPU busy (attempt %d/%d): WSL average=%s%% max=%s%% Windows=%s%%\n' \
            "$attempt" "$cpu_gate_attempts" "$linux_avg" "$linux_max" \
            "${windows_load:-unavailable}" >&2
        sleep "$cpu_gate_retry_delay"
    done

    echo 'CPU did not remain below the configured idle thresholds' >&2
    return 1
}

get_free_port() {
    "$msys_bash" -l -c \
        'export MSYSTEM=MINGW64; source /etc/profile >/dev/null 2>&1 || :; export PATH=/mingw64/bin:/usr/bin:$PATH; python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_STREAM); s.bind((\"127.0.0.1\", 0)); print(s.getsockname()[1]); s.close()"' \
        | tr -d '\r'
}

wait_up() {
    local attempt

    for attempt in $(seq 1 100); do
        if curl -fsS --max-time 1 "$health_url" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done

    return 1
}

prefix=$(mktemp -d "$scratch_root/nginx-oha.XXXXXX")
prefix_win=$(wslpath -m "$prefix")
port=$(get_free_port)
port_upstream=''
health_url="http://127.0.0.1:$port/empty.gif"
started=0

cleanup() {
    if [ "$started" -eq 1 ]; then
        if [ "$direct_quit" -eq 1 ]; then
            nginx_pid=$(ps -eo pid=,args= | awk -v binary="$binary" \
                -v prefix="$prefix_win" \
                'index($0, binary) && index($0, prefix) && index($0, " -s quit") == 0 { print $1; exit }')
            if [ -n "$nginx_pid" ]; then
                kill -TERM "$nginx_pid" 2>/dev/null || true
                wait "$starter_pid" 2>/dev/null || true
            fi
        else
            (
                cd "$prefix/control"
                "$binary" -p "$prefix_win/" -c nginx.conf -s quit \
                    >/dev/null 2>&1
            ) || true

            if [ "$daemon" = off ]; then
                wait "$starter_pid" 2>/dev/null || true
            fi
        fi
    fi
}

trap cleanup EXIT INT TERM

mkdir -p "$prefix/logs" "$prefix/html" "$prefix/temp" "$prefix/control"
truncate -s 1024 "$prefix/html/1k.bin"
truncate -s 65536 "$prefix/html/64k.bin"
truncate -s 1048576 "$prefix/html/1m.bin"

event_extra=''
if [ "$backend" = iocp ]; then
    event_extra=$'    iocp_threads 1;\n    post_acceptex 32;'
fi

cat >"$prefix/nginx.conf" <<EOF
daemon $daemon;
master_process $master_process;
worker_processes $worker_processes;
error_log logs/error.log $error_log_level;
pid logs/nginx.pid;

events {
    use $backend;
    worker_connections $worker_connections;
$event_extra
}

http {
    access_log off;
    server_tokens off;
    sendfile $sendfile;
    keepalive_timeout 30;
    keepalive_requests 1000000;

EOF

if [ -n "$proxy_upstream" ] && [ "$proxy_keepalive" -gt 0 ]; then
    cat >>"$prefix/nginx.conf" <<EOF
    upstream bench_proxy_upstream {
        server $proxy_authority;
        keepalive $proxy_keepalive;
    }

EOF
fi

cat >>"$prefix/nginx.conf" <<EOF
    server {
        listen 127.0.0.1:$port;
        location = /empty.gif { empty_gif; }
EOF

if [ -n "$proxy_upstream" ]; then
    cat >>"$prefix/nginx.conf" <<EOF
        location /proxy {
            proxy_http_version 1.1;
            proxy_set_header Connection "";
            proxy_set_header Host "localhost";
            proxy_buffering $proxy_buffering;
EOF

    if [ "$proxy_keepalive" -gt 0 ]; then
        cat >>"$prefix/nginx.conf" <<EOF
            proxy_pass http://bench_proxy_upstream$proxy_path;
EOF
    else
        cat >>"$prefix/nginx.conf" <<EOF
            proxy_pass $proxy_upstream;
EOF
    fi

    cat >>"$prefix/nginx.conf" <<'EOF'
        }
EOF
else
    cat >>"$prefix/nginx.conf" <<EOF
        location / { root $prefix_win/html; }
EOF
fi

cat >>"$prefix/nginx.conf" <<'EOF'
    }
}
EOF

metadata="$results_dir/$label.meta"
{
    printf 'binary=%s\n' "$binary"
    printf 'backend=%s\n' "$backend"
    printf 'label=%s\n' "$label"
    printf 'scratch_prefix=%s\n' "$prefix"
    printf 'connections=%s\n' "$connections"
    printf 'client_processes=%s\n' "$client_processes"
    printf 'worker_processes=%s\n' "$worker_processes"
    printf 'warmup_duration=%s\n' "$warmup_duration"
    printf 'duration=%s\n' "$duration"
    printf 'sendfile=%s\n' "$sendfile"
    printf 'proxy_upstream=%s\n' "$proxy_upstream"
    printf 'proxy_buffering=%s\n' "$proxy_buffering"
    printf 'proxy_keepalive=%s\n' "$proxy_keepalive"
    printf 'master_process=%s\n' "$master_process"
    printf 'daemon=%s\n' "$daemon"
    printf 'gprof_dir=%s\n' "$gprof_dir"
    printf 'cpu_gate_linux_avg=%s\n' "$cpu_gate_linux_avg"
    printf 'cpu_gate_linux_max=%s\n' "$cpu_gate_linux_max"
    printf 'cpu_gate_windows=%s\n' "$cpu_gate_windows"
    printf 'cpu_gate_streak=%s\n' "$cpu_gate_streak"
    printf 'cpu_gate_attempts=%s\n' "$cpu_gate_attempts"
    printf 'cpu_gate_retry_delay=%s\n' "$cpu_gate_retry_delay"
    printf 'cpu_gate_scope=before-measured-sample\n'
    "$binary" -V 2>&1
} >"$metadata"

(
    cd "$prefix/control"
    "$binary" -p "$prefix_win/" -c nginx.conf -e stderr -t
)

WSLENV="${WSLENV:+$WSLENV:}NGX_GPROF_DIR/p" NGX_GPROF_DIR="$gprof_dir" \
    "$binary" -p "$prefix_win/" -c nginx.conf -e stderr \
        >"$prefix/logs/launch.log" 2>&1 &
starter_pid=$!
started=1

if ! wait_up; then
    tail -100 "$prefix/logs/error.log" >&2 || true
    exit 1
fi

oha_win=$(wslpath -m "$oha_bin")

for request_path in $paths; do
    workload=$request_path
    target_path=$request_path

    if [ -n "$proxy_upstream" ]; then
        workload="proxy${request_path}"
        target_path=/proxy
    fi

    result_name=${workload#/}
    result_name=${result_name//\//_}
    result_file="$results_dir/$label-$result_name.jsonl"
    : >"$result_file"
    result_win=$(wslpath -m "$result_file")

    WIN32_NODE_LOG="$prefix/logs/node-$result_name-warmup.log" \
      "$node_bin" "$script_dir/win32-oha-runner.js" "$oha_win" \
        "http://127.0.0.1:$port$target_path" "$connections" \
        "$warmup_duration" "$prefix_win" "$result_win" "$backend" \
        "$label-warmup" "$workload" "$client_processes" >/dev/null

    wait_for_idle_cpu
    WIN32_NODE_LOG="$prefix/logs/node-$result_name-sample.log" \
      "$node_bin" "$script_dir/win32-oha-runner.js" "$oha_win" \
        "http://127.0.0.1:$port$target_path" "$connections" \
        "$duration" "$prefix_win" "$result_win" "$backend" "$label" \
        "$workload" "$client_processes" >/dev/null

    cat "$result_file"
done

printf 'results written to %s\n' "$results_dir" >&2
