#!/bin/bash
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/.." && pwd)
cache_root="${TMPDIR:-/private/tmp}/esp32-hub-native-cache"
mkdir -p "$cache_root"
export CLANG_MODULE_CACHE_PATH="$cache_root/clang"
export SWIFTPM_MODULECACHE_OVERRIDE="$cache_root/modules"
export XDG_CACHE_HOME="$cache_root"
swift build --package-path "$repo_root/mac_bridge/native" --disable-sandbox --disable-dependency-cache \
  --disable-build-manifest-caching --manifest-cache local --scratch-path "$cache_root/build" \
  --cache-path "$cache_root/cache" --config-path "$cache_root/config" --security-path "$cache_root/security"
app_path="$repo_root/mac_bridge/native/build/DesktopHubHelper.app"
mkdir -p "$app_path/Contents/MacOS"
cp "$cache_root/build/debug/DesktopHubHelper" "$app_path/Contents/MacOS/DesktopHubHelper.new"
mv "$app_path/Contents/MacOS/DesktopHubHelper.new" "$app_path/Contents/MacOS/DesktopHubHelper"
cat > "$app_path/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>local.esp32.DesktopHubHelper</string>
<key>CFBundleName</key><string>ESP32 Desktop Hub Helper</string>
<key>CFBundleExecutable</key><string>DesktopHubHelper</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>0.8.0</string>
<key>LSUIElement</key><true/>
<key>NSBluetoothAlwaysUsageDescription</key><string>读取并控制你主动选择的已配对蓝牙设备。</string>
<key>NSLocationWhenInUseUsageDescription</key><string>读取当前 Mac Wi-Fi 名称，以确认已保存网络的切换结果。</string>
</dict></plist>
PLIST
codesign --force --sign - "$app_path"
echo "Built $app_path (local ad hoc signing)"
