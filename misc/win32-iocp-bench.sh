#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 nginx.exe [iocp|select|poll] [on|off] [label]" >&2
    exit 2
fi

binary=$(realpath "$1")
backend=${2:-iocp}
sendfile=${3:-on}
label=${4:-$(basename "$(dirname "$binary")")-$backend-sendfile-$sendfile}
script_dir=$(cd "$(dirname "$0")" && pwd)
node_bin=${NODE_BIN:-$(command -v node || true)}
node_output_file=${NODE_OUTPUT_FILE:-0}
aggregate_node_bin=${FILTER_NODE_BIN:-$(command -v node || true)}
results_dir=${RESULTS_DIR:-$PWD/bench-results}
scratch_root=${BENCH_SCRATCH_ROOT:-${TMPDIR:-/tmp}}
protocol=${BENCH_PROTOCOL:-http}
connections=${CONNECTIONS:-32}
client_processes=${CLIENT_PROCESSES:-1}
worker_connections=${WORKER_CONNECTIONS:-1024}
worker_processes=${WORKER_PROCESSES:-1}
warmup_ms=${WARMUP_MS:-1000}
sample_ms=${SAMPLE_MS:-2000}
samples=${SAMPLES:-3}
bench_paths=${BENCH_PATHS:-/empty.gif /1k.bin /64k.bin /1m.bin}
request_header_padding_bytes=${REQUEST_HEADER_PADDING_BYTES:-0}
connection_mode=${CONNECTION_MODE:-keepalive}
read_pause_ms=${READ_PAUSE_MS:-0}
master_process=${MASTER_PROCESS:-on}
error_log_level=${ERROR_LOG_LEVEL:-notice}
reload_smoke=${RELOAD_SMOKE:-0}
cpu_wsl_average_max=${CPU_WSL_AVERAGE_MAX:-20}
cpu_wsl_sample_max=${CPU_WSL_SAMPLE_MAX:-35}
cpu_windows_max=${CPU_WINDOWS_MAX:-25}
memory_wsl_available_min_mb=${MEMORY_WSL_AVAILABLE_MIN_MB:-1024}
memory_windows_available_min_mb=${MEMORY_WINDOWS_AVAILABLE_MIN_MB:-2048}
cmd_bin=${CMD_BIN:-$(command -v cmd.exe || true)}
msys_bash=${MSYS_BASH:-/usr/bin/bash}

case "$backend" in
    iocp|select|poll) ;;
    *) echo "unsupported event backend: $backend" >&2; exit 2 ;;
esac

case "$sendfile" in
    on|off) ;;
    *) echo "sendfile must be on or off" >&2; exit 2 ;;
esac

case "$protocol" in
    http|https) ;;
    *) echo "BENCH_PROTOCOL must be http or https" >&2; exit 2 ;;
esac

case "$connection_mode" in
    keepalive|close) ;;
    *) echo "CONNECTION_MODE must be keepalive or close" >&2; exit 2 ;;
esac

case "$master_process" in
    on|off) ;;
    *) echo "MASTER_PROCESS must be on or off" >&2; exit 2 ;;
esac

case "$reload_smoke" in
    0|1) ;;
    *) echo "RELOAD_SMOKE must be 0 or 1" >&2; exit 2 ;;
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

case "$client_processes" in
    ''|*[!0-9]*)
        echo "CLIENT_PROCESSES must be a positive integer" >&2
        exit 2
        ;;
esac

if [ "$client_processes" -lt 1 ] || [ "$client_processes" -gt "$connections" ]; then
    echo "CLIENT_PROCESSES must be from 1 through CONNECTIONS" >&2
    exit 2
fi

if [ "$client_processes" -gt 1 ] && [ "$node_output_file" != 1 ]; then
    echo "CLIENT_PROCESSES greater than 1 requires NODE_OUTPUT_FILE=1" >&2
    exit 2
fi

if [ "$client_processes" -gt 1 ] && [ ! -x "$aggregate_node_bin" ]; then
    echo "aggregate Node.js binary is not executable: $aggregate_node_bin" >&2
    exit 2
fi

case "$read_pause_ms" in
    ''|*[!0-9]*)
        echo "READ_PAUSE_MS must be a non-negative integer" >&2
        exit 2
        ;;
esac

case "$request_header_padding_bytes" in
    ''|*[!0-9]*)
        echo "REQUEST_HEADER_PADDING_BYTES must be an integer" >&2
        exit 2
        ;;
esac

if [ "$request_header_padding_bytes" -gt 12000 ]; then
    echo "REQUEST_HEADER_PADDING_BYTES must be at most 12000" >&2
    exit 2
fi

if [ ! -x "$binary" ]; then
    echo "nginx binary is not executable: $binary" >&2
    exit 2
fi

if [ ! -x "$node_bin" ]; then
    echo "Node.js binary is not executable: $node_bin" >&2
    exit 2
fi

if [ ! -x "$cmd_bin" ] || [ ! -x "$msys_bash" ]; then
    echo "cmd.exe or MSYS2 bash is unavailable" >&2
    exit 2
fi

mkdir -p "$results_dir"

if [ ! -d "$scratch_root" ]; then
    echo "benchmark scratch root does not exist: $scratch_root" >&2
    exit 2
fi

wait_for_idle_cpu() {
    local attempt linux_avg linux_max linux_free_mb sample windows_free_mb
    local windows_load windows_sample

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

        linux_free_mb=$(awk '
            /^MemAvailable:/ { available = $2 }
            /^MemFree:/      { free = $2 }
            /^Buffers:/      { buffers = $2 }
            /^Cached:/       { cached = $2 }
            /^SReclaimable:/ { reclaimable = $2 }
            /^Shmem:/        { shmem = $2 }
            END {
                if (!available) {
                    available = free + buffers + cached + reclaimable - shmem
                }

                print int(available / 1024)
            }
        ' /proc/meminfo)

        windows_sample=$(
            "$cmd_bin" /d /s /c \
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
                ' || true
        )

        read -r windows_load windows_free_mb <<<"$windows_sample"

        if [[ ! "$windows_load" =~ ^[0-9]+([.][0-9]+)?$ ]]
        then
            printf 'CPU busy (attempt %d/50): Windows load unavailable\n' \
                "$attempt" >&2
            sleep 3
            continue
        fi

        if [[ ! "$windows_free_mb" =~ ^[0-9]+$ ]]
        then
            printf 'CPU busy (attempt %d/50): Windows memory unavailable\n' \
                "$attempt" >&2
            sleep 3
            continue
        fi

        if awk -v average="$linux_avg" -v maximum="$linux_max" \
               -v windows="$windows_load" \
               -v linux_free="$linux_free_mb" \
               -v windows_free="$windows_free_mb" \
               -v average_max="$cpu_wsl_average_max" \
               -v sample_max="$cpu_wsl_sample_max" \
               -v windows_max="$cpu_windows_max" \
               -v linux_free_min="$memory_wsl_available_min_mb" \
               -v windows_free_min="$memory_windows_available_min_mb" \
               'BEGIN {
                    exit !(average <= average_max &&
                           maximum <= sample_max &&
                           windows <= windows_max &&
                           linux_free >= linux_free_min &&
                           windows_free >= windows_free_min)
                }'
        then
            printf 'CPU gate passed: WSL average=%s%% max=%s%% free=%s MiB, Windows=%s%% free=%s MiB\n' \
                "$linux_avg" "$linux_max" "$linux_free_mb" \
                "$windows_load" "$windows_free_mb" >&2
            printf '%s %s %s %s %s\n' "$linux_avg" "$linux_max" \
                "$windows_load" "$linux_free_mb" "$windows_free_mb"
            return 0
        fi

        printf 'CPU busy (attempt %d/50): WSL average=%s%% max=%s%% free=%s MiB, Windows=%s%% free=%s MiB\n' \
            "$attempt" "$linux_avg" "$linux_max" "$linux_free_mb" \
            "$windows_load" "$windows_free_mb" >&2
        sleep 3
    done

    echo 'CPU did not become idle within five minutes' >&2
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
        if curl -k -fsS --max-time 1 "$health_url" \
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
        if ! curl -k -fsS --max-time 1 "$health_url" \
            >/dev/null 2>&1
        then
            return 0
        fi

        sleep 0.1
    done

    return 1
}

prefix=$(mktemp -d "$scratch_root/nginx-iocp-bench.XXXXXX")
prefix_win=$(wslpath -m "$prefix")
port=$(get_free_port)
started=0
starter_pid=0

cleanup() {
    if [ "$started" -eq 1 ]; then
        "$binary" -p "$prefix_win/" -c nginx.conf -s quit \
            >/dev/null 2>&1 || true
        wait_down || true
        if [ "$starter_pid" -ne 0 ]; then
            wait "$starter_pid" 2>/dev/null || true
        fi
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
truncate -s 4194304 "$prefix/html/4m.bin"
truncate -s 8388608 "$prefix/html/8m.bin"
truncate -s 16777216 "$prefix/html/16m.bin"

health_url="http://127.0.0.1:$port/empty.gif"
listen_directive="listen 127.0.0.1:$port;"
bench_tls=0

if [ "$protocol" = https ]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 2 \
        -subj /CN=localhost \
        -keyout "$prefix/conf-cert.key" \
        -out "$prefix/conf-cert.pem" >/dev/null 2>&1
    prefix_win_cert=$(wslpath -m "$prefix/conf-cert.pem")
    prefix_win_key=$(wslpath -m "$prefix/conf-cert.key")
    health_url="https://127.0.0.1:$port/empty.gif"
    listen_directive="listen 127.0.0.1:$port ssl;"
    bench_tls=1
fi

event_extra=
if [ "$backend" = iocp ]; then
    event_extra=$'    iocp_threads 1;\n    post_acceptex 32;'
fi

http_extra=
if [ "$request_header_padding_bytes" -gt 0 ]; then
    http_extra=$'    client_header_buffer_size 16k;\n    large_client_header_buffers 4 16k;'
fi

cat >"$prefix/nginx.conf" <<EOF
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
$http_extra

    server {
        $listen_directive
EOF

if [ "$protocol" = https ]; then
    cat >>"$prefix/nginx.conf" <<EOF
        ssl_certificate $prefix_win_cert;
        ssl_certificate_key $prefix_win_key;
        ssl_session_cache off;
EOF
fi

cat >>"$prefix/nginx.conf" <<EOF
        location = /empty.gif { empty_gif; }
        location / { root $prefix_win/html; }
    }
}
EOF

read -r linux_average linux_maximum windows_load linux_free_mb \
    windows_free_mb < <(wait_for_idle_cpu)

metadata="$results_dir/$label.meta"
{
    printf 'binary=%s\n' "$binary"
    printf 'backend=%s\n' "$backend"
    printf 'sendfile=%s\n' "$sendfile"
    printf 'protocol=%s\n' "$protocol"
    printf 'connections=%s\n' "$connections"
    printf 'client_processes=%s\n' "$client_processes"
    printf 'worker_connections=%s\n' "$worker_connections"
    printf 'worker_processes=%s\n' "$worker_processes"
    printf 'warmup_ms=%s\n' "$warmup_ms"
    printf 'sample_ms=%s\n' "$sample_ms"
    printf 'samples=%s\n' "$samples"
    printf 'paths=%s\n' "$bench_paths"
    printf 'request_header_padding_bytes=%s\n' \
        "$request_header_padding_bytes"
    printf 'connection_mode=%s\n' "$connection_mode"
    printf 'read_pause_ms=%s\n' "$read_pause_ms"
    printf 'master_process=%s\n' "$master_process"
    printf 'error_log_level=%s\n' "$error_log_level"
    printf 'reload_smoke=%s\n' "$reload_smoke"
    printf 'prefix=%s\n' "$prefix"
    printf 'cpu_wsl_average=%s\n' "$linux_average"
    printf 'cpu_wsl_maximum=%s\n' "$linux_maximum"
    printf 'cpu_windows=%s\n' "$windows_load"
    printf 'memory_wsl_available_mb=%s\n' "$linux_free_mb"
    printf 'memory_windows_available_mb=%s\n' "$windows_free_mb"
    printf 'cpu_wsl_average_max=%s\n' "$cpu_wsl_average_max"
    printf 'cpu_wsl_sample_max=%s\n' "$cpu_wsl_sample_max"
    printf 'cpu_windows_max=%s\n' "$cpu_windows_max"
    printf 'memory_wsl_available_min_mb=%s\n' \
        "$memory_wsl_available_min_mb"
    printf 'memory_windows_available_min_mb=%s\n' \
        "$memory_windows_available_min_mb"
    "$binary" -V 2>&1
} >"$metadata"

"$binary" -p "$prefix_win/" -c nginx.conf -e stderr -t

"$binary" -p "$prefix_win/" -c nginx.conf -e stderr \
    >"$prefix/logs/launch.log" 2>&1 &
starter_pid=$!
started=1

if ! wait_up; then
    tail -100 "$prefix/logs/error.log" >&2 || true
    exit 1
fi

if [ "$reload_smoke" = 1 ]; then
    "$binary" -p "$prefix_win/" -c nginx.conf -s reload
    sleep 0.2

    if ! wait_up; then
        tail -100 "$prefix/logs/error.log" >&2 || true
        exit 1
    fi
fi

for request_path in $bench_paths; do
    name=${request_path#/}
    name=${name%.*}

    read -r path_linux_average path_linux_maximum path_windows_load \
        path_linux_free_mb path_windows_free_mb \
        < <(wait_for_idle_cpu)
    printf 'path=%s cpu_wsl_average=%s cpu_wsl_maximum=%s cpu_windows=%s memory_wsl_available_mb=%s memory_windows_available_mb=%s\n' \
        "$request_path" "$path_linux_average" "$path_linux_maximum" \
        "$path_windows_load" "$path_linux_free_mb" \
        "$path_windows_free_mb" >>"$metadata"

    bench_script=http-loopback-bench.js
    if [ "$connection_mode" = close ]; then
        bench_script=http-connect-bench.js
    fi

    result_file="$results_dir/$label-$name.jsonl"

    if [ "$node_output_file" = 1 ]; then
        rm -f -- "$result_file"
        touch "$result_file"

        if [ "$client_processes" = 1 ]; then
            BENCH_TLS="$bench_tls" READ_PAUSE_MS="$read_pause_ms" \
            "$node_bin" "$script_dir/$bench_script" \
                "$port" "$request_path" "$connections" "$warmup_ms" \
                "$sample_ms" "$samples" "$request_header_padding_bytes" \
                "$(wslpath -m "$result_file")" "$prefix_win"
        else
            client_base=$((connections / client_processes))
            client_extra=$((connections % client_processes))
            client_parts=()
            client_pids=()

            for client_index in $(seq 1 "$client_processes"); do
                client_connections=$client_base
                if [ "$client_index" -le "$client_extra" ]; then
                    client_connections=$((client_connections + 1))
                fi

                client_part="$results_dir/$label-$name-client${client_index}.jsonl"
                rm -f -- "$client_part"
                touch "$client_part"
                client_parts+=("$client_part")

                BENCH_TLS="$bench_tls" READ_PAUSE_MS="$read_pause_ms" \
                "$node_bin" "$script_dir/$bench_script" \
                    "$port" "$request_path" "$client_connections" \
                    "$warmup_ms" "$sample_ms" "$samples" \
                    "$request_header_padding_bytes" \
                    "$(wslpath -m "$client_part")" "$prefix_win" &
                client_pids+=("$!")
            done

            client_status=0
            for client_pid in "${client_pids[@]}"; do
                if ! wait "$client_pid"; then
                    client_status=1
                fi
            done

            if [ "$client_status" -ne 0 ]; then
                echo "one or more benchmark client processes failed" >&2
                exit 1
            fi

            "$aggregate_node_bin" "$script_dir/aggregate-bench.js" \
                "$result_file" "${client_parts[@]}"
        fi

        cat "$result_file"
    else
        BENCH_TLS="$bench_tls" READ_PAUSE_MS="$read_pause_ms" \
        "$node_bin" "$script_dir/$bench_script" \
            "$port" "$request_path" "$connections" "$warmup_ms" \
            "$sample_ms" "$samples" "$request_header_padding_bytes" \
            "" | tee "$result_file"
    fi
done

"$binary" -p "$prefix_win/" -c nginx.conf -s quit
wait_down
started=0

if grep -E '\[(emerg|alert|crit)\]|shutdown left pending' \
    "$prefix/logs/error.log" >/dev/null
then
    tail -100 "$prefix/logs/error.log" >&2
    exit 1
fi

printf 'results written to %s\n' "$results_dir" >&2
