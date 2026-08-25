#!/usr/bin/env bash

set -euo pipefail

bench_repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
bench_scratch="$bench_repo/.git/benchmark-work"
mkdir -p "$bench_scratch"

if [ "$#" -lt 1 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 nginx.exe [iocp|select|poll] [label] [udp-receives]" >&2
    exit 2
fi

binary=$(realpath "$1")
backend=${2:-iocp}
label=${3:-$(basename "$(dirname "$binary")")-$backend-udp}
udp_receives=${4:-64}
script_dir=$(cd "$(dirname "$0")" && pwd)
node_bin=${NODE_BIN:-$(command -v node || true)}
node_output_file=${NODE_OUTPUT_FILE:-0}
results_dir=${RESULTS_DIR:-$PWD/bench-results}
connections=${CONNECTIONS:-32}
payload_bytes=${PAYLOAD_BYTES:-64}
response_bytes=${RESPONSE_BYTES:-8}
warmup_ms=${WARMUP_MS:-1000}
sample_ms=${SAMPLE_MS:-2000}
samples=${SAMPLES:-3}
worker_processes=${WORKER_PROCESSES:-1}
error_log_level=${ERROR_LOG_LEVEL:-notice}
cpu_wsl_average_max=${CPU_WSL_AVERAGE_MAX:-12}
cpu_wsl_sample_max=${CPU_WSL_SAMPLE_MAX:-20}
cpu_windows_max=${CPU_WINDOWS_MAX:-15}
memory_wsl_available_min_mb=${MEMORY_WSL_AVAILABLE_MIN_MB:-2048}
memory_windows_available_min_mb=${MEMORY_WINDOWS_AVAILABLE_MIN_MB:-4096}
cmd_bin=${CMD_BIN:-$(command -v cmd.exe || true)}
msys_bash=${MSYS_BASH:-/usr/bin/bash}

case "$backend" in
    iocp|select|poll) ;;
    *) echo "unsupported event backend: $backend" >&2; exit 2 ;;
esac

case "$udp_receives" in
    ''|*[!0-9]*) echo "udp receives must be an integer" >&2; exit 2 ;;
esac

case "$node_output_file" in
    0|1) ;;
    *) echo "NODE_OUTPUT_FILE must be 0 or 1" >&2; exit 2 ;;
esac

case "$worker_processes" in
    ''|*[!0-9]*)
        echo "WORKER_PROCESSES must be a positive integer" >&2
        exit 2
        ;;
esac

if [ "$worker_processes" -lt 1 ]; then
    echo "WORKER_PROCESSES must be at least 1" >&2
    exit 2
fi

if [ "$response_bytes" -lt 1 ] || [ "$response_bytes" -gt 4096 ]; then
    echo "response bytes must be from 1 to 4096" >&2
    exit 2
fi

if [ ! -x "$binary" ] || [ ! -x "$node_bin" ]; then
    echo "binary or Node.js executable is missing" >&2
    exit 2
fi

if [ ! -x "$cmd_bin" ] || [ ! -x "$msys_bash" ]; then
    echo "cmd.exe or MSYS2 bash is unavailable" >&2
    exit 2
fi

mkdir -p "$results_dir"

wait_for_idle_cpu() {
    local attempt linux_avg linux_max linux_free_mb sample
    local windows_free_mb windows_load windows_sample

    for attempt in $(seq 1 50); do
        sample=$(LC_ALL=C mpstat 1 3)

        read -r linux_avg linux_max < <(
            awk '
                $2 == "all" {
                    used = 100 - $NF
                    if ($1 == "Average:") average = used
                    else if (used > maximum) maximum = used
                }
                END { printf "%.2f %.2f\n", average, maximum }
            ' <<<"$sample"
        )

        linux_free_mb=$(awk '
            /^MemAvailable:/ { available = $2 }
            /^MemFree:/      { free = $2 }
            /^Buffers:/      { buffers = $2 }
            /^Cached:/       { cached = $2 }
            /^SReclaimable:/ { reclaimable = $2 }
            /^Shmem:/        { shmem = $2 }
            END {
                if (!available) available = free + buffers + cached + reclaimable - shmem
                print int(available / 1024)
            }
        ' /proc/meminfo)

        windows_sample=$("$cmd_bin" /d /s /c \
            'wmic cpu get LoadPercentage /value & wmic OS get FreePhysicalMemory /value' \
            2>/dev/null | tr -d '\r' | awk -F= '
                $1 == "LoadPercentage" && $2 ~ /^[0-9.]+$/ {
                    cpu += $2
                    cpus++
                }
                $1 == "FreePhysicalMemory" && $2 ~ /^[0-9]+$/ {
                    memory = int($2 / 1024)
                }
                END {
                    if (cpus && memory) {
                        printf "%.2f %d\n", cpu / cpus, memory
                    }
                }
            ' || true)
        read -r windows_load windows_free_mb <<<"$windows_sample"

        if [[ ! "$windows_load" =~ ^[0-9]+([.][0-9]+)?$ ]] \
            || [[ ! "$windows_free_mb" =~ ^[0-9]+$ ]]; then
            printf 'CPU busy (attempt %d/50): Windows telemetry unavailable\n' \
                "$attempt" >&2
            sleep 3
            continue
        fi

        if awk -v average="$linux_avg" -v maximum="$linux_max" \
               -v windows="$windows_load" -v linux_free="$linux_free_mb" \
               -v windows_free="$windows_free_mb" \
               -v average_max="$cpu_wsl_average_max" \
               -v sample_max="$cpu_wsl_sample_max" \
               -v windows_max="$cpu_windows_max" \
               -v linux_min="$memory_wsl_available_min_mb" \
               -v windows_min="$memory_windows_available_min_mb" \
               'BEGIN { exit !(average <= average_max && maximum <= sample_max && windows <= windows_max && linux_free >= linux_min && windows_free >= windows_min) }'; then
            printf 'CPU gate passed: WSL average=%s%% max=%s%% free=%s MiB, Windows=%s%% free=%s MiB\n' \
                "$linux_avg" "$linux_max" "$linux_free_mb" "$windows_load" "$windows_free_mb" >&2
            printf '%s %s %s %s %s\n' "$linux_avg" "$linux_max" "$windows_load" "$linux_free_mb" "$windows_free_mb"
            return 0
        fi

        printf 'CPU busy (attempt %d/50): WSL average=%s%% max=%s%% free=%s MiB, Windows=%s%% free=%s MiB\n' \
            "$attempt" "$linux_avg" "$linux_max" "$linux_free_mb" "$windows_load" "$windows_free_mb" >&2
        sleep 3
    done

    echo 'CPU did not become idle within five minutes' >&2
    return 1
}

get_free_port() {
    "$msys_bash" -l -c \
        'export MSYSTEM=MINGW64; source /etc/profile >/dev/null 2>&1 || :; export PATH=/mingw64/bin:/usr/bin:$PATH; python3 -c "import socket; s=socket.socket(socket.AF_INET, socket.SOCK_DGRAM); s.bind((\"127.0.0.1\", 0)); print(s.getsockname()[1]); s.close()"' \
        | tr -d '\r'
}

prefix=$(mktemp -d "$bench_scratch/nginx-iocp-udp-bench.XXXXXX")
prefix_win=$(wslpath -m "$prefix")
port=$(get_free_port)
started=0
starter_pid=0

cleanup() {
    if [ "$started" -eq 1 ]; then
        "$binary" -p "$prefix_win/" -c nginx.conf -s quit >/dev/null 2>&1 || true
        sleep 1
        if [ "$starter_pid" -ne 0 ]; then
            wait "$starter_pid" 2>/dev/null || true
        fi
    fi

    if [ "${KEEP_ARTIFACTS:-0}" != 1 ]; then
        case "$prefix" in
            "$bench_scratch"/nginx-iocp-udp-bench.*) rm -rf -- "$prefix" ;;
        esac
    fi
}

trap cleanup EXIT INT TERM

mkdir -p "$prefix/logs" "$prefix/temp"
response=$(printf '%*s' "$response_bytes" '' | tr ' ' U)
event_extra=
if [ "$backend" = iocp ]; then
    event_extra=$(printf '    iocp_threads 1;\n    iocp_udp_receives %s;' "$udp_receives")
fi

printf '%s\n' \
    "worker_processes $worker_processes;" \
    "error_log logs/error.log $error_log_level;" \
    'pid logs/nginx.pid;' \
    'events {' \
    "    use $backend;" \
    '    worker_connections 1024;' \
    "$event_extra" \
    '}' \
    'stream {' \
    '    server {' \
    "        listen 127.0.0.1:$port udp;" \
    "        return \"$response\";" \
    '    }' \
    '}' >"$prefix/nginx.conf"

read -r linux_average linux_maximum windows_load linux_free_mb \
    windows_free_mb < <(wait_for_idle_cpu)

metadata="$results_dir/$label.meta"
{
    printf 'binary=%s\n' "$binary"
    printf 'backend=%s\n' "$backend"
    printf 'connections=%s\n' "$connections"
    printf 'worker_processes=%s\n' "$worker_processes"
    printf 'payload_bytes=%s\n' "$payload_bytes"
    printf 'response_bytes=%s\n' "$response_bytes"
    printf 'udp_receives=%s\n' "$udp_receives"
    printf 'warmup_ms=%s\n' "$warmup_ms"
    printf 'sample_ms=%s\n' "$sample_ms"
    printf 'samples=%s\n' "$samples"
    printf 'prefix=%s\n' "$prefix"
    printf 'cpu_wsl_average=%s\n' "$linux_average"
    printf 'cpu_wsl_maximum=%s\n' "$linux_maximum"
    printf 'cpu_windows=%s\n' "$windows_load"
    printf 'memory_wsl_available_mb=%s\n' "$linux_free_mb"
    printf 'memory_windows_available_mb=%s\n' "$windows_free_mb"
    "$binary" -V 2>&1
} >"$metadata"

"$binary" -p "$prefix_win/" -c nginx.conf -e stderr -t
"$binary" -p "$prefix_win/" -c nginx.conf -e stderr \
    >"$prefix/logs/launch.log" 2>&1 &
starter_pid=$!
started=1
sleep 1

read -r path_linux_average path_linux_maximum path_windows_load \
    path_linux_free_mb path_windows_free_mb < <(wait_for_idle_cpu)
printf 'path=udp-%s cpu_wsl_average=%s cpu_wsl_maximum=%s cpu_windows=%s memory_wsl_available_mb=%s memory_windows_available_mb=%s\n' \
    "$payload_bytes" "$path_linux_average" "$path_linux_maximum" "$path_windows_load" "$path_linux_free_mb" "$path_windows_free_mb" >>"$metadata"

result_file="$results_dir/$label-udp.jsonl"

if [ "$node_output_file" = 1 ]; then
    rm -f -- "$result_file"
    touch "$result_file"
    "$node_bin" "$script_dir/udp-loopback-bench.js" "$port" \
        "$connections" "$payload_bytes" "$response_bytes" "$warmup_ms" \
        "$sample_ms" "$samples" "$(wslpath -m "$result_file")" "$prefix_win"
    cat "$result_file"
else
    "$node_bin" "$script_dir/udp-loopback-bench.js" "$port" \
        "$connections" "$payload_bytes" "$response_bytes" "$warmup_ms" \
        "$sample_ms" "$samples" "" | tee "$result_file"
fi

"$binary" -p "$prefix_win/" -c nginx.conf -s quit
started=0

if grep -E '\[(emerg|alert|crit)\]|shutdown left pending' \
    "$prefix/logs/error.log" >/dev/null; then
    tail -100 "$prefix/logs/error.log" >&2
    exit 1
fi

printf 'results written to %s\n' "$results_dir" >&2
