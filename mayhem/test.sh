#!/usr/bin/env bash
#
# mayhem/test.sh — RUN this repo's OWN functional test suite (already built by mayhem/build.sh).
# exit 0 = pass. EDIT per repo. PATCH-grade oracle: after an agent patches the source, the grader
# rebuilds (build.sh) then runs this. DELETE this file if the repo has no meaningful tests.
#
# IMPORTANT:
#  * Must assert BEHAVIOR/OUTPUT, not just exit status. The oracle has to check asserted values /
#    golden-output diffs / known-answer results — so a PATCH that "fixes" a bug by making the program
#    exit(0) (or any no-op) FAILS here. Running inputs and checking only "exit 0 / didn't crash" is
#    NOT a functional test (it's trivially reward-hackable) — use the project's real assertion suite.
#  * Do NOT build here — mayhem/build.sh already compiled the test suite (with the project's normal
#    flags). This script only RUNS the pre-built tests and reports counts. If the test runner is
#    missing, that's a build.sh bug — fail loudly rather than silently rebuilding.
#  * REQUIRED OUTPUT — a CTRF (https://ctrf.io) summary so Mayhem/the PATCH grader reads the counts:
#      - writes a CTRF JSON report to ${CTRF_REPORT:-$SRC/ctrf-report.json}, and
#      - prints a one-line `CTRF {...}` marker to stdout (same JSON, compact).
#    Only `results.summary` (with tests/passed/failed/pending/skipped/other) is required.
#    Use the emit_ctrf helper below; it computes tests = passed+failed+skipped and sets the exit
#    code (0 iff failed==0). Map your framework's output to passed/failed/skipped.
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
: "${MAYHEM_JOBS:=$(nproc)}"   # build parallelism; env-overridable, falls back to nproc (use -j"$MAYHEM_JOBS")
cd "$SRC"

# emit_ctrf <tool> <passed> <failed> [skipped] [pending] [other]
# Writes a CTRF report (file + stdout `CTRF {...}` marker) and returns non-zero iff failed>0.
emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

# clifm has no upstream test suite. This is a known-answer suite over its public CLI, run on the
# oracle build (./clifm, upstream Makefile, normal flags): `clifm --mime-type FILE...` must name the
# MIME type of each crafted file, via the built-in fast_magic detector (src/fast_magic.c) and libmagic.
BIN="$SRC/clifm"
[ -x "$BIN" ] || { echo "missing $BIN — mayhem/build.sh must build it" >&2; emit_ctrf clifm-kat 0 1; exit 1; }
W=$(mktemp -d "${TMPDIR:-/tmp}/clifm-test.XXXXXX"); trap 'rm -rf "$W"' EXIT
printf '\x89PNG\r\n\x1a\n\0\0\0\rIHDR\0\0\0\1\0\0\0\1\x08\x02\0\0\0' > "$W/a"
printf 'hello world\n' > "$W/b"
printf '%%PDF-1.4\n%%\n' > "$W/c"
printf 'GIF89a\1\0\1\0\0\0\0;' > "$W/d"
printf '\x7fELF\x02\x01\x01\0\0\0\0\0\0\0\0\0\x02\0\x3e\0' > "$W/f"
: > "$W/g"
pass=0; fail=0
check() { # file expected-mime
  local got; got=$(cd "$W" && "$BIN" --mime-type "$1" 2>/dev/null)
  if [ "$got" = "$1: $2" ]; then pass=$((pass+1)); echo "PASS $1 -> $2"
  else fail=$((fail+1)); echo "FAIL $1: expected '$1: $2', got '$got'"; fi
}
check a image/png
check b text/plain
check c application/pdf
check d image/gif
check f application/x-executable
check g inode/x-empty
emit_ctrf clifm-kat "$pass" "$fail"
