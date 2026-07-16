cmake_minimum_required(VERSION 3.21)

# Read version from the top-level VERSION file (single source of truth,
# also consumed by the makefile build).  No more two-file edits per bump.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" KONCPC_VERSION_RAW)
string(STRIP "${KONCPC_VERSION_RAW}" KONCPC_VERSION)
project(koncepcja VERSION ${KONCPC_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────
option(WITH_CURL  "Enable libcurl (M4 HTTP)"  OFF)
option(WITH_JPEG  "Enable libjpeg (AVI rec)"  OFF)

# Modern UI gate (P1.5.1 step 4).  ON = the current ImGui + SDL_GPU
# build.  OFF = exclude UI-only sources from compilation, leaving the
# core ready for a future headless main (P1.5.2) to wire up.  OFF is
# an in-progress state: kon_cpc_ja.cpp still references imgui_state
# from imgui_ui.h, so a full link with OFF will fail until P1.5.2
# splits that header.  Defaulting ON keeps every existing build
# (CI matrix, distro packages, local dev) bit-identical to today.
option(KONCPC_BUILD_MODERN_UI "Build the ImGui + SDL_GPU modern UI" ON)

# ── Git hash ─────────────────────────────────────────────
execute_process(
    COMMAND git rev-parse --verify HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

# ── SDL3 from vendor submodule ───────────────────────────
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/SDL EXCLUDE_FROM_ALL)

# ── External dependencies (vcpkg provides these on Windows) ──
find_package(Freetype REQUIRED)
find_package(PNG REQUIRED)
find_package(ZLIB REQUIRED)

if(WITH_CURL)
    find_package(CURL)
endif()
if(WITH_JPEG)
    find_package(JPEG)
endif()

# ── Collect main sources ─────────────────────────────────
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
    src/*.cpp
    src/*.c
)

# Exclude .mm files on non-Apple platforms
if(NOT APPLE)
    list(FILTER APP_SOURCES EXCLUDE REGEX ".*\\.mm$")
else()
    file(GLOB_RECURSE MM_SOURCES CONFIGURE_DEPENDS src/*.mm)
    list(APPEND APP_SOURCES ${MM_SOURCES})
    # Enable ObjC++ compilation for .mm files
    set_source_files_properties(${MM_SOURCES} PROPERTIES
        LANGUAGE OBJCXX
        COMPILE_FLAGS "-x objective-c++"
    )
    enable_language(OBJCXX)
endif()

# ── UI-only source files (P1.5.1 step 4) ─────────────────
# Files that have hard dependencies on Dear ImGui and/or the modern
# UI widgets.  When KONCPC_BUILD_MODERN_UI=OFF these are excluded
# from the library so the headless variant doesn't drag in ImGui
# transitively.  Vendored ImGui itself is handled separately below.
#
# Keep in sync if a new UI-only TU is added; CI catches inclusion
# regressions because the modern build still compiles them as today.
# Keep this list in sync with MODERN_UI_FILES in the makefile.
set(MODERN_UI_FILES
    imgui_ui imgui_ui_host devtools_ui command_palette workspace_layout
    video
)
list(JOIN MODERN_UI_FILES "|" MODERN_UI_FILES_JOINED)
set(MODERN_UI_SOURCES_REGEX "src/(${MODERN_UI_FILES_JOINED})\\.cpp$")

if(NOT KONCPC_BUILD_MODERN_UI)
    message(STATUS "KONCPC_BUILD_MODERN_UI=OFF — excluding modern UI sources from koncepcja_lib")
    message(STATUS "  Note: full link will fail until P1.5.2 lands a headless main and an ImGui-free imgui_ui.h split.")
    list(FILTER APP_SOURCES EXCLUDE REGEX "${MODERN_UI_SOURCES_REGEX}")
endif()

# ── Vendored ImGui sources ───────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
if(KONCPC_BUILD_MODERN_UI)
    set(IMGUI_SOURCES
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
        ${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp
    )
else()
    set(IMGUI_SOURCES "")
endif()

# ── Vendored ImGuiColorTextEdit sources (relocation sweep, 2026-07-13) ───
# Lives under vendor/ now, so it's not picked up by the src/*.cpp glob above
# — add it back explicitly, gated the same way MODERN_UI_FILES gated it when
# it lived in src/. See makefile's VENDOR_TEXTEDITOR_SOURCES for the mirror.
set(VENDOR_TEXTEDITOR_SOURCES
    ${CMAKE_SOURCE_DIR}/vendor/ImGuiColorTextEdit/TextEditor.cpp
    ${CMAKE_SOURCE_DIR}/vendor/ImGuiColorTextEdit/LanguageDefinitions.cpp
)
if(KONCPC_BUILD_MODERN_UI)
    list(APPEND APP_SOURCES ${VENDOR_TEXTEDITOR_SOURCES})
endif()

# ── Shared source library (used by both main exe and tests) ──
add_library(koncepcja_lib STATIC
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(koncepcja_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/vendor/msf_gif
    ${CMAKE_SOURCE_DIR}/vendor/ImGuiColorTextEdit
    ${CMAKE_SOURCE_DIR}/vendor/portable-file-dialogs
)

target_link_libraries(koncepcja_lib PUBLIC
    SDL3::SDL3-static
    Freetype::Freetype
    PNG::PNG
    ZLIB::ZLIB
)

# ── Compile definitions ──────────────────────────────────
if(GIT_HASH)
    target_compile_definitions(koncepcja_lib PUBLIC HASH="${GIT_HASH}")
endif()

# Bake the version into the binary.  Same compile-define pattern the
# makefile uses (see KONCPC_VERSION above).
target_compile_definitions(koncepcja_lib PUBLIC KONCPC_VERSION_STRING="v${PROJECT_VERSION}")

# Modern-UI guard define (P1.5.1 step 4).  Source files that have
# optional ImGui paths (currently: macos_menu.mm) check this to skip
# ImGui-only code in headless builds.
if(KONCPC_BUILD_MODERN_UI)
    target_compile_definitions(koncepcja_lib PUBLIC KONCPC_MODERN_UI)
endif()

if(WIN32)
    target_compile_definitions(koncepcja_lib PUBLIC WINDOWS _WIN32_WINNT=0x0601 NOMINMAX)
endif()

if(WITH_CURL AND CURL_FOUND)
    target_compile_definitions(koncepcja_lib PUBLIC HAS_LIBCURL)
    target_link_libraries(koncepcja_lib PUBLIC CURL::libcurl)
endif()

if(WITH_JPEG AND JPEG_FOUND)
    target_compile_definitions(koncepcja_lib PUBLIC HAS_LIBJPEG)
    target_link_libraries(koncepcja_lib PUBLIC JPEG::JPEG)
endif()

# ── Platform-specific linking ────────────────────────────
if(WIN32)
    target_link_libraries(koncepcja_lib PUBLIC ws2_32 winmm)
elseif(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    target_link_libraries(koncepcja_lib PUBLIC ${COCOA_FRAMEWORK})
endif()
# (Linux: no platform link flags — SDL_GPU's Vulkan backend doesn't need OpenGL.)

# ── Compiler flags ───────────────────────────────────────
if(MSVC)
    # Suppress common harmless warnings from legacy C/C++ code
    #   C4201: nameless struct/union (used heavily in register unions)
    #   C4244: conversion with possible data loss (int -> byte etc.)
    #   C4267: size_t -> int conversion
    #   C4100: unreferenced formal parameter
    target_compile_options(koncepcja_lib PUBLIC
        /W4
        /wd4201 /wd4244 /wd4267 /wd4100
        /utf-8
        /permissive-
    )
    # msvc_compat.h is included via koncepcja.h (#ifdef _MSC_VER)
    # NOT force-included via /FI — that pollutes Windows SDK headers
    # TODO: enable /WX once MSVC warnings are cleaned up
    # target_compile_options(koncepcja_lib PUBLIC $<$<CONFIG:Debug>:/WX>)
else()
    target_compile_options(koncepcja_lib PUBLIC
        -Wall -Wextra
        -Wformat=2
        -Wpointer-arith
        -Wredundant-decls
    )
endif()

# ── Relaxed warnings for vendored code ───────────────────
if(MSVC)
    set(VENDORED_WARN_FLAGS "/W0")
else()
    set(VENDORED_WARN_FLAGS "-w")
endif()

# ImGui sources: compile with minimal warnings
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES
    COMPILE_FLAGS "${VENDORED_WARN_FLAGS}"
)

# Vendored TextEditor (now under vendor/ImGuiColorTextEdit — see
# VENDOR_TEXTEDITOR_SOURCES above)
set_source_files_properties(${VENDOR_TEXTEDITOR_SOURCES} PROPERTIES
    COMPILE_FLAGS "${VENDORED_WARN_FLAGS}"
)

# ── Main executable ──────────────────────────────────────
add_executable(koncepcja main.cpp)
target_link_libraries(koncepcja PRIVATE koncepcja_lib)

if(WIN32)
    # Link as a Windows GUI app (no console window) for Release
    set_target_properties(koncepcja PROPERTIES
        WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    )
endif()

# ── Test executable ──────────────────────────────────────
# googletest: use local directory if present, otherwise fetch from GitHub
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(BUILD_GMOCK ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
if(EXISTS "${CMAKE_SOURCE_DIR}/googletest/CMakeLists.txt")
    add_subdirectory(googletest EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(googletest)
endif()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS test/*.cpp)

add_executable(test_runner ${TEST_SOURCES})
target_include_directories(test_runner PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/test
)
target_link_libraries(test_runner PRIVATE
    koncepcja_lib
    gtest
    gmock
)

if(MSVC)
    # Relax warnings for test code
    target_compile_options(test_runner PRIVATE /W3 /wd4201 /wd4244 /wd4267 /wd4100)
endif()

# Register tests with CTest
include(CTest)
add_test(NAME unit_tests COMMAND test_runner --gtest_shuffle)
