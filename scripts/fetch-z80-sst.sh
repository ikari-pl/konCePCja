#!/usr/bin/env bash
# fetch-z80-sst.sh — populate test/hw/sst/ with SingleStepTests (jsmoo) Z80
# per-opcode JSON for the z80_singlestep_test.cpp oracle (beads-yjql).
#
# The repo commits a trimmed slice (block-op + SCF/CCF families, 100 cases each,
# cycles stripped) so CI runs a permanent oracle without a heavy vendored blob.
# Run this to (re)generate that slice, widen the case count, or add opcodes.
#
#   scripts/fetch-z80-sst.sh [CASES] [OPCODE ...]
#
#   CASES    cases per opcode to keep (default 100; the full files hold 1000)
#   OPCODE   space-in-name opcode file stems, e.g. "ed b2" "37" (default: the
#            committed block + SCF/CCF set). "all" fetches the whole page 00..ff
#            plus the cb/dd/ed/fd/ddcb/fdcb pages — large; needs plenty of disk.
#
# Requires: curl, jq. Source: github.com/SingleStepTests/z80 (MIT).
set -euo pipefail

BASE="https://raw.githubusercontent.com/SingleStepTests/z80/main/v1"
DIR="$(cd "$(dirname "$0")/.." && pwd)/test/hw/sst"
CASES="${1:-100}"
shift || true

DEFAULT_OPS=(
  "ed a0" "ed a8" "ed b0" "ed b8"   # LDI LDD LDIR LDDR
  "ed a1" "ed a9" "ed b1" "ed b9"   # CPI CPD CPIR CPDR
  "ed a2" "ed aa" "ed b2" "ed ba"   # INI IND INIR INDR
  "ed a3" "ed ab" "ed b3" "ed bb"   # OUTI OUTD OTIR OTDR
  "37" "3f"                          # SCF CCF (incoming-Q sweep)
)

if [ "$#" -gt 0 ]; then
  OPS=("$@")
else
  OPS=("${DEFAULT_OPS[@]}")
fi

mkdir -p "$DIR"
command -v jq >/dev/null || { echo "jq is required" >&2; exit 1; }

ok=0
for op in "${OPS[@]}"; do
  enc="${op// /%20}"
  out="$DIR/${op// /_}.json"
  # Keep only the fields the harness compares; drop the per-T-state cycle trace.
  if curl -fsS -m 60 "$BASE/$enc.json" |
       jq -c "[.[0:$CASES][] | {name, initial, final, ports}]" >"$out" 2>/dev/null &&
     [ -s "$out" ]; then
    ok=$((ok + 1))
  else
    echo "FAILED: $op" >&2
    rm -f "$out"
  fi
done
echo "wrote $ok/${#OPS[@]} opcode files to $DIR ($CASES cases each)"
