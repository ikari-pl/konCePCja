cmake_minimum_required(VERSION 3.21)
project(koncepcja VERSION 5.7.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Options ──────────────────────────────────────────────
option(WITH_CURL  "Enable libcurl (M4 HTTP)"  OFF)
option(WITH_JPEG  "Enable libjpeg (AVI rec)"  OFF)

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

# ── Vendored ImGui sources ───────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlgpu3.cpp
)

# ── Capsimg include directories ──────────────────────────
set(CAPSIMG_INCLUDES
    ${CMAKE_SOURCE_DIR}/src/capsimg/LibIPF
    ${CMAKE_SOURCE_DIR}/src/capsimg/Device
    ${CMAKE_SOURCE_DIR}/src/capsimg/CAPSImg
    ${CMAKE_SOURCE_DIR}/src/capsimg/Codec
    ${CMAKE_SOURCE_DIR}/src/capsimg/Core
)

# ── Shared source library (used by both main exe and tests) ──
add_library(koncepcja_lib STATIC
    ${APP_SOURCES}
    ${IMGUI_SOURCES}
)

target_include_directories(koncepcja_lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${CAPSIMG_INCLUDES}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
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

# Vendored TextEditor (in src/ but third-party)
set(VENDORED_TEXTEDITOR_SOURCES
    ${CMAKE_SOURCE_DIR}/src/TextEditor.cpp
    ${CMAKE_SOURCE_DIR}/src/LanguageDefinitions.cpp
)
set_source_files_properties(${VENDORED_TEXTEDITOR_SOURCES} PROPERTIES
    COMPILE_FLAGS "${VENDORED_WARN_FLAGS}"
)

# Capsimg sources: compile with relaxed warnings
file(GLOB_RECURSE CAPSIMG_SOURCES CONFIGURE_DEPENDS
    src/capsimg/*.cpp
    src/capsimg/*.c
)
set_source_files_properties(${CAPSIMG_SOURCES} PROPERTIES
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
