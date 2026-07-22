# CMake toolchain: cross-compile the Windows x86_64 CLIENT from Linux.
#
# Purpose: the host runs the dedicated server on Linux; players on Windows only
# need fallout2-ce.exe (the viewer). This lets that .exe be produced from the
# same Linux box, so no Windows machine is needed to ship binaries to them.
#
# The f2_server target is skipped automatically for WIN32 (see CMakeLists.txt) —
# server_net.cc is still POSIX-only, by design.
#
# Usage:
#   cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-win -j
#
# Prerequisites (Debian/Ubuntu names):
#   g++-mingw-w64-x86-64        the C++ cross compiler (gcc alone is NOT enough)
#   SDL2 mingw development libs — distros generally do not package these; take
#     the official SDL2-devel-<ver>-mingw.tar.gz from libsdl.org and point
#     SDL2_DIR / CMAKE_PREFIX_PATH at its x86_64-w64-mingw32 prefix.
#   zlib built for mingw, likewise via CMAKE_PREFIX_PATH.
# Extra prefixes are searched because CMAKE_FIND_ROOT_PATH is listed below.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(TOOLCHAIN_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER ${TOOLCHAIN_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_PREFIX}-g++)
set(CMAKE_RC_COMPILER ${TOOLCHAIN_PREFIX}-windres)

# Where to look for the target's headers/libraries. Append your SDL2/zlib mingw
# prefixes here (or pass -DCMAKE_PREFIX_PATH=... on the command line).
set(CMAKE_FIND_ROOT_PATH /usr/${TOOLCHAIN_PREFIX})

# Programs come from the HOST (cmake, make); headers and libraries must come
# from the TARGET sysroot only, or the build silently picks up Linux ones.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Ship a self-contained .exe so players do not need the MinGW runtime DLLs
# alongside it (SDL2.dll is still required next to the executable).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static-libgcc -static-libstdc++")
