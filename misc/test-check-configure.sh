#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

fake="$tmp/configure"
printf '%s\n' '#!/usr/bin/env bash' \
    'echo "configuration stopped unexpectedly"' \
    'exit 17' > "$fake"
chmod +x "$fake"

set +e
"$script_dir/check-configure.sh" "$tmp/configure.log" "$fake"
status=$?
set -e

if [ "$status" -ne 17 ]; then
    echo "configure status was not preserved: $status" >&2
    exit 1
fi

if ! grep -Fq "configuration stopped unexpectedly" "$tmp/configure.log"; then
    echo "configure output was not captured" >&2
    exit 1
fi

echo "configure status test passed"
