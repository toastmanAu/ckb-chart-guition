#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 -m unittest discover -s tests -v
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
  tests/test_metric_core.c firmware/main/metric_core.c -lm -o "$workdir/test_metric_core"
"$workdir/test_metric_core"
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
  tests/test_lc_badge.c firmware/main/lc_badge.c -o "$workdir/test_lc_badge"
"$workdir/test_lc_badge"
${CC:-cc} -std=c11 -Wall -Wextra -Werror \
  tests/test_screenshot_stream.c firmware/main/screenshot_stream.c -o "$workdir/test_screenshot_stream"
"$workdir/test_screenshot_stream"
