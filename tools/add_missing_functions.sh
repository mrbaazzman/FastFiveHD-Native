#!/usr/bin/env bash
# Add caller-supplied ELF addresses as functions in the existing Ghidra project and dump only
# those functions to analysis/reports/pcode_extra.tsv.
#
#   tools/add_missing_functions.sh 0x553b70 0x552e68 ...
set -euo pipefail
cd "$(dirname "$0")/.."

GHIDRA=/opt/tools/ghidra/support/analyzeHeadless
OUT=analysis/reports/pcode_extra.tsv

"$GHIDRA" "$PWD/analysis/ghidra-work" fast5 \
  -process -noanalysis \
  -scriptPath "$PWD/recomp/ghidra_scripts" \
  -postScript AddAndDumpFunctions.java "$PWD/$OUT" "$@"

echo "wrote $OUT ($(wc -c < "$OUT") bytes)"
