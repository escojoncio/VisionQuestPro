#!/usr/bin/env bash
# Tests run on the macOS runner (not on visionOS) before the IPA may be published:
#  1) VPKEngine gpu/ on the host (null platform): cmake build + ctest.
#  2) VPKEngine gpu/metal2 for the visionOS SDK (compile + link only): catches .mm errors early.
#  3) Klepton host tests (klepton/runtime/xr/tests, klepton/tests/t_ptw.c) with ASan + UBSan.
# Usage (repo root): tools/ci/host-tests.sh <VPKEngine checkout> [<work dir>]
# Prints "FAILED: ..." lines and a summary; exits non-zero if anything failed.
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
VPK=$(cd "$1" && pwd)
WORK=${2:-$ROOT/build/host-tests}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)
echo "host-tests: -j$JOBS, work $WORK"

gpu_host() {
  cmake -S "$VPK/gpu" -B "$WORK/gpu" -DCMAKE_BUILD_TYPE=Release || return 1
  # -k: report every compile error at once, not only the first.
  cmake --build "$WORK/gpu" -j "$JOBS" -- -k || return 1
  (cd "$WORK/gpu" && ctest -j "$JOBS" --output-on-failure)
}
metal2_xros() {
  cmake -S "$VPK/gpu" -B "$WORK/metal2-xros" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_MACOSX_BUNDLE=OFF -DBUILD_TESTING=ON || return 1
  cmake --build "$WORK/metal2-xros" -j "$JOBS" --target vpgpu_metal2 vpgpu_metal2_tests
}
klepton_tests() {
  local rc=0 XR="$ROOT/klepton/runtime/xr" T="$WORK/klepton" ; mkdir -p "$T"
  local SAN="-g -O1 -Wall -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
  run() { # name, then compile args
    local n=$1; shift
    echo "--- $n"
    if clang $SAN "$@" -o "$T/$n" && perl -e 'alarm 240; exec @ARGV' "$T/$n"; then echo "ok: $n"; else echo "FAILED: klepton test $n"; rc=1; fi
  }
  cd "$XR" || return 1
  run pred_frame_offset_test tests/pred_frame_offset_test.c -lm
  run kl_ovrp_eyesize_test -I. tests/kl_ovrp_eyesize_test.c -lm
  run t_ovrp_input -I.. -I../.. tests/t_ovrp_input.c -lm
  run spatial_test -I.. -I../jni -I../libc -I../gfx -I. -I../media -I../guest -I../diag \
      tests/spatial_test.c kl_spatial.c kl_ovrp_spatial.c -lm -lpthread
  cd "$ROOT/klepton" || return 1
  run t_ptw tests/t_ptw.c runtime/gfx/kl_reproject_ptw.c -lm
  mkdir -p "$T/memdiag"
  awk '/KL-TEST-BEGIN (rate|range|fstat)/{p=1;next} /KL-TEST-END (rate|range|fstat)/{p=0} p' runtime/libc/kl_libc.c > "$T/memdiag/libc_blocks.inc"
  awk '/KL-TEST-BEGIN ent/{p=1;next} /KL-TEST-END ent/{p=0} p' visionos/Sources/kl_app.c > "$T/memdiag/app_blocks.inc"
  run t_memdiag -I"$T/memdiag" tests/t_memdiag.c -lpthread
  awk '/^static int klj_decode_args\(/,/^}/{print} /^static int klj_decode_args_a\(/,/^}/{print}' runtime/kl_jni.c > "$T/memdiag/jni_decoders.inc"
  run t_jni_sig -I"$T/memdiag" tests/t_jni_sig.c
  awk '/KL-TEST-BEGIN announce/{p=1;next} /KL-TEST-END announce/{p=0} p' runtime/media/kl_mediandk.c > "$T/memdiag/announce.inc"
  run t_media_announce -I"$T/memdiag" tests/t_media_announce.c
  awk '/KL-TEST-BEGIN backpressure/{p=1;next} /KL-TEST-END backpressure/{p=0} p' runtime/media/kl_mediandk.c > "$T/memdiag/backpressure.inc"
  run t_media_backpressure -I"$T/memdiag" tests/t_media_backpressure.c
  mkdir -p "$T/fence"
  awk '/KL-TEST-BEGIN fence/{p=1;next} /KL-TEST-END fence/{p=0} p' runtime/gfx/kl_vulkan.c > "$T/fence/fence_blocks.inc"
  run t_frame_fence -Wno-unused-function -I"$T/fence" -I"$VPK/gpu/vulkan/third_party/vulkan-headers/include" tests/t_frame_fence.c -lpthread
  return $rc
}

# The three groups are independent: run them side by side, each into its own log.
for g in gpu_host metal2_xros klepton_tests; do
  ( set -o pipefail; $g ) >"$WORK/$g.log" 2>&1 &
  eval "PID_$g=$!"
done
FAIL=0
for g in gpu_host metal2_xros klepton_tests; do
  eval "wait \$PID_$g"; rc=$?
  echo; echo "=================== $g (exit $rc)"
  cat "$WORK/$g.log"
  if [ $rc -ne 0 ]; then echo "FAILED: $g"; FAIL=1; fi
done
echo; echo "=================== summary"
grep -hE 'tests passed|tests failed|^ok: |^FAILED' "$WORK"/*.log || true
if [ $FAIL -ne 0 ]; then echo "host-tests: FAILED"; exit 1; fi
echo "host-tests: OK"
