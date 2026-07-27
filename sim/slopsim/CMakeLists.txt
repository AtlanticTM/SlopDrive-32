cmake_minimum_required(VERSION 3.24)
project(slopsim CXX)

# Host build only — never part of the PlatformIO firmware build. Windows-first
# (WinLibs MinGW-w64 GCC), Linux expected to work with the same deps.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clangd
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)  # gnu++2b parity with the repo's other host builds

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

include(FetchContent)

# ---- FTXUI (TUI) ------------------------------------------------------------
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui
    GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI.git
    GIT_TAG v6.1.9
    GIT_SHALLOW ON)

# ---- IXWebSocket (WS server + client) --------------------------------------
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.6
    GIT_SHALLOW ON
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchIXWebSocket.cmake)

# ---- cpp-httplib (page/capabilities facade — used from M5 on) ---------------
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.20.1
    GIT_SHALLOW ON)

FetchContent_MakeAvailable(ftxui ixwebsocket httplib)

# Ruckig is vendored as sources (SlopMotion is header-only but Ruckig is not).
file(GLOB RUCKIG_SOURCES "${REPO_ROOT}/lib/ruckig/src/ruckig/*.cpp")

add_executable(slopsim
    src/main.cpp
    src/net/WsServerPort.cpp
    src/net/HttpFacade.cpp
    src/net/MdnsAdvertiser.cpp
    src/common/OpenBrowser.cpp
    src/machine/MachineSim.cpp
    src/tui/MachineScreen.cpp
    ${RUCKIG_SOURCES})

target_include_directories(slopsim PRIVATE
    src
    "${REPO_ROOT}/lib/slopsync/include"
    "${REPO_ROOT}/lib/slopmotion/include"
    "${REPO_ROOT}/lib/ruckig/include")
    # NOTE: include/comms (the firmware's own SlopSyncCatalog.h) is
    # DELIBERATELY NOT on this path. slopsim declares its own, different
    # device catalog — src/machine/SlopSimCatalog.h — so that pointing a
    # client at the sim proves it can render a machine it has never met, not
    # just SlopDrive-32 again over a fresh socket. See that file's header.

target_compile_options(slopsim PRIVATE -Wall -Wextra)

# Standalone exe: no libstdc++/libgcc/winpthread DLL hunting — "just run it
# from a terminal" is the product requirement.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(slopsim PRIVATE -static)
endif()

target_link_libraries(slopsim PRIVATE
    ftxui::component ftxui::dom ftxui::screen
    ixwebsocket
    httplib::httplib)

if(WIN32)
    # winmm: timeBeginPeriod/timeEndPeriod — the 1 ms scheduler tick the motion
    # loop needs (HostPlatform.h HostTimerResolution). Without it Windows sleeps
    # in 15.625 ms quanta and the "1 kHz" sampler is a ~64 Hz staircase.
    target_link_libraries(slopsim PRIVATE ws2_32 dnsapi shell32 winmm)
endif()
