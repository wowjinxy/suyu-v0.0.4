# SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

if(NOT DEFINED RECOMP_TOOL OR NOT DEFINED NSO_TEST_HELPER OR NOT DEFINED NSO_TEST_DIR)
    message(FATAL_ERROR "NSO inspect test is missing a required path")
endif()

file(REMOVE_RECURSE "${NSO_TEST_DIR}")
file(MAKE_DIRECTORY "${NSO_TEST_DIR}")
set(NSO_FIXTURE "${NSO_TEST_DIR}/synthetic.nso")

execute_process(
    COMMAND "${NSO_TEST_HELPER}" --write-fixture "${NSO_FIXTURE}"
    RESULT_VARIABLE FIXTURE_RESULT
    OUTPUT_VARIABLE FIXTURE_OUTPUT
    ERROR_VARIABLE FIXTURE_ERROR
)
if(NOT FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not create synthetic NSO: ${FIXTURE_OUTPUT}${FIXTURE_ERROR}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${NSO_FIXTURE}" --assume-aarch64
    RESULT_VARIABLE INSPECT_RESULT
    OUTPUT_VARIABLE INSPECT_OUTPUT
    ERROR_VARIABLE INSPECT_ERROR
)
if(NOT INSPECT_RESULT EQUAL 0)
    message(FATAL_ERROR "Human-readable NSO inspection failed: ${INSPECT_OUTPUT}${INSPECT_ERROR}")
endif()
foreach(EXPECTED
        "Format: NSO0"
        "Architecture: AArch64 (explicit assumption)"
        "Build ID: 000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
        "text: file=0x100 memory=0x1000 decoded=16 stored=16 compression=none"
        "AArch64 blocks: 1")
    string(FIND "${INSPECT_OUTPUT}" "${EXPECTED}" EXPECTED_POSITION)
    if(EXPECTED_POSITION EQUAL -1)
        message(FATAL_ERROR "Inspection output is missing '${EXPECTED}':\n${INSPECT_OUTPUT}")
    endif()
endforeach()

set(LZ4_NSO_FIXTURE "${NSO_TEST_DIR}/synthetic-lz4.nso")
execute_process(
    COMMAND "${NSO_TEST_HELPER}" --write-lz4-fixture "${LZ4_NSO_FIXTURE}"
    RESULT_VARIABLE LZ4_FIXTURE_RESULT
    ERROR_VARIABLE LZ4_FIXTURE_ERROR
)
if(NOT LZ4_FIXTURE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not create LZ4 NSO: ${LZ4_FIXTURE_ERROR}")
endif()
execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${LZ4_NSO_FIXTURE}"
    RESULT_VARIABLE LZ4_INSPECT_RESULT
    OUTPUT_VARIABLE LZ4_INSPECT_OUTPUT
    ERROR_VARIABLE LZ4_INSPECT_ERROR
)
if(NOT LZ4_INSPECT_RESULT EQUAL 0)
    message(FATAL_ERROR "LZ4 NSO inspection failed: ${LZ4_INSPECT_OUTPUT}${LZ4_INSPECT_ERROR}")
endif()
string(FIND "${LZ4_INSPECT_OUTPUT}" "compression=lz4" LZ4_METADATA_POSITION)
if(LZ4_METADATA_POSITION EQUAL -1)
    message(FATAL_ERROR "LZ4 metadata was not reported:\n${LZ4_INSPECT_OUTPUT}")
endif()
string(FIND "${LZ4_INSPECT_OUTPUT}" "Segments decoded: yes" LZ4_DECODED_POSITION)
string(FIND "${LZ4_INSPECT_OUTPUT}" "requires an LZ4 decompressor" LZ4_UNAVAILABLE_POSITION)
if(LZ4_DECODED_POSITION EQUAL -1 AND LZ4_UNAVAILABLE_POSITION EQUAL -1)
    message(FATAL_ERROR "LZ4 decoding was neither successful nor clearly unavailable:\n${LZ4_INSPECT_OUTPUT}")
endif()

execute_process(
    COMMAND "${RECOMP_TOOL}" inspect-nso --input "${NSO_FIXTURE}" --json
    RESULT_VARIABLE JSON_RESULT
    OUTPUT_VARIABLE JSON_OUTPUT
    ERROR_VARIABLE JSON_ERROR
)
if(NOT JSON_RESULT EQUAL 0)
    message(FATAL_ERROR "JSON NSO inspection failed: ${JSON_OUTPUT}${JSON_ERROR}")
endif()
foreach(EXPECTED
        "\"format\": \"NSO0\""
        "\"architecture\": null"
        "\"build_id\": \"000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f\""
        "\"decoded_size\": 16")
    string(FIND "${JSON_OUTPUT}" "${EXPECTED}" EXPECTED_POSITION)
    if(EXPECTED_POSITION EQUAL -1)
        message(FATAL_ERROR "JSON output is missing '${EXPECTED}':\n${JSON_OUTPUT}")
    endif()
endforeach()
