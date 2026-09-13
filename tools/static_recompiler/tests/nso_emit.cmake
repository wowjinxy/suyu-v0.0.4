# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

foreach(required RECOMP_TOOL NSO_TEST_HELPER NSO_EMIT_TEST_DIR RECOMP_GENERATOR RECOMP_C_COMPILER)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} was not provided")
    endif()
endforeach()

if(NOT NSO_EMIT_TEST_DIR MATCHES "[/\\\\]nso_emit$")
    message(FATAL_ERROR "Refusing to clean an unexpected test directory: ${NSO_EMIT_TEST_DIR}")
endif()
file(REMOVE_RECURSE "${NSO_EMIT_TEST_DIR}")
file(MAKE_DIRECTORY "${NSO_EMIT_TEST_DIR}")

set(nso_fixture "${NSO_EMIT_TEST_DIR}/synthetic.nso")
set(misaligned_nso_fixture "${NSO_EMIT_TEST_DIR}/misaligned.nso")
set(reversed_nso_fixture "${NSO_EMIT_TEST_DIR}/reversed.nso")
set(npdm_fixture "${NSO_EMIT_TEST_DIR}/main.npdm")
set(npdm32_fixture "${NSO_EMIT_TEST_DIR}/main32.npdm")
set(npdm_unknown_fixture "${NSO_EMIT_TEST_DIR}/main-unknown-address.npdm")
set(npdm_oversized_fixture "${NSO_EMIT_TEST_DIR}/main-oversized.npdm")
foreach(pair
        "--write-emittable-fixture;${nso_fixture}"
        "--write-misaligned-emittable-fixture;${misaligned_nso_fixture}"
        "--write-reversed-emittable-fixture;${reversed_nso_fixture}"
        "--write-npdm;${npdm_fixture}"
        "--write-aarch32-npdm;${npdm32_fixture}"
        "--write-unknown-address-npdm;${npdm_unknown_fixture}"
        "--write-oversized-npdm;${npdm_oversized_fixture}")
    list(GET pair 0 operation)
    list(GET pair 1 path)
    execute_process(
        COMMAND "${NSO_TEST_HELPER}" "${operation}" "${path}"
        RESULT_VARIABLE fixture_result
        OUTPUT_VARIABLE fixture_stdout
        ERROR_VARIABLE fixture_stderr
    )
    if(NOT fixture_result EQUAL 0)
        message(FATAL_ERROR
            "Fixture writer failed (${fixture_result})\n${fixture_stdout}\n${fixture_stderr}")
    endif()
endforeach()

execute_process(
    COMMAND "${RECOMP_TOOL}" emit-nso
        --input "${nso_fixture}"
        --output "${NSO_EMIT_TEST_DIR}/missing-architecture"
    RESULT_VARIABLE missing_arch_result
    OUTPUT_VARIABLE missing_arch_stdout
    ERROR_VARIABLE missing_arch_stderr
)
if(missing_arch_result EQUAL 0 OR
   NOT missing_arch_stderr MATCHES "requires exactly one of --npdm or --assume-aarch64")
    message(FATAL_ERROR
        "emit-nso did not require an architecture source:\n${missing_arch_stdout}${missing_arch_stderr}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" emit-nso
        --input "${nso_fixture}"
        --npdm "${npdm_fixture}"
        --assume-aarch64
        --output "${NSO_EMIT_TEST_DIR}/conflicting-architecture"
    RESULT_VARIABLE conflicting_arch_result
    OUTPUT_VARIABLE conflicting_arch_stdout
    ERROR_VARIABLE conflicting_arch_stderr
)
if(conflicting_arch_result EQUAL 0 OR
   NOT conflicting_arch_stderr MATCHES "requires exactly one of --npdm or --assume-aarch64")
    message(FATAL_ERROR
        "emit-nso accepted two architecture sources:\n${conflicting_arch_stdout}${conflicting_arch_stderr}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" emit-nso
        --input "${nso_fixture}"
        --npdm "${npdm32_fixture}"
        --output "${NSO_EMIT_TEST_DIR}/aarch32-output"
    RESULT_VARIABLE aarch32_result
    OUTPUT_VARIABLE aarch32_stdout
    ERROR_VARIABLE aarch32_stderr
)
if(aarch32_result EQUAL 0 OR NOT aarch32_stderr MATCHES "declares AArch32")
    message(FATAL_ERROR
        "emit-nso did not reject AArch32:\n${aarch32_stdout}${aarch32_stderr}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" emit-nso
        --input "${nso_fixture}"
        --npdm "${npdm_unknown_fixture}"
        --output "${NSO_EMIT_TEST_DIR}/unknown-address-output"
    RESULT_VARIABLE unknown_address_result
    OUTPUT_VARIABLE unknown_address_stdout
    ERROR_VARIABLE unknown_address_stderr
)
if(unknown_address_result EQUAL 0 OR
   NOT unknown_address_stderr MATCHES "unsupported process address-space")
    message(FATAL_ERROR
        "emit-nso accepted an unknown address space:\n${unknown_address_stdout}${unknown_address_stderr}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" emit-nso
        --input "${nso_fixture}"
        --npdm "${npdm_oversized_fixture}"
        --output "${NSO_EMIT_TEST_DIR}/oversized-npdm-output"
    RESULT_VARIABLE oversized_npdm_result
    OUTPUT_VARIABLE oversized_npdm_stdout
    ERROR_VARIABLE oversized_npdm_stderr
)
if(oversized_npdm_result EQUAL 0 OR
   NOT oversized_npdm_stderr MATCHES "exceeds maximum size of 32768 bytes")
    message(FATAL_ERROR
        "emit-nso did not reject NPDM before allocation:\n${oversized_npdm_stdout}${oversized_npdm_stderr}")
endif()

foreach(pair
        "${misaligned_nso_fixture};not 0x1000-aligned"
        "${reversed_nso_fixture};rodata segment ends after the data segment begins")
    list(GET pair 0 fixture)
    list(GET pair 1 expected_error)
    execute_process(
        COMMAND "${RECOMP_TOOL}" emit-nso
            --input "${fixture}"
            --npdm "${npdm_fixture}"
            --output "${NSO_EMIT_TEST_DIR}/invalid-layout-output"
        RESULT_VARIABLE invalid_layout_result
        OUTPUT_VARIABLE invalid_layout_stdout
        ERROR_VARIABLE invalid_layout_stderr
    )
    if(invalid_layout_result EQUAL 0 OR NOT invalid_layout_stderr MATCHES "${expected_error}")
        message(FATAL_ERROR
            "emit-nso accepted an invalid load layout:\n${invalid_layout_stdout}${invalid_layout_stderr}")
    endif()
endforeach()

set(assumed_dir "${NSO_EMIT_TEST_DIR}/assumed-aarch64")
execute_process(
    COMMAND "${RECOMP_TOOL}" emit-nso
        --input "${nso_fixture}"
        --assume-aarch64
        --output "${assumed_dir}"
    RESULT_VARIABLE assumed_result
    OUTPUT_VARIABLE assumed_stdout
    ERROR_VARIABLE assumed_stderr
)
if(NOT assumed_result EQUAL 0 OR
   NOT assumed_stderr MATCHES "treating NSO instructions as AArch64 without main.npdm" OR
   NOT EXISTS "${assumed_dir}/CMakeLists.txt")
    message(FATAL_ERROR
        "Explicit AArch64 assumption did not emit successfully:\n${assumed_stdout}${assumed_stderr}")
endif()

set(generated_dir "${NSO_EMIT_TEST_DIR}/generated-nso")
set(build_dir "${NSO_EMIT_TEST_DIR}/build")
execute_process(
    COMMAND "${RECOMP_TOOL}" emit-nso
        --input "${nso_fixture}"
        --npdm "${npdm_fixture}"
        --module main
        --title "Synthetic NSO emission test"
        --output "${generated_dir}"
    RESULT_VARIABLE emit_result
    OUTPUT_VARIABLE emit_stdout
    ERROR_VARIABLE emit_stderr
)
if(NOT emit_result EQUAL 0)
    message(FATAL_ERROR "NSO emitter failed (${emit_result})\n${emit_stdout}\n${emit_stderr}")
endif()
string(FIND "${emit_stdout}"
    "Generated 3 blocks from 26 AArch64 instructions (2 translated terminators)."
    statistics_position)
if(statistics_position EQUAL -1)
    message(FATAL_ERROR "Unexpected NSO emitter statistics:\n${emit_stdout}")
endif()
foreach(expected
        "Architecture: AArch64 (main.npdm)"
        "Entry: 0x1040"
        "NSO build ID: 808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f")
    string(FIND "${emit_stdout}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "NSO emitter output is missing '${expected}':\n${emit_stdout}")
    endif()
endforeach()

file(READ "${generated_dir}/data/rodata.bin" rodata_hex HEX)
file(READ "${generated_dir}/data/data.bin" data_hex HEX)
if(NOT rodata_hex STREQUAL "0500000000000000" OR NOT data_hex STREQUAL "0700000000000000")
    message(FATAL_ERROR "Decoded NSO data segments were not preserved")
endif()
file(READ "${generated_dir}/main.c" generated_main)
string(FIND "${generated_main}"
    "0x1000ULL,0x68ULL,0x3000ULL,0x8ULL,0x5000ULL,0x8ULL,0x10ULL" layout_position)
if(layout_position EQUAL -1)
    message(FATAL_ERROR "Generated runner does not embed the exact NSO segment layout")
endif()
string(FIND "${generated_main}" "GUEST_MEM_SIZE > (uint64_t)(size_t)-1" host_size_position)
if(host_size_position EQUAL -1)
    message(FATAL_ERROR "Generated runner does not guard host-size memory allocation")
endif()
file(READ "${generated_dir}/CMakeLists.txt" generated_cmake)
string(FIND "${generated_cmake}" "CMAKE_SIZEOF_VOID_P EQUAL 8" host_width_position)
if(host_width_position EQUAL -1)
    message(FATAL_ERROR "Generated project does not require a 64-bit host")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" emit-nso
        --input "${nso_fixture}"
        --npdm "${npdm_fixture}"
        --output "${generated_dir}"
    RESULT_VARIABLE nonempty_result
    OUTPUT_VARIABLE nonempty_stdout
    ERROR_VARIABLE nonempty_stderr
)
if(nonempty_result EQUAL 0 OR NOT nonempty_stderr MATCHES "output directory is not empty")
    message(FATAL_ERROR
        "emit-nso overwrote a non-empty directory without --force:\n${nonempty_stdout}${nonempty_stderr}")
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
        "Generated NSO project configure failed (${configure_result})\n${configure_stdout}\n${configure_stderr}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config Release --target recompiled
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Generated NSO project build failed (${build_result})\n${build_stdout}\n${build_stderr}")
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
    message(FATAL_ERROR "Could not locate the generated NSO executable")
endif()
get_filename_component(executable_dir "${recompiled_executable}" DIRECTORY)

execute_process(
    COMMAND "${recompiled_executable}" --no-save
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_stdout
    ERROR_VARIABLE run_stderr
)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "Generated NSO program failed (${run_result})\n${run_stdout}\n${run_stderr}")
endif()
set(run_output "${run_stdout}\n${run_stderr}")
foreach(expected
        "Loaded segment text.bin (104 bytes) at 0x1000"
        "Loaded segment rodata.bin (8 bytes) at 0x3000"
        "Loaded segment data.bin (8 bytes) at 0x5000"
        "x0=0x5 x1=0x7 x2=0xc x3=0x9")
    string(FIND "${run_output}" "${expected}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Generated NSO output is missing '${expected}':\n${run_output}")
    endif()
endforeach()
if(EXISTS "${executable_dir}/save_data/autosave.bin")
    message(FATAL_ERROR "--no-save unexpectedly created an autosave")
endif()

execute_process(
    COMMAND "${recompiled_executable}"
    RESULT_VARIABLE save_enabled_result
    OUTPUT_VARIABLE save_enabled_stdout
    ERROR_VARIABLE save_enabled_stderr
)
if(NOT save_enabled_result EQUAL 0)
    message(FATAL_ERROR
        "Generated NSO program failed with save data enabled (${save_enabled_result})\n"
        "${save_enabled_stdout}\n${save_enabled_stderr}")
endif()
set(save_enabled_output "${save_enabled_stdout}\n${save_enabled_stderr}")
string(FIND "${save_enabled_output}" "x0=0x5 x1=0x7 x2=0xc x3=0x9" save_state_position)
if(save_state_position EQUAL -1 OR EXISTS "${executable_dir}/save_data/autosave.bin")
    message(FATAL_ERROR
        "Save-data initialization replaced the NSO image or wrote a process snapshot:\n"
        "${save_enabled_output}")
endif()

file(APPEND "${executable_dir}/data/data.bin" "X")
execute_process(
    COMMAND "${recompiled_executable}" --no-save
    RESULT_VARIABLE corrupt_result
    OUTPUT_VARIABLE corrupt_stdout
    ERROR_VARIABLE corrupt_stderr
)
if(corrupt_result EQUAL 0 OR NOT corrupt_stderr MATCHES "has 9 bytes; expected 8")
    message(FATAL_ERROR
        "Generated loader accepted a mismatched segment:\n${corrupt_stdout}${corrupt_stderr}")
endif()

message(STATUS "NSO emission test passed\n${emit_stdout}${run_output}")
