#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 nginx.exe [iocp|select|poll] [on|off] [label]" >&2
    exit 2
fi

binary=$(realpath "$1")
backend=${2:-iocp}
sendfile=${3:-off}
label=${4:-$(basename "$(dirname "$binary")")-$backend-sendfile-$sendfile}
script_dir=$(cd "$(dirname "$0")" && pwd)
results_dir=${RESULTS_DIR:-$PWD/bench-results}
repeats=${REPEATS:-5}
max_attempts=${MAX_ATTEMPTS:-3}
resume=${RESUME:-0}
control_binary=${CONTROL_BINARY:-}
control_backend=${CONTROL_BACKEND:-$backend}
control_sendfile=${CONTROL_SENDFILE:-$sendfile}
control_label=${CONTROL_LABEL:-$label-control}
control_worker_processes=${CONTROL_WORKER_PROCESSES:-${WORKER_PROCESSES:-1}}

case "$repeats" in
    ''|*[!0-9]*)
        echo "REPEATS must be a positive integer" >&2
        exit 2
        ;;
esac

if [ "$repeats" -lt 3 ]; then
    echo "REPEATS must be at least 3 for a noise filter" >&2
    exit 2
fi

case "$max_attempts" in
    ''|*[!0-9]*)
        echo "MAX_ATTEMPTS must be a positive integer" >&2
        exit 2
        ;;
esac

if [ "$max_attempts" -lt 1 ]; then
    echo "MAX_ATTEMPTS must be at least 1" >&2
    exit 2
fi

case "$resume" in
    0|1) ;;
    *)
        echo "RESUME must be 0 or 1" >&2
        exit 2
        ;;
esac

if [ ! -x "$binary" ]; then
    echo "nginx binary is not executable: $binary" >&2
    exit 2
fi

if [ -n "$control_binary" ]; then
    control_binary=$(realpath "$control_binary")

    if [ ! -x "$control_binary" ]; then
        echo "control nginx binary is not executable: $control_binary" >&2
        exit 2
    fi

    if [ "$control_label" = "$label" ]; then
        echo "CONTROL_LABEL must differ from the candidate label" >&2
        exit 2
    fi
fi

raw_dir="$results_dir/$label-raw"
summary="$results_dir/$label-summary.jsonl"
control_raw_dir="$results_dir/$control_label-raw"
control_summary="$results_dir/$control_label-summary.jsonl"
comparison_summary="$results_dir/$label-vs-$control_label.jsonl"

prepare_results() {
    local dir output

    dir=$1
    output=$2

    if [ "$resume" != 1 ] && [ -d "$dir" ] \
        && find "$dir" -mindepth 1 -print -quit | grep -q .
    then
        echo "refusing to mix new samples with existing results: $dir" >&2
        exit 2
    fi

    mkdir -p "$dir"
    rm -f -- "$output"
}

run_one() {
    local attempt attempt_dir failure_dir final_dir group repeat run_backend
    local run_binary run_label run_sendfile run_workers settle

    run_binary=$1
    run_backend=$2
    run_sendfile=$3
    group=$4
    repeat=$5
    run_workers=$6
    run_label="repeat${repeat}-${group}"
    final_dir="$results_dir/$group-raw/$run_label"
    failure_dir="$results_dir/$group-failures"

    if [ "$resume" = 1 ] && [ -d "$final_dir" ]; then
        echo "keeping completed noise-aware run: $run_label" >&2
        return 0
    fi

    for attempt in $(seq 1 "$max_attempts"); do
        attempt_dir=$(mktemp -d \
            "$results_dir/.${run_label}-attempt${attempt}.XXXXXX")

        echo "starting noise-aware run $repeat/$repeats: $run_label " \
             "(attempt $attempt/$max_attempts)" >&2

        if RESULTS_DIR="$attempt_dir" WORKER_PROCESSES="$run_workers" \
            "$script_dir/win32-iocp-bench.sh" "$run_binary" \
            "$run_backend" "$run_sendfile" "$run_label" \
            >"$attempt_dir/driver.stdout" \
            2> >(tee "$attempt_dir/driver.stderr" >&2)
        then
            for settle in $(seq 1 5); do
                sleep 1
                if mv "$attempt_dir" "$final_dir" 2>/dev/null; then
                    return 0
                fi
            done

            echo "could not move completed result after waiting for Windows file handles: $run_label" >&2
        fi

        mkdir -p "$failure_dir"
        mv "$attempt_dir" "$failure_dir/"
        echo "discarded failed attempt $attempt/$max_attempts for " \
             "$run_label" >&2
    done

    echo "all attempts failed for $run_label" >&2
    return 1
}

prepare_results "$raw_dir" "$summary"

if [ -n "$control_binary" ]; then
    prepare_results "$control_raw_dir" "$control_summary"
    rm -f -- "$comparison_summary"
fi

for repeat in $(seq 1 "$repeats"); do
    if [ -n "$control_binary" ] && [ $((repeat % 2)) -eq 1 ]; then
        run_one "$control_binary" "$control_backend" "$control_sendfile" \
            "$control_label" "$repeat" "$control_worker_processes"
    fi

    run_one "$binary" "$backend" "$sendfile" "$label" "$repeat" \
        "${WORKER_PROCESSES:-1}"

    if [ -n "$control_binary" ] && [ $((repeat % 2)) -eq 0 ]; then
        run_one "$control_binary" "$control_backend" "$control_sendfile" \
            "$control_label" "$repeat" "$control_worker_processes"
    fi
done

filter_node_bin=${FILTER_NODE_BIN:-$(command -v node || true)}
if [ ! -x "$filter_node_bin" ]; then
    echo "filter Node.js binary is not executable: $filter_node_bin" >&2
    exit 2
fi

"$filter_node_bin" "$script_dir/noise-filter.js" "$raw_dir" \
    "$label" "$summary"

echo "noise-aware summary written to $summary" >&2

if [ -n "$control_binary" ]; then
    "$filter_node_bin" "$script_dir/noise-filter.js" "$control_raw_dir" \
        "$control_label" "$control_summary"

    echo "noise-aware control summary written to $control_summary" >&2

    "$filter_node_bin" "$script_dir/noise-compare.js" "$raw_dir" \
        "$control_raw_dir" "$label" "$control_label" "$comparison_summary"

    echo "paired noise-aware comparison written to $comparison_summary" >&2
fi
