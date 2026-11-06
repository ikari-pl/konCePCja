#!/bin/bash
# clang-tidy gate for the files a change touches (beads-myd6).
#
# `make clang-tidy` runs every translation unit and can only be informational:
# the tree carries a few hundred findings, so it never passes and CI keeps it
# `continue-on-error`. That means a NEW finding never fails anything.
#
# This script runs clang-tidy on just the changed files and fails on a finding
# that is not already in the baseline. Two things make that honest:
#
#  * It parses real diagnostics (`file:line:col: warning: ... [check]`) and
#    never `grep .` the output: every file prints "N warnings generated" and
#    "Suppressed N warnings (N in non-user code)" even when it is clean, so the
#    old target's `grep "."` marks every file dirty by construction.
#
#  * It subtracts the baseline. A changed file inherits the findings of the
#    first-party headers it includes (via -header-filter), and 40-odd files
#    already have findings of their own; without the subtraction every PR
#    touching such a file would fail for reasons it did not cause. So the same
#    files are checked at BASE too and diagnostics present in both are dropped.
#
# A diagnostic is keyed by file, check name and the *text* of the source line,
# not by line number: adding a function at the top of a file shifts every
# warning below it, and a line-number key would call all of them new.
#
# Usage: tools/clang_tidy_changed.sh [--base <git-ref|sha>] [-- <compiler args>]
#   --base defaults to origin/master.  A sha is used as the base commit as is.
#
# Exit: 0 clean (or nothing to check), 1 new diagnostics, 2 setup failure.
# A setup failure (no merge base, no base worktree, clang-tidy refusing to
# parse its config, a compile error) exits non-zero on purpose: a gate that
# cannot run must not report success.
set -uo pipefail

BASE="origin/master"
while [ $# -gt 0 ]; do
  case "$1" in
    --base) BASE="${2:?--base needs a ref}"; shift 2 ;;
    --) shift; break ;;
    *) break ;;
  esac
done

if [ -z "${CLANG_TIDY:-}" ]; then
  CLANG_TIDY="$(command -v clang-tidy || true)"
fi
# A name that does not run is not a gate.  The makefile sets CLANG_TIDY, so a
# binary missing from PATH arrives here as a non-empty string, every invocation
# then fails with "command not found" inside the subshell and the run looks
# clean -- the exact silent pass this target exists to stop.
if [ -z "$CLANG_TIDY" ] || ! "$CLANG_TIDY" --version >/dev/null 2>&1; then
  echo "clang-tidy-changed: '$CLANG_TIDY' is not a working clang-tidy." >&2
  echo "Set CLANG_TIDY to one, or install it (pip install clang-tidy==22.1.8)." >&2
  exit 2
fi

# The commit to compare against.  A ref (the default origin/master, or a
# branch) means "where this change diverged from it", so take the merge base.
# A full commit sha is taken as given: CI has the exact base commit of the PR,
# and on a shallow checkout the two commits need not have a common ancestor
# locally at all, so asking for one there fails for the wrong reason.
if [[ "$BASE" =~ ^[0-9a-f]{40}$ ]]; then
  MERGE_BASE="$BASE"
else
  MERGE_BASE="$(git merge-base "$BASE" HEAD 2>/dev/null)"
  if [ -z "$MERGE_BASE" ]; then
    echo "clang-tidy-changed: cannot find the merge base with '$BASE'." >&2
    echo "Fetch enough history first (git fetch origin $BASE, or fetch-depth: 0)," >&2
    echo "or pass --base <commit sha>." >&2
    exit 2
  fi
fi
if ! git cat-file -e "$MERGE_BASE^{commit}" 2>/dev/null; then
  echo "clang-tidy-changed: '$MERGE_BASE' is not a commit in this clone." >&2
  exit 2
fi

# The .cpp files this change touches.  Compared against the working tree, not
# HEAD, so a local run sees uncommitted edits too; in CI the PR is checked out
# as a commit and the two are the same.
#
# Headers are covered through -header-filter on whichever translation unit
# includes them.  A changed header no .cpp includes has no translation unit to
# check it in: running clang-tidy on a bare header means guessing a language
# and an include set, which is how a "clean" header run silently checks nothing.
CHANGED_CPP=()
while IFS= read -r f; do
  [ -n "$f" ] && CHANGED_CPP+=("$f")
done < <(git diff --name-only --diff-filter=d "$MERGE_BASE" -- 'src/*.cpp' 'src/**/*.cpp' \
           | grep -v -E '^src/(argparse|compat/|m4board_web_assets)' || true)

if [ ${#CHANGED_CPP[@]} -eq 0 ]; then
  echo "clang-tidy-changed: no changed .cpp files against $BASE ($MERGE_BASE); nothing to check."
  exit 0
fi

echo "clang-tidy-changed: ${#CHANGED_CPP[@]} changed translation unit(s) against $BASE ($MERGE_BASE)"

# Same invocation `make clang-tidy` uses, so the two runs agree:
# -header-filter here, and everything after `--` on our command line is the
# compiler command line (the makefile passes $(COMMON_CFLAGS)).
TIDY_ARGS=(-header-filter='src/.*')
COMPILE_ARGS=()
if [ $# -gt 0 ]; then
  COMPILE_ARGS=("$@")
fi

WORK="$(mktemp -d)"
BASE_WT=""
# shellcheck disable=SC2329  # run by the trap below, not called directly
cleanup() {
  if [ -n "$BASE_WT" ]; then
    git worktree remove --force "$BASE_WT" >/dev/null 2>&1 || rm -rf "$BASE_WT"
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT

# One diagnostic per line: "path:line:col: severity: message [check]".
# clang-tidy also emits notes ("note: expanded from macro") and, on a broken
# setup, "error: unable to find ..." -- errors are kept and fail the run.
collect() { # <dir> <out>
  local dir="$1" out="$2"
  : > "$out"
  # The `--` is load-bearing: without it clang-tidy reads the compiler flags as
  # its own options, ignores them and reports no diagnostics at all, which looks
  # exactly like a clean run.  "Error while processing" below is what catches a
  # run that did not happen.
  ( cd "$dir" && "$CLANG_TIDY" "${CHANGED_CPP[@]}" "${TIDY_ARGS[@]}" -- "${COMPILE_ARGS[@]}" 2>&1 ) \
    | tee "$out.raw" \
    | grep -E '^[^:]+:[0-9]+:[0-9]+: (warning|error): ' > "$out" || true
  # clang-tidy prints this when it could not process a file.  It is the one
  # signal that separates "clean" from "never ran": a run with no diagnostics
  # and no such error is a clean run, and anything else must not read as
  # success (beads-myd6: dropping the `--` once made every run look clean).
  if grep -q 'Error while processing' "$out.raw" 2>/dev/null; then
    echo "clang-tidy-changed: clang-tidy could not process a file in $dir:" >&2
    grep -E 'Error while processing|error: ' "$out.raw" | head -10 >&2
    return 3
  fi
  return 0
}

# Key a diagnostic by file, check name and the text of the source line it
# points at, so a shift in line numbers does not turn a baseline finding into a
# "new" one.  A finding whose line text also changed is treated as new, which
# is the right answer: it is a different statement.
key_diags() { # <dir> <in> <out>
  local dir="$1" in="$2" out="$3"
  : > "$out"
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    local path rest file lno check text
    path="${line%%:*}"
    rest="${line#*:}"
    lno="${rest%%:*}"
    check="$(printf '%s' "$line" | sed -nE 's/.*\[([a-z0-9.,-]+)\]$/\1/p')"
    file="${path#"$dir"/}"
    # The source line the diagnostic points at, with whitespace collapsed.
    text="$(sed -n "${lno}p" "$dir/$file" 2>/dev/null | tr -s '[:space:]' ' ' | sed -E 's/^ //; s/ $//')"
    printf '%s\t%s\t%s\n' "$file" "$check" "$text" >> "$out"
  done < "$in"
}

echo "clang-tidy-changed: checking the working tree"
if ! collect "$PWD" "$WORK/head.txt"; then
  echo "clang-tidy-changed: the check did not run (see above)" >&2
  exit 2
fi
if grep -qE ': error: ' "$WORK/head.txt"; then
  echo "clang-tidy-changed: clang-tidy reported errors (setup or compile failure):" >&2
  grep -E ': error: ' "$WORK/head.txt" >&2
  exit 2
fi

# Baseline: the same files as they are at the merge base, in a throwaway
# worktree so the checkout under test is never disturbed.
BASE_WT="$(mktemp -d)/base"
git worktree add --detach --force "$BASE_WT" "$MERGE_BASE" >/dev/null 2>&1 || {
  echo "clang-tidy-changed: cannot create a worktree at $MERGE_BASE" >&2
  exit 2
}
# A new worktree has its submodules empty, and vendor/SDL is one: every unit
# that includes SDL then fails to parse at BASE, and the gate stopped on the
# first PR that touched such a file (#164). Third-party headers are not what
# this compares, so the baseline borrows the checkout's own.
for sub in $(git submodule --quiet foreach --recursive 'echo $sm_path' 2>/dev/null); do
  if [ -d "$sub" ] && [ -z "$(ls -A "$BASE_WT/$sub" 2>/dev/null)" ]; then
    rmdir "$BASE_WT/$sub" 2>/dev/null || rm -rf "${BASE_WT:?}/$sub"
    ln -s "$PWD/$sub" "$BASE_WT/$sub"
  fi
done

PRESENT=()
for f in "${CHANGED_CPP[@]}"; do
  [ -f "$BASE_WT/$f" ] && PRESENT+=("$f")
done

if [ ${#PRESENT[@]} -eq 0 ]; then
  echo "clang-tidy-changed: none of the changed files exist at $BASE (all new); no baseline."
  key_diags "$PWD" "$WORK/head.txt" "$WORK/head.keys"
  : > "$WORK/base.keys"
else
  echo "clang-tidy-changed: checking ${#PRESENT[@]} of them at $BASE for the baseline"
  CHANGED_CPP=("${PRESENT[@]}")
  # A baseline that cannot run is not "no findings": say so rather than
  # silently treating every existing warning as new.
  if ! collect "$BASE_WT" "$WORK/base.txt"; then
    echo "clang-tidy-changed: the baseline run did not happen (see above)" >&2
    exit 2
  fi
  if grep -qE ': error: ' "$WORK/base.txt"; then
    echo "clang-tidy-changed: the baseline run failed; refusing to guess:" >&2
    grep -E ': error: ' "$WORK/base.txt" >&2
    exit 2
  fi
  key_diags "$BASE_WT" "$WORK/base.txt" "$WORK/base.keys"
  key_diags "$PWD" "$WORK/head.txt" "$WORK/head.keys"
fi

NEW="$(sort -u "$WORK/head.keys" | comm -23 - <(sort -u "$WORK/base.keys"))"

if [ -n "$NEW" ]; then
  count="$(printf '%s\n' "$NEW" | wc -l | tr -d ' ')"
  echo "" >&2
  echo "clang-tidy-changed: $count new clang-tidy finding(s) in the files this change touches:" >&2
  while IFS=$'\t' read -r file check text; do
    # Point at the actual line for the human reading the log.
    loc="$(grep -nF -- "$text" "$file" 2>/dev/null | head -1 | cut -d: -f1)"
    printf '  %s%s [%s]\n    %s\n' "$file" "${loc:+:$loc}" "$check" "$text" >&2
  done <<< "$NEW"
  echo "" >&2
  echo "These are new against $BASE. Fix them, or (if the check is wrong here)" >&2
  echo "add a // NOLINT(check) at the end of that line." >&2
  exit 1
fi

echo "clang-tidy-changed: no new findings in ${#CHANGED_CPP[@]} changed translation unit(s)."
exit 0
