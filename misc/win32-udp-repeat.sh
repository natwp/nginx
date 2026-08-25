#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 nginx.exe [iocp|select|poll] [label] [udp-receives]" >&2
    exit 2
fi

binary=$(realpath "$1")
backend=${2:-iocp}
label=${3:-$(basename "$(dirname "$binary")")-$backend-udp}
udp_receives=${4:-64}
script_dir=$(cd "$(dirname "$0")" && pwd)
results_dir=${RESULTS_DIR:-$PWD/bench-results}
repeats=${REPEATS:-5}
max_attempts=${MAX_ATTEMPTS:-3}
resume=${RESUME:-0}
control_binary=${CONTROL_BINARY:-}
control_backend=${CONTROL_BACKEND:-$backend}
control_udp_receives=${CONTROL_UDP_RECEIVES:-$udp_receives}
control_label=${CONTROL_LABEL:-$label-control}

case "$repeats" in
    ''|*[!0-9]*) echo "REPEATS must be an integer" >&2; exit 2 ;;
esac
if [ "$repeats" -lt 3 ]; then
    echo "REPEATS must be at least 3" >&2
    exit 2
fi

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
        echo "CONTROL_LABEL must differ from candidate label" >&2
        exit 2
    fi
fi

raw_dir="$results_dir/$label-raw"
summary="$results_dir/$label-summary.jsonl"
control_raw_dir="$results_dir/$control_label-raw"
control_summary="$results_dir/$control_label-summary.jsonl"
comparison_summary="$results_dir/$label-vs-$control_label.jsonl"

prepare_results() {
    local dir=$1
    local output=$2

    if [ "$resume" != 1 ] && [ -d "$dir" ] \
        && find "$dir" -mindepth 1 -print -quit | grep -q .; then
        echo "refusing to mix new samples with existing results: $dir" >&2
        exit 2
    fi

    mkdir -p "$dir"
    rm -f -- "$output"
}

run_one() {
    local run_binary=$1
    local run_backend=$2
    local run_receives=$3
    local group=$4
    local repeat=$5
    local run_label="repeat${repeat}-${group}"
    local final_dir="$results_dir/$group-raw/$run_label"
    local failure_dir="$results_dir/$group-failures"
    local attempt attempt_dir settle

    if [ "$resume" = 1 ] && [ -d "$final_dir" ]; then
        echo "keeping completed UDP run: $run_label" >&2
        return 0
    fi

    for attempt in $(seq 1 "$max_attempts"); do
        attempt_dir=$(mktemp -d "$results_dir/.${run_label}-attempt${attempt}.XXXXXX")
        echo "starting UDP run $repeat/$repeats: $run_label (attempt $attempt/$max_attempts)" >&2

        if RESULTS_DIR="$attempt_dir" \
            "$script_dir/win32-udp-bench.sh" "$run_binary" "$run_backend" \
            "$run_label" "$run_receives" \
            >"$attempt_dir/driver.stdout" \
            2> >(tee "$attempt_dir/driver.stderr" >&2); then
            for settle in $(seq 1 5); do
                sleep 1
                if mv "$attempt_dir" "$final_dir" 2>/dev/null; then
                    return 0
                fi
            done

            echo "could not move completed UDP result after waiting for Windows file handles: $run_label" >&2
        fi

        mkdir -p "$failure_dir"
        mv "$attempt_dir" "$failure_dir/"
        echo "discarded failed UDP attempt $attempt/$max_attempts for $run_label" >&2
    done

    echo "all UDP attempts failed for $run_label" >&2
    return 1
}

prepare_results "$raw_dir" "$summary"
if [ -n "$control_binary" ]; then
    prepare_results "$control_raw_dir" "$control_summary"
    rm -f -- "$comparison_summary"
fi

for repeat in $(seq 1 "$repeats"); do
    if [ -n "$control_binary" ] && [ $((repeat % 2)) -eq 1 ]; then
        run_one "$control_binary" "$control_backend" "$control_udp_receives" \
            "$control_label" "$repeat"
    fi

    run_one "$binary" "$backend" "$udp_receives" "$label" "$repeat"

    if [ -n "$control_binary" ] && [ $((repeat % 2)) -eq 0 ]; then
        run_one "$control_binary" "$control_backend" "$control_udp_receives" \
            "$control_label" "$repeat"
    fi
done

filter_node_bin=${FILTER_NODE_BIN:-$(command -v node || true)}
if [ ! -x "$filter_node_bin" ]; then
    echo "filter Node.js binary is not executable: $filter_node_bin" >&2
    exit 2
fi

"$filter_node_bin" "$script_dir/noise-filter.js" "$raw_dir" \
    "$label" "$summary"

if [ -n "$control_binary" ]; then
    "$filter_node_bin" "$script_dir/noise-filter.js" "$control_raw_dir" \
        "$control_label" "$control_summary"
    "$filter_node_bin" "$script_dir/noise-compare.js" "$raw_dir" \
        "$control_raw_dir" "$label" "$control_label" "$comparison_summary"
fi
