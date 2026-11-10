#!/bin/bash
# 打 dmg：先编译，再把 .app 和「应用程序」替身装进磁盘映像
set -euo pipefail
cd "$(dirname "$0")"

ROOT="$(cd .. && pwd)"
APP="$ROOT/dist/桌面番茄钟.app"
DMG="$ROOT/dist/桌面番茄钟-1.0.dmg"
STAGE="$ROOT/.build/dmgroot"
MNT="$ROOT/.build/mnt"

./build.sh

echo "==> 打 dmg"
rm -rf "$STAGE" "$MNT"
mkdir -p "$STAGE" "$MNT"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"

rm -f "$DMG"
hdiutil create -quiet -volname "桌面番茄钟" -srcfolder "$STAGE" -format UDZO "$DMG"
hdiutil verify "$DMG" 2>&1 | tail -1

echo "==> 完成: $DMG（$(du -h "$DMG" | cut -f1)）"
echo "    想放到桌面：cp \"$DMG\" ~/Desktop/"
