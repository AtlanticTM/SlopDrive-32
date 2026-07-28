cmake_minimum_required(VERSION 3.24)
project(slopbench CXX)

# Host build only -- never part of the PlatformIO firmware build. Windows-first
# (WinLibs MinGW-w64 GCC), matching sim/slopsim's own toolchain so both sims
# build with the same one-line PATH prepend.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clangd
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)  # gnu++2b parity with the repo's other host builds

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

include(FetchContent)

# ---- IXWebSocket (WS server) ------------------------------------------------
# The ONLY external dependency: SlopBench is deliberately lean (task brief:
# "mostly config parsing + a value store + the TUI") and needs no motion
# engine (no Ruckig), no TUI framework (plain ANSI redraw, see tui/BenchTui),
# and no HTTP facade (WS-only per the task brief -- no page to serve).
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.6
    GIT_SHALLOW ON
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchIXWebSocket.cmake)

FetchContent_MakeAvailable(ixwebsocket)

add_executable(slopbench
    src/main.cpp
    src/net/WsServerPort.cpp
    src/config/ConfigParser.cpp
    src/hub/BenchHub.cpp
    src/tui/BenchTui.cpp)

target_include_directories(slopbench PRIVATE
    src
    "${REPO_ROOT}/lib/slopsync/include")

target_compile_options(slopbench PRIVATE -Wall -Wextra)

# Standalone exe: no libstdc++/libgcc/winpthread DLL hunting -- "just run it
# from a terminal" is the product requirement, same as sim/slopsim.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(slopbench PRIVATE -static)
endif()

target_link_libraries(slopbench PRIVATE ixwebsocket)

if(WIN32)
    target_link_libraries(slopbench PRIVATE ws2_32 dnsapi shell32)
endif()
