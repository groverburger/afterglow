# Cross-compile a Windows build with MinGW-w64 (e.g. `brew install mingw-w64`):
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake
#   cmake --build build-win --parallel
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)
set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
