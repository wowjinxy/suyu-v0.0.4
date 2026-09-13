# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

foreach(required RECOMP_TOOL RECOMP_FIXTURE RECOMP_TEST_DIR RECOMP_GENERATOR RECOMP_C_COMPILER)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} was not provided")
    endif()
endforeach()

if(NOT RECOMP_TEST_DIR MATCHES "[/\\\\]smoke$")
    message(FATAL_ERROR "Refusing to clean an unexpected test directory: ${RECOMP_TEST_DIR}")
endif()
file(REMOVE_RECURSE "${RECOMP_TEST_DIR}")

set(generated_dir "${RECOMP_TEST_DIR}/generated-Pokémon-漢")
set(build_dir "${RECOMP_TEST_DIR}/build")

execute_process(
    COMMAND "${RECOMP_TOOL}"
        emit-raw
        --input "${RECOMP_FIXTURE}"
        --base 0x1000
        --entry 0x1000
        --root 0x1004
        --module smoke
        --title "Pokémon 漢: Static \"Recompiler\"??/n Smoke Test"
        --output "${generated_dir}"
    RESULT_VARIABLE emit_result
    OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr
)
if(NOT emit_result EQUAL 0)
    message(FATAL_ERROR "Emitter failed (${emit_result})\n${emit_stdout}\n${emit_stderr}")
endif()
if(NOT emit_stdout MATCHES
   "Generated 2 blocks from 4 AArch64 instructions \\(1 translated terminators\\).")
    message(FATAL_ERROR "Unexpected emitter statistics:\n${emit_stdout}")
endif()

foreach(generated_file
        CMakeLists.txt
        recomp_runtime.c
        recomp_runtime.h
        recomp_export.c
        main.c
        data/text.bin
        src/recompiled_smoke.c
        src/recompiled_smoke_0.c)
    if(NOT EXISTS "${generated_dir}/${generated_file}")
        message(FATAL_ERROR "Emitter did not create ${generated_file}")
    endif()
endforeach()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${RECOMP_FIXTURE}" "${generated_dir}/data/text.bin"
    RESULT_VARIABLE compare_result
)
if(NOT compare_result EQUAL 0)
    message(FATAL_ERROR "Generated text segment differs from the input fixture")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${generated_dir}"
        -B "${build_dir}"
        -G "${RECOMP_GENERATOR}"
        "-DCMAKE_C_COMPILER=${RECOMP_C_COMPILER}"
        -DCMAKE_DISABLE_FIND_PACKAGE_SDL2=ON
        -DCMAKE_BUILD_TYPE=Release
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Generated project configure failed (${configure_result})\n${configure_stdout}\n${configure_stderr}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config Release --target recompiled
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Generated project build failed (${build_result})\n${build_stdout}\n${build_stderr}")
endif()

set(executable_candidates
    "${build_dir}/recompiled${CMAKE_EXECUTABLE_SUFFIX}"
    "${build_dir}/Release/recompiled${CMAKE_EXECUTABLE_SUFFIX}"
    "${build_dir}/recompiled.exe"
    "${build_dir}/Release/recompiled.exe")
unset(recompiled_executable)
foreach(candidate IN LISTS executable_candidates)
    if(EXISTS "${candidate}")
        set(recompiled_executable "${candidate}")
        break()
    endif()
endforeach()
if(NOT DEFINED recompiled_executable)
    message(FATAL_ERROR "Could not locate the generated recompiled executable")
endif()

execute_process(
    COMMAND "${recompiled_executable}" --no-save
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "Generated program failed (${run_result})\n${run_stdout}\n${run_stderr}")
endif()

set(run_output "${run_stdout}\n${run_stderr}")
if(NOT run_output MATCHES "Loaded segment text.bin \\(16 bytes\\)")
    message(FATAL_ERROR "Generated runner did not load its bundled text segment:\n${run_output}")
endif()
string(FIND "${run_stdout}" "Pokémon 漢: Static \"Recompiler\"??/n Smoke Test" title_position)
if(title_position EQUAL -1)
    message(FATAL_ERROR "Generated title was not preserved literally:\n${run_stdout}")
endif()
if(NOT run_output MATCHES "Unhandled SVC #0x0 x0=0x5 x1=0x7 x2=0xc x3=0x0")
    message(FATAL_ERROR "Unexpected generated program output:\n${run_output}")
endif()
if(EXISTS "${build_dir}/save_data/autosave.bin" OR
   EXISTS "${build_dir}/Release/save_data/autosave.bin")
    message(FATAL_ERROR "--no-save unexpectedly created an autosave")
endif()

message(STATUS "Static recompiler smoke test passed\n${emit_stdout}${run_output}")
