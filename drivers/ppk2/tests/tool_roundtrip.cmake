# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez
# ppk2-tool sim -> decode: 0.2 s at 1 mA with 70 samples lost halfway.
execute_process(COMMAND ${TOOL} sim --seconds 0.2 --amps 0.001 --mv 3300 --drop 70
    OUTPUT_FILE ${WORK}/roundtrip.bin RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "sim failed: ${rc}")
endif()
execute_process(COMMAND ${TOOL} decode ${WORK}/roundtrip.bin
    OUTPUT_FILE ${WORK}/roundtrip.csv ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "decode failed: ${rc}\n${err}")
endif()
if(NOT err MATCHES "summary: words=20000 gaps=1 missing=6 bad_range=0 trailing_bytes=0")
    message(FATAL_ERROR "unexpected summary:\n${err}")
endif()
if(NOT err MATCHES "every coefficient from the unit")
    message(FATAL_ERROR "calibration not read from the metadata:\n${err}")
endif()
