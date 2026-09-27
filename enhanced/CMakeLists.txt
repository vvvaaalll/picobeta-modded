cmake_minimum_required(VERSION 3.16)
project(picobeta-enhanced VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(picobeta-enhanced
    src/main.cpp
)

# Set TARGET_WINXP=ON (e.g. via the toolchain-mingw32-xp.cmake toolchain file)
# to target the oldest Windows Winsock/API level (Windows XP, WINNT 5.01)
# instead of the default (Windows 7, WINNT 6.01).
option(TARGET_WINXP "Target Windows XP API/Winsock compatibility" OFF)

if(WIN32 OR MINGW OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(picobeta-enhanced PRIVATE ws2_32)
    if(TARGET_WINXP)
        target_compile_definitions(picobeta-enhanced PRIVATE _WIN32_WINNT=0x0501 WINVER=0x0501 NTDDI_VERSION=0x05010000)
    else()
        target_compile_definitions(picobeta-enhanced PRIVATE _WIN32_WINNT=0x0601)
    endif()
endif()

if(MINGW)
    target_link_options(picobeta-enhanced PRIVATE -static -static-libgcc -static-libstdc++)
endif()

install(TARGETS picobeta-enhanced RUNTIME DESTINATION bin)
