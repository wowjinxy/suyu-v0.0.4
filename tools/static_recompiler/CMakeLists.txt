# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.22)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(suyu_recompiler_tool LANGUAGES C CXX)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "suyu-recomp requires a 64-bit host toolchain")
endif()

add_executable(suyu_recomp suyu_recomp.cpp)
target_compile_features(suyu_recomp PRIVATE cxx_std_20)
target_include_directories(suyu_recomp PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")
if(WIN32)
    target_link_libraries(suyu_recomp PRIVATE shell32)
endif()

if(MSVC)
    target_compile_options(suyu_recomp PRIVATE /W4 /permissive-)
else()
    target_compile_options(suyu_recomp PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    add_test(
        NAME static_recompiler_smoke
        COMMAND "${CMAKE_COMMAND}"
            "-DRECOMP_TOOL=$<TARGET_FILE:suyu_recomp>"
            "-DRECOMP_FIXTURE=${CMAKE_CURRENT_LIST_DIR}/test.bin"
            "-DRECOMP_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/smoke"
            "-DRECOMP_GENERATOR=${CMAKE_GENERATOR}"
            "-DRECOMP_C_COMPILER=${CMAKE_C_COMPILER}"
            "-P" "${CMAKE_CURRENT_LIST_DIR}/tests/smoke.cmake"
    )
    set_tests_properties(static_recompiler_smoke PROPERTIES TIMEOUT 120)
endif()
