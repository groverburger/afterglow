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
cat > "$STAGE/README.txt" <<TXT
Afterglow $VERSION - two-deck DJ app for Windows 10/11 (64-bit)

Double-click Afterglow.exe. Nothing to install: the built-in songs, font and
demo set are inside the exe.

Your music: on first run the Library shows your Music folder (including a
OneDrive Music folder). Add more with "+ Add folder" or by dragging a folder
onto the window. Supported: WAV, MP3, FLAC, M4A/AAC, WMA.

Clips, transitions and settings are stored in %APPDATA%\\Afterglow
(File > Open music folder). Exported sets go to Music\\Afterglow.

If Windows SmartScreen warns about an unknown app, click "More info" then
"Run anyway" (the exe is not code-signed).
TXT
# Windows line endings for Notepad users.
for f in "$STAGE/README.txt" "$STAGE"/licenses/*.txt; do perl -pi -e 's/\r?\n/\r\n/' "$f"; done
ZIP="dist/Afterglow-$VERSION-windows-x64.zip"
rm -f "$ZIP"
(cd build-win/package && zip -qr -X "../../$ZIP" Afterglow)
echo "Wrote $ZIP"
