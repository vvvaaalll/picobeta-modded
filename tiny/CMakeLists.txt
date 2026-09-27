cmake_minimum_required(VERSION 3.16)
project(picobeta VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cross-compilation for Windows with MinGW-w64 (from WSL/Linux)
# Usage:
#   mkdir build && cd build
#   cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-mingw64.cmake ..
#   cmake --build .
# Or simply (if toolchain set):
#   x86_64-w64-mingw32-cmake ..   (if available)
#   or set CXX etc.

add_executable(picobeta
    src/main.cpp
)

# Set TARGET_WINXP=ON (e.g. via the toolchain-mingw32-xp.cmake toolchain file)
# to target the oldest Windows Winsock/API level (Windows XP, WINNT 5.01)
# instead of the default (Windows 7, WINNT 6.01).
option(TARGET_WINXP "Target Windows XP API/Winsock compatibility" OFF)

# Winsock for Windows
if(WIN32 OR MINGW OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(picobeta PRIVATE ws2_32)
    if(TARGET_WINXP)
        target_compile_definitions(picobeta PRIVATE _WIN32_WINNT=0x0501 WINVER=0x0501 NTDDI_VERSION=0x05010000)
    else()
        target_compile_definitions(picobeta PRIVATE _WIN32_WINNT=0x0601)
    endif()
endif()

# Static linking for easier distribution (optional, comment if issues)
if(MINGW)
    target_link_options(picobeta PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS picobeta RUNTIME DESTINATION bin)
