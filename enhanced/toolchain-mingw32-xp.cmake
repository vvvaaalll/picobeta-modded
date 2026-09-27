# CMake toolchain file for cross-compiling to Windows XP-compatible 32-bit (i686)
# with MinGW-w64, from WSL/Linux.
#
# Usage:
#   mkdir -p build-win32 && cd build-win32
#   cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw32-xp.cmake ..
#   cmake --build .
#
# Produces a statically-linked 32-bit .exe with the PE subsystem/OS version
# set to 5.01 and Winsock/Windows headers pinned to the XP API level, so it
# runs on Windows XP SP2/SP3 (and everything newer) without needing any
# MinGW runtime DLLs installed on the target machine.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86)

set(CMAKE_C_COMPILER   i686-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER i686-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  i686-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/i686-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Tell CMakeLists.txt to compile against the Windows XP (WINNT 5.01) API level
# instead of the default Windows 7 level.
set(TARGET_WINXP ON CACHE BOOL "Target Windows XP API/Winsock compatibility" FORCE)

# Static link (no external MinGW DLLs needed on the target machine) and pin
# the PE subsystem/OS version to 5.01 so Windows XP's loader accepts the exe
# (MinGW's default of 6.0 makes XP refuse to start it).
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-static -static-libgcc -static-libstdc++ -Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1 -Wl,--major-os-version,5 -Wl,--minor-os-version,1")
