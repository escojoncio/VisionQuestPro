#!/usr/bin/env bash
# Early, clear errors: every RUNTIME_SHIP source of Klepton, compiled with -fsyntax-only against the
# visionOS SDK (same flags as `make build/xros/libklepton.a`), in parallel. Every file is checked
# before failing, so one run lists all the errors. .S files are assembly: nothing to syntax-check.
# Usage (repo root): tools/ci/syntax-xros.sh      Needs vendor-moltenvk/out (MoltenVK headers).
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT/klepton"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
VARS=$(printf 'ci-vars:\n\t@echo "$(RUNTIME_SHIP)"\n' | make -s -f Makefile -f - ci-vars XROS_PLATFORMS=xros) || { echo "FAILED: cannot read RUNTIME_SHIP from klepton/Makefile"; exit 1; }
CFL=$(printf 'ci-vars:\n\t@echo "$(XROS_CFLAGS)"\n' | make -s -f Makefile -f - ci-vars XROS_PLATFORMS=xros)
SDK=$(xcrun --sdk xros --show-sdk-path)
SRCS=$(printf '%s\n' $VARS | grep -E '\.(c|m)$' || true)
[ -n "$SRCS" ] || { echo "FAILED: RUNTIME_SHIP has no sources"; exit 1; }
echo "syntax-xros: $(echo "$SRCS" | wc -l | tr -d ' ') files, -j$JOBS, sdk $SDK"
OUT=$(mktemp -d)
export CFL SDK OUT
# shellcheck disable=SC2016
printf '%s\n' $SRCS | xargs -P "$JOBS" -I{} bash -c \
  'f="{}"; o="$OUT/$(echo "$f" | tr / _).log"; clang $CFL -target arm64-apple-xros1.0 -isysroot "$SDK" -fsyntax-only "$f" >"$o" 2>&1 || { echo "FAILED: $f"; cat "$o"; touch "$OUT/failed"; }'
if [ -e "$OUT/failed" ]; then echo "syntax-xros: FAILED (see errors above)"; exit 1; fi
echo "syntax-xros: OK"
