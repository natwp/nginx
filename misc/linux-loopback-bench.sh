#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 nginx [label]" >&2
    exit 2
fi

binary=$(realpath "$1")
label=${2:-$(basename "$(dirname "$binary")")-epoll-sendfile-on}
script_dir=$(cd "$(dirname "$0")" && pwd)
node_bin=${NODE_BIN:-$(command -v node || true)}
results_dir=${RESULTS_DIR:-$PWD/bench-results}
connections=${CONNECTIONS:-32}
warmup_ms=${WARMUP_MS:-1000}
sample_ms=${SAMPLE_MS:-2000}
samples=${SAMPLES:-3}
bench_paths=${BENCH_PATHS:-/empty.gif /1k.bin /64k.bin /1m.bin}
cpu_wsl_average_max=${CPU_WSL_AVERAGE_MAX:-20}
cpu_wsl_sample_max=${CPU_WSL_SAMPLE_MAX:-35}
cpu_windows_max=${CPU_WINDOWS_MAX:-25}
cmd_bin=${CMD_BIN:-$(command -v cmd.exe || true)}

if [ ! -x "$binary" ]; then
    echo "nginx binary is not executable: $binary" >&2
    exit 2
fi

if [ ! -x "$node_bin" ]; then
    echo "Node.js binary is not executable: $node_bin" >&2
    exit 2
fi

if [ ! -x "$cmd_bin" ]; then
    echo "cmd.exe is unavailable" >&2
    exit 2
fi

mkdir -p "$results_dir"

wait_for_idle_cpu() {
    local attempt linux_avg linux_max windows_load sample

    for attempt in $(seq 1 50); do
        sample=$(LC_ALL=C mpstat 1 3)

        read -r linux_avg linux_max < <(
            awk '
                $2 == "all" {
                    used = 100 - $NF
                    if ($1 == "Average:") {
                        average = used
                    } else if (used > maximum) {
                        maximum = used
                    }
                }
                END { printf "%.2f %.2f\n", average, maximum }
            ' <<<"$sample"
        )

        windows_load=$("$cmd_bin" /d /s /c \
            'wmic cpu get LoadPercentage /value' 2>/dev/null \
            | tr -d '\r' | awk -F= '
                $1 == "LoadPercentage" && $2 ~ /^[0-9.]+$/ {
                    cpu += $2
                    cpus++
                }
                END {
                    if (cpus) {
                        printf "%.2f\n", cpu / cpus
                    }
                }
            ')

        if [[ ! "$windows_load" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
            printf 'CPU busy (attempt %d/50): Windows load unavailable\n' \
                "$attempt" >&2
            sleep 3
            continue
        fi

        if awk -v average="$linux_avg" -v maximum="$linux_max" \
               -v windows="$windows_load" \
               -v average_max="$cpu_wsl_average_max" \
               -v sample_max="$cpu_wsl_sample_max" \
               -v windows_max="$cpu_windows_max" \
               'BEGIN {
                    exit !(average <= average_max &&
                           maximum <= sample_max &&
                           windows <= windows_max)
                }'
        then
            printf 'CPU gate passed: WSL average=%s%% max=%s%%, Windows=%s%%\n' \
                "$linux_avg" "$linux_max" "$windows_load" >&2
            printf '%s %s %s\n' "$linux_avg" "$linux_max" "$windows_load"
            return 0
        fi

        printf 'CPU busy (attempt %d/50): WSL average=%s%% max=%s%%, Windows=%s%%\n' \
            "$attempt" "$linux_avg" "$linux_max" "$windows_load" >&2
        sleep 3
    done

    echo 'CPU did not become idle within five minutes' >&2
    return 1
}

get_free_port() {
    python3 -c 'import socket; s=socket.socket(socket.AF_INET, socket.SOCK_STREAM); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()'
}

wait_up() {
    local attempt

    for attempt in $(seq 1 100); do
        if curl -fsS --max-time 1 "http://127.0.0.1:$port/empty.gif" \
            >/dev/null 2>&1
        then
            return 0
        fi

        sleep 0.1
    done

    return 1
}

wait_down() {
    local attempt

    for attempt in $(seq 1 100); do
        if ! curl -fsS --max-time 1 "http://127.0.0.1:$port/empty.gif" \
            >/dev/null 2>&1
        then
            return 0
        fi

        sleep 0.1
    done

    return 1
}

prefix=$(mktemp -d)
port=$(get_free_port)
started=0

cleanup() {
    if [ "$started" -eq 1 ]; then
        "$binary" -p "$prefix/" -c nginx.conf -s quit \
            >/dev/null 2>&1 || true
        wait_down || true
    fi

    if [ "${KEEP_ARTIFACTS:-0}" != 1 ]; then
    rm -rf -- "$prefix"
    fi
}

trap cleanup EXIT INT TERM

mkdir -p "$prefix/logs" "$prefix/html" "$prefix/temp"
truncate -s 1024 "$prefix/html/1k.bin"
truncate -s 65536 "$prefix/html/64k.bin"
truncate -s 1048576 "$prefix/html/1m.bin"

cat >"$prefix/nginx.conf" <<EOF
worker_processes 1;
error_log logs/error.log notice;
pid logs/nginx.pid;

events {
    use epoll;
    worker_connections 1024;
}

http {
    access_log off;
    server_tokens off;
    sendfile on;
    keepalive_timeout 30;
    keepalive_requests 1000000;

    server {
        listen 127.0.0.1:$port;
        location = /empty.gif { empty_gif; }
        location / { root $prefix/html; }
    }
}
EOF

read -r linux_average linux_maximum windows_load < <(wait_for_idle_cpu)

metadata="$results_dir/$label.meta"
{
    printf 'binary=%s\n' "$binary"
    printf 'backend=epoll\n'
    printf 'sendfile=on\n'
    printf 'connections=%s\n' "$connections"
    printf 'warmup_ms=%s\n' "$warmup_ms"
    printf 'sample_ms=%s\n' "$sample_ms"
    printf 'samples=%s\n' "$samples"
    printf 'paths=%s\n' "$bench_paths"
    printf 'cpu_wsl_average=%s\n' "$linux_average"
    printf 'cpu_wsl_maximum=%s\n' "$linux_maximum"
    printf 'cpu_windows=%s\n' "$windows_load"
    printf 'cpu_wsl_average_max=%s\n' "$cpu_wsl_average_max"
    printf 'cpu_wsl_sample_max=%s\n' "$cpu_wsl_sample_max"
    printf 'cpu_windows_max=%s\n' "$cpu_windows_max"
    "$binary" -V 2>&1
} >"$metadata"

"$binary" -p "$prefix/" -c nginx.conf -t
"$binary" -p "$prefix/" -c nginx.conf
started=1

if ! wait_up; then
    tail -100 "$prefix/logs/error.log" >&2 || true
    exit 1
fi

for request_path in $bench_paths; do
    name=${request_path#/}
    name=${name%.*}

    read -r path_linux_average path_linux_maximum path_windows_load \
        < <(wait_for_idle_cpu)
    printf 'path=%s cpu_wsl_average=%s cpu_wsl_maximum=%s cpu_windows=%s\n' \
        "$request_path" "$path_linux_average" "$path_linux_maximum" \
        "$path_windows_load" >>"$metadata"

    "$node_bin" "$script_dir/http-loopback-bench.js" \
        "$port" "$request_path" "$connections" "$warmup_ms" \
        "$sample_ms" "$samples" \
        | tee "$results_dir/$label-$name.jsonl"
done

"$binary" -p "$prefix/" -c nginx.conf -s quit
wait_down
started=0

if grep -E '\[(emerg|alert|crit)\]' "$prefix/logs/error.log" >/dev/null
then
    tail -100 "$prefix/logs/error.log" >&2
    exit 1
fi

printf 'results written to %s\n' "$results_dir" >&2
