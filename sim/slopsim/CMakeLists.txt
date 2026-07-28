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
    # The firmware's own fray-d port — pure math (math.h only), no Arduino/
    # FreeRTOS deps, already host-tested. MachineSim reuses advpat::Settings
    # verbatim for pattern-advanced (0x1210) parity (19-channel sim-fidelity
    # follow-on) rather than re-deriving the same clamp/coupling math twice.
    "${REPO_ROOT}/src/motion/AdvancedPattern.cpp"
    ${RUCKIG_SOURCES})

target_include_directories(slopsim PRIVATE
    src
    "${REPO_ROOT}/lib/slopsync/include"
    "${REPO_ROOT}/lib/slopmotion/include"
    "${REPO_ROOT}/lib/ruckig/include"
    "${REPO_ROOT}/include"
    # AdvancedPattern.cpp's own #include "AdvancedPattern.h" is unqualified —
    # PlatformIO's LDF auto-adds every include/ subdirectory to -I, which is
    # why the firmware build resolves it with no path prefix; CMake does not
    # do that implicitly, so its home directory is added explicitly here.
    "${REPO_ROOT}/include/motion")
    # include/comms (the firmware's own SlopSyncCatalog.h) IS on this path —
    # SlopDeck DESIGN.md's sim-fidelity ruling (docs/slopdeck/DESIGN.md §5/§7)
    # restores the `device` profile to LITERAL device-catalog fidelity: the
    # sim calls buildSlopDriveCatalog() from THIS header rather than
    # re-authoring a parallel copy, so the two can never silently diverge.
    # That header is hardware-free (native tests already build it), so this
    # is a safe include on the host. src/machine/SlopSimCatalog.h's
    # benchrig:: catalog is UNCHANGED and still used for `--profile alien`.

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
