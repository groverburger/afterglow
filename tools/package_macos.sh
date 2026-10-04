#!/bin/sh
# Builds a universal (Apple Silicon + Intel) Afterglow.app and wraps it in a DMG:
#   sh tools/package_macos.sh [version]   # -> dist/Afterglow-<version>-macos.dmg
set -e
cd "$(dirname "$0")/.."
VERSION=${1:-$(sed -n 's/^project(Afterglow VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}
cmake -S . -B build-mac -DCMAKE_BUILD_TYPE=Release \
  "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0
cmake --build build-mac --parallel --target afterglow
STAGE=build-mac/dmg
rm -rf "$STAGE" && mkdir -p "$STAGE" dist
cp -R build-mac/Afterglow.app "$STAGE/"
codesign --force --deep --sign - "$STAGE/Afterglow.app"  # ad-hoc: required to launch on Apple Silicon
ln -s /Applications "$STAGE/Applications"
DMG="dist/Afterglow-$VERSION-macos.dmg"
rm -f "$DMG"
hdiutil create -volname Afterglow -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG" >/dev/null
echo "Wrote $DMG"
