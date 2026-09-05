#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 5 ]; then
    echo "usage: $0 candidate.exe control.exe [backend] [candidate-label] [control-label]" >&2
    exit 2
fi

candidate=$(realpath "$1")
control=$(realpath "$2")
backend=${3:-iocp}
candidate_label=${4:-candidate}
control_label=${5:-control}
script_dir=$(cd "$(dirname "$0")" && pwd)
results_root=${RESULTS_DIR:-./nginx-oha-compare}
repeats=${REPEATS:-6}
max_attempts=${MAX_ATTEMPTS:-2}
workers=${WORKER_PROCESSES:-1}
connections=${CONNECTIONS:-48}
clients=${CLIENT_PROCESSES:-4}
paths=${BENCH_PATHS:-/empty.gif /64k.bin}
warmup_duration=${OHA_WARMUP_DURATION:-2s}
sample_duration=${OHA_DURATION:-7s}
filter_node=${FILTER_NODE_BIN:-$(command -v node || true)}
resume=${RESUME:-0}

case "$results_root" in
    /*) ;;
    *) echo "RESULTS_DIR must be an absolute path" >&2; exit 2 ;;
esac

case "$backend" in
    iocp|select|poll) ;;
    *) echo "backend must be iocp, select, or poll" >&2; exit 2 ;;
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

if [ ! -x "$candidate" ] || [ ! -x "$control" ] \
   || [ ! -x "$filter_node" ]
then
    echo "candidate, control, or filter Node.js is not executable" >&2
    exit 2
fi

if [ "$candidate_label" = "$control_label" ]; then
    echo "candidate and control labels must differ" >&2
    exit 2
fi

if [ "$resume" = 0 ] && [ -d "$results_root" ] \
   && [ -n "$(rg --files "$results_root" 2>/dev/null | sed -n '1p')" ]
then
    echo "refusing to mix samples with existing results: $results_root" >&2
    exit 2
fi

mkdir -p "$results_root/$candidate_label-raw" \
         "$results_root/$control_label-raw"

run_one() {
    local attempt attempt_dir binary final_dir group repeat

    binary=$1
    group=$2
    repeat=$3
    final_dir="$results_root/$group-raw/repeat$repeat-$group"

    if [ "$resume" = 1 ] && [ -d "$final_dir" ] \
       && [ -n "$(rg --files "$final_dir" 2>/dev/null | sed -n '1p')" ]
    then
        echo "keeping completed comparison run: repeat $repeat $group" >&2
        return 0
    fi

    for attempt in $(seq 1 "$max_attempts"); do
        attempt_dir=$(mktemp -d \
            "$results_root/.repeat${repeat}-${group}.XXXXXX")

        echo "comparison round $repeat/$repeats: $group " \
             "(attempt $attempt/$max_attempts)" >&2

        if RESULTS_DIR="$attempt_dir" BENCH_SCRATCH_ROOT="${TMPDIR:-/tmp}" \
           MASTER_PROCESS=on DAEMON=off BENCH_DIRECT_QUIT=0 \
           BENCH_PATHS="$paths" WORKER_PROCESSES="$workers" \
           CONNECTIONS="$connections" CLIENT_PROCESSES="$clients" \
           OHA_WARMUP_DURATION="$warmup_duration" \
           OHA_DURATION="$sample_duration" \
           "$script_dir/win32-oha-bench.sh" "$binary" "$backend" \
               "$group" >"$attempt_dir/driver.stdout" \
               2>"$attempt_dir/driver.stderr" \
           && "$filter_node" "$script_dir/validate-oha-results.js" \
               "$attempt_dir"
        then
            mv "$attempt_dir" "$final_dir"
            return 0
        fi

        mkdir -p "$results_root/failures"
        mv "$attempt_dir" \
            "$results_root/failures/repeat${repeat}-${group}-attempt${attempt}"
    done

    echo "all attempts failed: repeat $repeat $group" >&2
    return 1
}

for repeat in $(seq 1 "$repeats"); do
    if [ $((repeat % 2)) -eq 1 ]; then
        run_one "$control" "$control_label" "$repeat"
        run_one "$candidate" "$candidate_label" "$repeat"
    else
        run_one "$candidate" "$candidate_label" "$repeat"
        run_one "$control" "$control_label" "$repeat"
    fi
done

"$filter_node" "$script_dir/noise-filter.js" \
    "$results_root/$candidate_label-raw" "$candidate_label" \
    "$results_root/$candidate_label-summary.jsonl"
"$filter_node" "$script_dir/noise-filter.js" \
    "$results_root/$control_label-raw" "$control_label" \
    "$results_root/$control_label-summary.jsonl"
"$filter_node" "$script_dir/noise-compare.js" \
    "$results_root/$candidate_label-raw" \
    "$results_root/$control_label-raw" \
    "$candidate_label" "$control_label" \
    "$results_root/$candidate_label-vs-$control_label.jsonl"

echo "comparison results written to $results_root" >&2
