#!/usr/bin/env bash
# VPKEngine's Vulkan driver (gpu/vulkan, target VPVulkan) for the visionOS device, packaged as
# VPVulkan.framework for the app's Frameworks/ (Klepton loads it by path: KL_MVK_DYLIB).
# Usage: build-vpvulkan.sh <VPKEngine checkout> <output dir>
set -euo pipefail
SRC="$1"
OUT="$2"
BUILD="$OUT/vpvulkan-build"
cmake -S "$SRC/gpu" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT=xros -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_MACOSX_BUNDLE=OFF -DBUILD_TESTING=OFF
ninja -C "$BUILD" VPVulkan
DYLIB=$(find "$BUILD" -name 'libVPVulkan.dylib' -type f | head -n 1)
[ -n "$DYLIB" ] || { echo "libVPVulkan.dylib not built"; exit 1; }
FW="$OUT/VPVulkan.framework"
rm -rf "$FW"
mkdir -p "$FW"
cp "$DYLIB" "$FW/VPVulkan"
install_name_tool -id @rpath/VPVulkan.framework/VPVulkan "$FW/VPVulkan"
cat > "$FW/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>VPVulkan</string>
  <key>CFBundleIdentifier</key><string>com.kdt.livecontainer.vpvulkan</string>
  <key>CFBundleName</key><string>VPVulkan</string>
  <key>CFBundlePackageType</key><string>FMWK</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>CFBundleSupportedPlatforms</key><array><string>XROS</string></array>
  <key>MinimumOSVersion</key><string>26.0</string>
</dict>
</plist>
PLIST
xcrun vtool -show-build "$FW/VPVulkan" | grep -E "platform|minos" || true
nm -gU "$FW/VPVulkan" | grep -c " _vk" | sed 's/^/exported vk* symbols: /'
ls -la "$FW"
