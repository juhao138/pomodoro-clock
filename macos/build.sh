#!/bin/bash
# 构建 macOS 版：编译 → 组装 .app → 可选生成图标
set -euo pipefail
cd "$(dirname "$0")"

ROOT="$(cd .. && pwd)"
BUILD="$ROOT/.build"
APP="$ROOT/dist/桌面番茄钟.app"
CACHE="$BUILD/swift-cache"

mkdir -p "$BUILD" "$CACHE"

echo "==> 编译（x86_64 + arm64 通用二进制）"
SOURCES="Sources/Theme.swift Sources/Text.swift Sources/Paint.swift \
Sources/Model.swift Sources/Home.swift Sources/SettingsPanel.swift \
Sources/App.swift Sources/main.swift"

for ARCH in x86_64 arm64; do
    echo "    - $ARCH"
    swiftc -O -target "${ARCH}-apple-macos12.0" \
        -module-cache-path "$CACHE-$ARCH" \
        -o "$BUILD/PomodoroClock-$ARCH" $SOURCES \
        -framework AppKit -framework AVFoundation -framework QuartzCore
done

lipo -create -output "$BUILD/PomodoroClock" \
    "$BUILD/PomodoroClock-x86_64" "$BUILD/PomodoroClock-arm64"

echo "==> 组装 app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/PomodoroClock" "$APP/Contents/MacOS/PomodoroClock"
cp Resources/Saira.ttf "$APP/Contents/Resources/Saira.ttf"
cp Resources/Saira-OFL.txt "$APP/Contents/Resources/Saira-OFL.txt"
[ -f "Resources/AppIcon.icns" ] && cp Resources/AppIcon.icns "$APP/Contents/Resources/AppIcon.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>PomodoroClock</string>
    <key>CFBundleDisplayName</key><string>桌面番茄钟</string>
    <key>CFBundleIdentifier</key><string>app.pomodoro.clock</string>
    <key>CFBundleExecutable</key><string>PomodoroClock</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>CFBundleVersion</key><string>1</string>
    <key>LSMinimumSystemVersion</key><string>12.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>NSPrincipalClass</key><string>NSApplication</string>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
</dict>
</plist>
PLIST

codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || true
lipo -archs "$APP/Contents/MacOS/PomodoroClock" | sed 's/^/==> 架构: /'
echo "==> 完成: $APP"
