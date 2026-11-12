#!/usr/bin/env bash
# Dump Ghidra P-code for libFast5.so.
#
#   tools/dump_pcode.sh [maxFunctionSize]
#
# Runs a private headless import in analysis/ghidra-work and writes
# analysis/reports/pcode.tsv. The project is deleted afterwards.
set -euo pipefail
cd "$(dirname "$0")/.."

GHIDRA=/opt/tools/ghidra/support/analyzeHeadless
SO="analysis/apk/lib/armeabi-v7a/libFast5.so"
PROJ="analysis/ghidra-work"
OUT="analysis/reports/pcode.tsv"
MAX="${1:-128}"
MODE="${2:-import}"

if [ "$MODE" = "reuse" ]; then
  mkdir -p analysis/reports
  "$GHIDRA" "$PWD/$PROJ" fast5 \
    -process -noanalysis \
    -scriptPath "$PWD/recomp/ghidra_scripts" \
    -postScript DumpPcode.java "$PWD/$OUT" "$MAX"
  echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
  exit 0
fi

rm -rf "$PROJ"
mkdir -p "$PROJ" analysis/reports

"$GHIDRA" "$PWD/$PROJ" fast5 \
  -import "$PWD/$SO" \
  -scriptPath "$PWD/recomp/ghidra_scripts" \
  -postScript DumpPcode.java "$PWD/$OUT" "$MAX"

echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
