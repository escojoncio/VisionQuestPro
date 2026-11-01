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
  run t_ovrp_render -Wno-unused-function -I.. -I../.. tests/t_ovrp_render.c -lm
  run t_ovrp_misc -Wno-unused-function -I.. -I../.. tests/t_ovrp_misc.c
  run spatial_test -I.. -I../jni -I../libc -I../gfx -I. -I../media -I../guest -I../diag \
      tests/spatial_test.c kl_spatial.c kl_ovrp_spatial.c -lm -lpthread
  cd "$ROOT/klepton" || return 1
  run t_ptw tests/t_ptw.c runtime/gfx/kl_reproject_ptw.c -lm
  run t_bg -rdynamic -Iruntime/gfx tests/t_bg.c runtime/gfx/kl_bg.c -lpthread -ldl
  run t_frame_boundary -rdynamic -Iruntime/gfx tests/t_frame_boundary.c runtime/gfx/kl_bg.c -lpthread -ldl
  run t_frame_boundary_nosym -DNO_SYM -rdynamic -Iruntime/gfx tests/t_frame_boundary.c runtime/gfx/kl_bg.c -lpthread -ldl
  run t_diagcap -rdynamic -Iruntime/diag -Iruntime/xr -Iruntime tests/t_diagcap.c runtime/diag/kl_diagcap.c runtime/kl_env.c -lpthread -ldl
  mkdir -p "$T/memdiag"
  awk '/KL-TEST-BEGIN (rate|range|fstat)/{p=1;next} /KL-TEST-END (rate|range|fstat)/{p=0} p' runtime/libc/kl_libc.c > "$T/memdiag/libc_blocks.inc"
  awk '/KL-TEST-BEGIN ent/{p=1;next} /KL-TEST-END ent/{p=0} p' visionos/Sources/kl_app.c > "$T/memdiag/app_blocks.inc"
  run t_memdiag -I"$T/memdiag" tests/t_memdiag.c -lpthread
  awk '/^static int klj_decode_args\(/,/^}/{print} /^static int klj_decode_args_a\(/,/^}/{print}' runtime/kl_jni.c > "$T/memdiag/jni_decoders.inc"
  run t_jni_sig -I"$T/memdiag" tests/t_jni_sig.c
  mkdir -p "$T/libcsem"
  awk '/KL-TEST-BEGIN (affinity|madvclip)/{p=1;next} /KL-TEST-END (affinity|madvclip)/{p=0} p' runtime/libc/kl_libc.c > "$T/libcsem/libc_blocks.inc"
  awk '/KL-TEST-BEGIN pthmap/{p=1;next} /KL-TEST-END pthmap/{p=0} p' runtime/libc/kl_pthread.c > "$T/libcsem/pthread_blocks.inc"
  awk '/KL-TEST-BEGIN mallocx/{p=1;next} /KL-TEST-END mallocx/{p=0} p' runtime/libc/kl_shim.c > "$T/libcsem/shim_blocks.inc"
  run t_libc_sem -Wno-unused-function -I"$T/libcsem" tests/t_libc_sem.c -lpthread
  awk '/KL-TEST-BEGIN announce/{p=1;next} /KL-TEST-END announce/{p=0} p' runtime/media/kl_mediandk.c > "$T/memdiag/announce.inc"
  run t_media_announce -I"$T/memdiag" tests/t_media_announce.c
  awk '/KL-TEST-BEGIN backpressure/{p=1;next} /KL-TEST-END backpressure/{p=0} p' runtime/media/kl_mediandk.c > "$T/memdiag/backpressure.inc"
  run t_media_backpressure -I"$T/memdiag" tests/t_media_backpressure.c
  mkdir -p "$T/ldeps"
  awk '/KL-TEST-BEGIN ldeps/{p=1;next} /KL-TEST-END ldeps/{p=0} p' runtime/kl_dl.c > "$T/ldeps/ldeps.inc"
  run t_load_needed -Wno-unused-function -I"$T/ldeps" tests/t_load_needed.c
  mkdir -p "$T/fence"
  awk '/KL-TEST-BEGIN fence/{p=1;next} /KL-TEST-END fence/{p=0} p' runtime/gfx/kl_vulkan.c > "$T/fence/fence_blocks.inc"
  run t_frame_fence -Wno-unused-function -I"$T/fence" -I"$VPK/gpu/vulkan/third_party/vulkan-headers/include" tests/t_frame_fence.c -lpthread
  mkdir -p "$T/newt"
  awk '/KL-TEST-BEGIN msgtype/{p=1;next} /KL-TEST-END msgtype/{p=0} p' runtime/xr/kl_ovrplat.c > "$T/newt/msgtype.inc"
  awk '/KL-TEST-BEGIN owns/{p=1;next} /KL-TEST-END owns/{p=0} p' runtime/xr/kl_ovrplat.c > "$T/newt/owns.inc"
  run t_ovrplat_msgtype -Wno-unused-function -I"$T/newt" tests/t_ovrplat_msgtype.c
  awk '/KL-TEST-BEGIN muxer/{p=1;next} /KL-TEST-END muxer/{p=0} p' runtime/media/kl_mediandk.c > "$T/newt/muxer.inc"
  awk '/^    M\("AMediaMuxer_/' runtime/media/kl_mediandk.c > "$T/newt/muxer_tab.inc"
  run t_media_muxer -Wno-unused-function -I"$T/newt" tests/t_media_muxer.c
  awk '/KL-TEST-BEGIN retiredue/{p=1;next} /KL-TEST-END retiredue/{p=0} p' runtime/gfx/kl_vulkan.c > "$T/newt/retiredue.inc"
  run t_retire_due -Wno-unused-function -I"$T/newt" tests/t_retire_due.c
  awk '/KL-TEST-BEGIN vktrap/{p=1;next} /KL-TEST-END vktrap/{p=0} p' runtime/gfx/kl_vulkan.c > "$T/newt/vktrap.inc"
  run t_vk_trap -Wno-unused-function -I"$T/newt" -I"$VPK/gpu/vulkan/third_party/vulkan-headers/include" tests/t_vk_trap.c
  awk '/KL-TEST-BEGIN robust/{p=1;next} /KL-TEST-END robust/{p=0} p' runtime/gfx/kl_vulkan.c > "$T/newt/robust.inc"
  run t_vk_robust -Wno-unused-function -I"$T/newt" -I"$VPK/gpu/vulkan/third_party/vulkan-headers/include" tests/t_vk_robust.c
  awk '/KL-TEST-BEGIN ue4sig/{p=1;next} /KL-TEST-END ue4sig/{p=0} p' runtime/guest/kl_ue4.c > "$T/newt/ue4sig.inc"
  run t_ue4_sig -Wno-unused-function -I"$T/newt" tests/t_ue4_sig.c
  awk '/KL-TEST-BEGIN ue4cmd/{p=1;next} /KL-TEST-END ue4cmd/{p=0} p' runtime/guest/kl_ue4.c > "$T/newt/ue4cmd.inc"
  run t_ue4_cmdline -Wno-unused-function -I"$T/newt" tests/t_ue4_cmdline.c
  awk '/KL-TEST-BEGIN aswsize/{p=1;next} /KL-TEST-END aswsize/{p=0} p' runtime/xr/kl_ovrp.c > "$T/newt/aswsize.inc"
  run t_asw_size -Wno-unused-function -I"$T/newt" tests/t_asw_size.c
  awk '/KL-TEST-BEGIN ptscan/{p=1;next} /KL-TEST-END ptscan/{p=0} p' runtime/xr/kl_ovrp.c > "$T/newt/ptscan.inc"
  run t_pt_scan -Wno-unused-function -I"$T/newt" tests/t_pt_scan.c
  awk '/KL-TEST-BEGIN bslash/{p=1;next} /KL-TEST-END bslash/{p=0} p' runtime/libc/kl_libc.c > "$T/newt/bslash.inc"
  run t_bslash -Wno-unused-function -I"$T/newt" tests/t_bslash.c
  awk '/KL-TEST-BEGIN fqonce/{p=1;next} /KL-TEST-END fqonce/{p=0} p' runtime/kl_fault.c > "$T/newt/fqonce.inc"
  run t_forcequit -Wno-unused-function -I"$T/newt" tests/t_forcequit.c
  awk '/KL-TEST-BEGIN seatoff/{p=1;next} /KL-TEST-END seatoff/{p=0} p' runtime/xr/kl_ovrp.c > "$T/newt/seatoff.inc"
  run t_seat_offset -Wno-unused-function -I"$T/newt" tests/t_seat_offset.c -lm
  awk '/KL-TEST-BEGIN ovrpfallback/{p=1;next} /KL-TEST-END ovrpfallback/{p=0} p' runtime/xr/kl_ovrp.c > "$T/newt/ovrpfallback.inc"
  run t_ovrp_fallback -Wno-unused-function -I"$T/newt" tests/t_ovrp_fallback.c
  mkdir -p "$T/vmlog"
  awk '/KL-TEST-BEGIN vmgroup/{p=1;next} /KL-TEST-END vmgroup/{p=0} p' runtime/libc/kl_libc.c > "$T/vmlog/vmgroup.inc"
  run t_vmgroup -Wno-unused-function -I"$T/vmlog" tests/t_vmgroup.c
  awk '/KL-TEST-BEGIN lring/{p=1;next} /KL-TEST-END lring/{p=0} p' runtime/libc/kl_va_handlers.c > "$T/vmlog/lring.inc"
  run t_lring -I"$T/vmlog" tests/t_lring.c
  awk '/KL-TEST-BEGIN synthobj/{p=1;next} /KL-TEST-END synthobj/{p=0} p' runtime/kl_jni.c > "$T/newt/synthobj.inc"
  run t_jni_synth -I"$T/newt" tests/t_jni_synth.c
  awk '/KL-TEST-BEGIN ue4recv/{p=1;next} /KL-TEST-END ue4recv/{p=0} p' runtime/jni/kl_jni_ue4.c > "$T/newt/ue4recv.inc"
  run t_ue4_receivers -Wno-unused-function -I"$T/newt" tests/t_ue4_receivers.c
  awk '/KL-TEST-BEGIN ue4thunk/{p=1;next} /KL-TEST-END ue4thunk/{p=0} p' runtime/jni/kl_jni_ue4.c > "$T/newt/ue4thunk.inc"
  run t_ue4_thunk -Wno-unused-function -I"$T/newt" tests/t_ue4_thunk.c
  awk '/KL-TEST-BEGIN noophit/{p=1;next} /KL-TEST-END noophit/{p=0} p' runtime/kl_jni.c > "$T/newt/noophit.inc"
  run t_noop_hit -Wno-unused-function -I"$T/newt" tests/t_noop_hit.c -lpthread
  return $rc
}

# The three groups are independent: run them side by side, each into its own log.
for g in gpu_host metal2_xros klepton_tests; do
  ( set -o pipefail; $g ) >"$WORK/$g.log" 2>&1 &
  eval "PID_$g=$!"
done
# Watchdog: a group still running after 7 min is hung (they take ~1.5 min). Kill it so its partial log is printed below.
( sleep 420; for g in gpu_host metal2_xros klepton_tests; do eval "kill \$PID_$g 2>/dev/null"; done ) &
WD=$!
FAIL=0
for g in gpu_host metal2_xros klepton_tests; do
  eval "wait \$PID_$g"; rc=$?
  echo; echo "=================== $g (exit $rc)"
  cat "$WORK/$g.log"
  if [ $rc -ne 0 ]; then echo "FAILED: $g"; FAIL=1; fi
done
kill $WD 2>/dev/null
echo; echo "=================== summary"
grep -hE 'tests passed|tests failed|^ok: |^FAILED' "$WORK"/*.log || true
if [ $FAIL -ne 0 ]; then echo "host-tests: FAILED"; exit 1; fi
echo "host-tests: OK"
