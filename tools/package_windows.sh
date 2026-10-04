#!/bin/sh
# Cross-compiles Afterglow for 64-bit Windows with MinGW-w64 and zips it:
#   brew install mingw-w64 cmake   # once
#   sh tools/package_windows.sh    # -> dist/Afterglow-<version>-windows-x64.zip
set -e
cd "$(dirname "$0")/.."
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --parallel --target afterglow
VERSION=$(sed -n 's/^project(Afterglow VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
STAGE=build-win/package/Afterglow
rm -rf build-win/package && mkdir -p "$STAGE/licenses" dist
x86_64-w64-mingw32-strip -o "$STAGE/Afterglow.exe" build-win/Afterglow.exe
cp assets/Roboto-LICENSE.txt "$STAGE/licenses/Roboto.txt"
cp third_party/imgui/LICENSE.txt "$STAGE/licenses/Dear ImGui.txt"
cp third_party/sokol/LICENSE "$STAGE/licenses/sokol.txt"
cp third_party/dr_libs/LICENSE "$STAGE/licenses/dr_libs.txt"
cp packaging/windows-README.txt "$STAGE/README.txt"
# Windows line endings for Notepad users.
for f in "$STAGE/README.txt" "$STAGE"/licenses/*.txt; do perl -pi -e 's/\r?\n/\r\n/' "$f"; done
ZIP="dist/Afterglow-$VERSION-windows-x64.zip"
rm -f "$ZIP"
(cd build-win/package && zip -qr -X "../../$ZIP" Afterglow)
echo "Wrote $ZIP"
