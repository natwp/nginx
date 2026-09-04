#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 nginx.exe [label]" >&2
    exit 2
fi

binary=$(realpath "$1")
label=${2:-$(basename "$(dirname "$binary")")}
script_dir=$(cd "$(dirname "$0")" && pwd)
results_root=${RESULTS_DIR:-./nginx-oha-matrix}
repeats=${REPEATS:-6}
max_attempts=${MAX_ATTEMPTS:-2}
resume=${RESUME:-0}
modes=${MATRIX_MODES:-fixed scaled}
paths=${BENCH_PATHS:-/empty.gif /64k.bin}
fixed_connections=${FIXED_CONNECTIONS:-48}
fixed_clients=${FIXED_CLIENT_PROCESSES:-4}
scaled_connections_per_worker=${SCALED_CONNECTIONS_PER_WORKER:-32}
scaled_clients_per_worker=${SCALED_CLIENTS_PER_WORKER:-1}
warmup_duration=${OHA_WARMUP_DURATION:-2s}
sample_duration=${OHA_DURATION:-7s}
filter_node=${FILTER_NODE_BIN:-$(command -v node || true)}

if [ -n "${MATRIX_CONDITIONS:-}" ]; then
    read -r -a conditions <<<"$MATRIX_CONDITIONS"
else
    conditions=(
        iocp-w1
        select-w1
        poll-w1
        iocp-w4
        select-w4
        poll-w4
    )
fi

if [ "${#conditions[@]}" -lt 2 ]; then
    echo "MATRIX_CONDITIONS must contain at least two conditions" >&2
    exit 2
fi

case "$results_root" in
    /*) ;;
    *) echo "RESULTS_DIR must be an absolute path" >&2; exit 2 ;;
esac

case "$repeats" in
    ''|*[!0-9]*) echo "REPEATS must be an integer" >&2; exit 2 ;;
esac

case "$max_attempts" in
    ''|*[!0-9]*) echo "MAX_ATTEMPTS must be an integer" >&2; exit 2 ;;
esac

case "$resume" in
    0|1) ;;
    *) echo "RESUME must be 0 or 1" >&2; exit 2 ;;
esac

if [ "$repeats" -lt 3 ] || [ "$max_attempts" -lt 1 ]; then
    echo "REPEATS must be at least 3 and MAX_ATTEMPTS at least 1" >&2
    exit 2
fi

if [ ! -x "$binary" ] || [ ! -x "$filter_node" ]; then
    echo "nginx or filter Node.js binary is not executable" >&2
    exit 2
fi

for mode in $modes; do
    case "$mode" in
        fixed|scaled) ;;
        *) echo "MATRIX_MODES supports only fixed and scaled" >&2; exit 2 ;;
    esac
done

if [ "$resume" = 0 ] && [ -d "$results_root" ] \
   && [ -n "$(rg --files "$results_root" 2>/dev/null | sed -n '1p')" ]
then
    echo "refusing to mix samples with existing results: $results_root" >&2
    exit 2
fi

mkdir -p "$results_root"

condition_values() {
    local condition

    condition=$1
    case "$condition" in
        wepoll-level-w*)
            backend=wepoll
            workers=${condition##*-w}
            wepoll_edge=off
            ;;
        wepoll-edge-w*)
            backend=wepoll
            workers=${condition##*-w}
            wepoll_edge=on
            ;;
        *)
            backend=${condition%-w*}
            workers=${condition##*-w}
            wepoll_edge=off
            ;;
    esac
}

load_values() {
    local mode workers

    mode=$1
    workers=$2

    if [ "$mode" = fixed ]; then
        connections=$fixed_connections
        clients=$fixed_clients
    else
        connections=$((scaled_connections_per_worker * workers))
        clients=$((scaled_clients_per_worker * workers))
    fi
}

run_one() {
    local attempt attempt_dir condition final_dir mode repeat run_label

    mode=$1
    repeat=$2
    condition=$3
    condition_values "$condition"
    load_values "$mode" "$workers"
    run_label="$label-$mode-$condition"
    final_dir="$results_root/$mode/$condition-raw/repeat$repeat-$condition"

    if [ "$resume" = 1 ] && [ -d "$final_dir" ] \
       && [ -n "$(rg --files "$final_dir" 2>/dev/null | sed -n '1p')" ]
    then
        echo "keeping completed run: $mode repeat $repeat $condition" >&2
        return 0
    fi

    mkdir -p "$results_root/$mode/$condition-raw"

    for attempt in $(seq 1 "$max_attempts"); do
        attempt_dir=$(mktemp -d \
            "$results_root/.${mode}-repeat${repeat}-${condition}.XXXXXX")

        echo "matrix $mode round $repeat/$repeats: $condition " \
             "(connections=$connections clients=$clients, " \
             "attempt $attempt/$max_attempts)" >&2

        if RESULTS_DIR="$attempt_dir" BENCH_SCRATCH_ROOT="${TMPDIR:-/tmp}" \
           MASTER_PROCESS=on DAEMON=off BENCH_DIRECT_QUIT=0 \
           BENCH_PATHS="$paths" WORKER_PROCESSES="$workers" \
           CONNECTIONS="$connections" CLIENT_PROCESSES="$clients" \
           WEPOLL_EDGE="$wepoll_edge" \
           OHA_WARMUP_DURATION="$warmup_duration" \
           OHA_DURATION="$sample_duration" \
           BENCH_HTTP2="${BENCH_HTTP2:-0}" \
           BENCH_ACCEPT_MUTEX="${BENCH_ACCEPT_MUTEX:-}" \
           BENCH_HTTP_VERSION="${BENCH_HTTP_VERSION:-}" \
           BENCH_MEMORY_UPSTREAM_DELAY_MS="${BENCH_MEMORY_UPSTREAM_DELAY_MS:-}" \
           BENCH_MEMORY_UPSTREAM_BODY_BYTES="${BENCH_MEMORY_UPSTREAM_BODY_BYTES:-65536}" \
           BENCH_PROXY_BUFFERING="${BENCH_PROXY_BUFFERING:-off}" \
           "$script_dir/win32-oha-bench.sh" "$binary" "$backend" \
               "$run_label" >"$attempt_dir/driver.stdout" \
               2>"$attempt_dir/driver.stderr" \
           && "$filter_node" "$script_dir/validate-oha-results.js" \
               "$attempt_dir"
        then
            mv "$attempt_dir" "$final_dir"
            return 0
        fi

        mkdir -p "$results_root/failures"
        mv "$attempt_dir" \
            "$results_root/failures/${mode}-repeat${repeat}-${condition}-attempt${attempt}"
    done

    echo "all attempts failed: $mode repeat $repeat $condition" >&2
    return 1
}

compare_pair() {
    local candidate candidate_dir control control_dir mode output

    mode=$1
    candidate=$2
    control=$3
    candidate_dir="$results_root/$mode/$candidate-raw"
    control_dir="$results_root/$mode/$control-raw"
    output="$results_root/$mode/$candidate-vs-$control.jsonl"

    if [ ! -d "$candidate_dir" ] || [ ! -d "$control_dir" ]; then
        return 0
    fi

    "$filter_node" "$script_dir/noise-compare.js" \
        "$candidate_dir" "$control_dir" \
        "$label-$mode-$candidate" "$label-$mode-$control" "$output"
}

for repeat in $(seq 1 "$repeats"); do
    mode_order=$modes
    if [ $((repeat % 2)) -eq 0 ]; then
        mode_order=$(printf '%s\n' $modes | sed -n '2p;1p')
    fi

    for mode in $mode_order; do
        mode_offset=0
        if [ "$mode" = scaled ]; then
            mode_offset=3
        fi

        offset=$(((repeat - 1 + mode_offset) % ${#conditions[@]}))

        for position in $(seq 0 $((${#conditions[@]} - 1))); do
            index=$(((offset + position) % ${#conditions[@]}))
            run_one "$mode" "$repeat" "${conditions[$index]}"
        done
    done
done

for mode in $modes; do
    mkdir -p "$results_root/$mode"

    for condition in "${conditions[@]}"; do
        "$filter_node" "$script_dir/noise-filter.js" \
            "$results_root/$mode/$condition-raw" \
            "$label-$mode-$condition" \
            "$results_root/$mode/$condition-summary.jsonl"
    done

    compare_pair "$mode" iocp-w1 select-w1
    compare_pair "$mode" iocp-w1 poll-w1
    compare_pair "$mode" iocp-w4 select-w4
    compare_pair "$mode" iocp-w4 poll-w4
    compare_pair "$mode" iocp-w4 iocp-w1
    compare_pair "$mode" select-w4 select-w1
    compare_pair "$mode" poll-w4 poll-w1
    compare_pair "$mode" wepoll-level-w1 iocp-w1
    compare_pair "$mode" wepoll-level-w1 select-w1
    compare_pair "$mode" wepoll-level-w1 poll-w1
    compare_pair "$mode" wepoll-level-w4 iocp-w4
    compare_pair "$mode" wepoll-level-w4 select-w4
    compare_pair "$mode" wepoll-level-w4 poll-w4
    compare_pair "$mode" wepoll-level-w4 wepoll-level-w1
    compare_pair "$mode" wepoll-edge-w1 iocp-w1
    compare_pair "$mode" wepoll-edge-w1 select-w1
    compare_pair "$mode" wepoll-edge-w1 poll-w1
    compare_pair "$mode" wepoll-edge-w4 iocp-w4
    compare_pair "$mode" wepoll-edge-w4 select-w4
    compare_pair "$mode" wepoll-edge-w4 poll-w4
    compare_pair "$mode" wepoll-edge-w4 wepoll-edge-w1
    compare_pair "$mode" wepoll-edge-w1 wepoll-level-w1
    compare_pair "$mode" wepoll-edge-w4 wepoll-level-w4
done

echo "matrix results written to $results_root" >&2
