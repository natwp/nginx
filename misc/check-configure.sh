#!/usr/bin/env bash

set -o pipefail

if [ "$#" -lt 2 ]; then
    echo "usage: $0 configure.log command [arguments ...]" >&2
    exit 2
fi

log=$1
shift

set +e
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
set -e

exit "$status"
