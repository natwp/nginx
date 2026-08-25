#!/usr/bin/env bash

set -euo pipefail

bench_repo=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
bench_scratch="$bench_repo/.git/benchmark-work"
mkdir -p "$bench_scratch"

if [ "$#" -lt 1 ]; then
    echo "usage: $0 script.js [args ...]" >&2
    exit 2
fi

node_win=${NODE_WIN_BIN:-$(command -v node.exe || true)}
msys_bash=${MSYS_BASH:-/usr/bin/bash}
script=$1
shift

if [ ! -x "$node_win" ]; then
    echo "Windows Node.js binary is not executable: $node_win" >&2
    exit 2
fi

if [ ! -x "$msys_bash" ]; then
    echo "MSYS2 bash is not executable: $msys_bash" >&2
    exit 2
fi

output=${WIN32_NODE_LOG:-}
if [ -z "$output" ]; then
    output=$(mktemp "$bench_scratch/win32-node.XXXXXX.log")
fi

case "$output" in
    "$bench_scratch"/*) ;;
    *) echo "WIN32_NODE_LOG must be below $bench_scratch" >&2; exit 2 ;;
esac

mkdir -p "$(dirname "$output")"
: >"$output"

export WSLENV="${WSLENV:+$WSLENV:}BENCH_TLS:READ_PAUSE_MS"

node_mixed=$(wslpath -m "$node_win")
script_mixed=$(wslpath -m "$script")
output_mixed=$(wslpath -m "$output")
printf -v command '%q ' "$node_mixed" "$script_mixed" "$@"
printf -v output_quoted '%q' "$output_mixed"
command+=" >$output_quoted 2>&1"

if ! "$msys_bash" -l -c \
    "export MSYSTEM=MINGW64; export MSYS_NO_PATHCONV=1; source /etc/profile >/dev/null 2>&1; $command"
then
    sed -n '1,240p' "$output" >&2
    exit 1
fi
