# cc2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(cc2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Tracy profiler and the SDK's perf-counter overlay stay out of shipped builds.
set(REXGLUE_ENABLE_TRACY OFF CACHE BOOL "Enable Tracy profiler integration")
set(REXGLUE_ENABLE_PERF_COUNTERS OFF CACHE BOOL "Enable lightweight perf counter registry and overlay")

# Shipped binaries carry no build-machine paths: everything under the checkout's parent folder
# (this repo, the SDK, jmlive) is recorded relative to it.
get_filename_component(CHECKOUT_PARENT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_compile_options("-ffile-prefix-map=${CHECKOUT_PARENT_DIR}/=")

include(generated/rexglue.cmake)

# sources.cmake is committed so a fresh tree knows the generated file names
# up front; watching it makes a build reconfigure if codegen reshapes the list.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/generated/cc2/sources.cmake")

# The repo builds the TU1-patched image; guest addresses in src/ use the TU1
# mapping (see src/dcc2_tu1_map.h).
add_compile_definitions(DCC2_TU1)

# Sources: the app shell and the guest-side hooks, plus the title-owned
# subsystems (audio backend, UI overlays, kernel exports). Those compile into
# the exe so their __imp__ definitions override the runtime's at link time.
file(GLOB AVATAR_LIB_SOURCES src/kernel/xam/avatars/*.cpp)
set(CC2_SOURCES
    src/main.cpp
    src/sentient.cpp
    src/shop_task_guard.cpp
    src/notify_hooks.cpp
    src/loader_hooks.cpp
    src/crash_log.cpp
    src/cc2/scene_driver.cpp
    src/cc2/famestar.cpp
    src/cc2/menu_map.cpp
    src/cc2/jinx_wearables.cpp
    src/cc2/display_settings.cpp
    src/cc2/freecam.cpp
    src/cc2/achievement_catalog.cpp
    src/cc2/mouse_ui.cpp
    src/cc2/keyboard_bindings.cpp
    src/cc2/online.cpp
    src/cc2/lobby.cpp
    src/cc2/relay.cpp
    src/cc2/avatar_fetch.cpp
    src/cc2/avatar_light.cpp
    src/cc2/avatar_hair.cpp
    src/cc2/tournament.cpp
    src/gpu/edge.cpp
    src/gpu/gpu_capture.cpp
    src/gpu/gpu_cmdbuf.cpp
    src/gpu/gpu_draw.cpp
    src/gpu/gpu_formats.cpp
    src/gpu/gpu_memory.cpp
    src/gpu/gpu_overlay.cpp
    src/gpu/gpu_renderer.cpp
    src/gpu/gpu_shader_pack.cpp
    src/gpu/gpu_texture_decode.cpp
    src/gpu/gpu_pointer.cpp
    src/gpu/gpu_tracker.cpp
    src/cc2/sdk_compat_check.cpp
    src/dll_dir.cpp
    src/audio/xma_kick.cpp
    src/kernel/function_dispatch.cpp
    src/kernel/content_seed.cpp
    src/kernel/export_override.cpp
    src/kernel/vfs_cull.cpp
    src/kernel/xam/xam_content.cpp
    src/kernel/xam/xam_input.cpp
    src/kernel/xam/xam_msg.cpp
    src/kernel/xam/xam_net.cpp
    src/kernel/xam/xam_avatar.cpp
    src/kernel/xam/xam_user.cpp
    ${AVATAR_LIB_SOURCES}
    src/kernel/xam/xhttp.cpp
    src/kernel/xam/apps/xlivebase_app.cpp
    src/kernel/xam/avatar_paths.cpp
    src/kernel/xam/apps/cc2_xgi_app.cpp
    src/kernel/xboxkrnl/thread_affinity.cpp
    src/kernel/xboxkrnl/nt_open_file.cpp
)

# Title audio backend: XAudio2 on Windows, selected via
# RuntimeConfig::audio_factory in cc2_app.h OnPreSetup.
if(WIN32)
    list(APPEND CC2_SOURCES
        src/audio/xaudio2/xaudio2_audio_driver.cpp
        src/audio/xaudio2/xaudio2_audio_system.cpp
    )
endif()

if(WIN32)
    add_executable(cc2 WIN32 ${CC2_SOURCES})
    # Ship as crashcourse2.exe; the CMake target stays "cc2".
    set_target_properties(cc2 PROPERTIES OUTPUT_NAME "crashcourse2")
    # Windows resources: app icon (MAINICON, loaded by the runtime's window
    # class) plus the version block (Task Manager display name). Linked as a
    # precompiled .res rather than built through CMake's clang RC pipeline;
    # regenerate with:
    #   rc.exe /nologo /fo src/cc2.res src/cc2.rc
    target_link_options(cc2 PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/cc2.res")
else()
    add_executable(cc2 ${CC2_SOURCES})
endif()

# Title-owned headers live under src/cc2/, clear of the SDK's rex/ tree.
target_include_directories(cc2 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/glm
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ImGui headers for the overlays. Include dirs only: the objects live in the
# runtime DLL, which exports them.
target_include_directories(cc2 PRIVATE
    $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)

# plume RHI for the native renderer (src/gpu).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/plume" plume)
target_link_libraries(cc2 PRIVATE plume)

# The XenosRecomp fork's translator library (tools/XenosRecomp, read only): the X4 pack reader, the fp2 key and the
# vfetch layout table. DXC is delay-loaded, so the game only needs its DLLs once a runtime translation runs.
enable_language(C)
set(XENOS_RECOMP_THIRDPARTY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/tools/XenosRecomp/thirdparty")
set(XENOS_RECOMP_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${XENOS_RECOMP_THIRDPARTY_ROOT}" xenosrecomp_thirdparty EXCLUDE_FROM_ALL)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/tools/XenosRecomp/XenosRecomp" xenosrecomp EXCLUDE_FROM_ALL)
target_link_libraries(cc2 PRIVATE xenosrecomp_core xxHash::xxhash)
target_link_libraries(cc2 PRIVATE winhttp crypt32)
if(WIN32)
    # The runtime and DXC load from bin\ beside the exe (src/dll_dir.cpp).
    target_link_options(cc2 PRIVATE "LINKER:/DELAYLOAD:$<TARGET_FILE_NAME:rexruntime>"
        "LINKER:/DELAYLOAD:dxcompiler.dll" "LINKER:/DELAYLOAD:dxil.dll")
    target_link_libraries(cc2 PRIVATE delayimp)
    add_custom_command(TARGET cc2 POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${XENOS_RECOMP_THIRDPARTY_ROOT}/dxc-bin/bin/x64/dxcompiler.dll"
        "${XENOS_RECOMP_THIRDPARTY_ROOT}/dxc-bin/bin/x64/dxil.dll"
        "$<TARGET_FILE_DIR:cc2>")
endif()
# Dev builds fall back to out/shaderpack and shaderpack/ in the source tree; Release uses the pack beside the exe.
target_compile_definitions(cc2 PRIVATE
    CC2_SOURCE_DIR="$<$<NOT:$<CONFIG:Release>>:${CMAKE_CURRENT_SOURCE_DIR}>")

# XAudio2 system library for the title audio backend.
if(WIN32)
    target_link_libraries(cc2 PRIVATE xaudio2 comctl32)
endif()

# No GPU plugin: the native renderer in src/gpu owns the window (see cc2_app.h).
rexglue_setup_target(cc2)

# JMlive: the Guide with Game Jolt sign-in; TITLE_INPUT leaves XamInputGetState to xam_input.cpp.
set(JMLIVE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../jmlive" CACHE PATH "")
include("${JMLIVE_DIR}/jmlive/jmlive.cmake")
jmlive_attach(cc2 TITLE_INPUT)

# The config is read from the executable root.
if(NOT EXISTS "${CMAKE_BINARY_DIR}/crashcourse2.toml")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/crashcourse2.toml"
                   "${CMAKE_BINARY_DIR}/crashcourse2.toml" COPYONLY)
endif()
