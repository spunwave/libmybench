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

# The same capture into a .ppk2 file, then read back: 20000 decoded + 6 lost = 20006 records.
execute_process(COMMAND ${TOOL} decode --ppk2 ${WORK}/roundtrip.ppk2 ${WORK}/roundtrip.bin
    ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT err MATCHES "wrote .*roundtrip.ppk2: 20006 records")
    message(FATAL_ERROR "decode --ppk2 failed: ${rc}\n${err}")
endif()
execute_process(COMMAND ${TOOL} info ${WORK}/roundtrip.ppk2 OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT out MATCHES "records: 20006 \\(0.200 s\\)" OR NOT out MATCHES "\\(20000 finite values\\)")
    message(FATAL_ERROR "info failed: ${rc}\n${out}")
endif()
# 1 mA with a 10 % ripple: the mean is close to 1000 uA.
if(NOT out MATCHES "current: mean (99[0-9]|100[0-9])\\.")
    message(FATAL_ERROR "unexpected mean current:\n${out}")
endif()
execute_process(COMMAND ${TOOL} export ${WORK}/roundtrip.ppk2 OUTPUT_FILE ${WORK}/roundtrip-export.csv RESULT_VARIABLE rc)
file(STRINGS ${WORK}/roundtrip-export.csv lines LIMIT_COUNT 2)
list(GET lines 1 first)
if(NOT rc EQUAL 0 OR NOT first MATCHES "^0,0.000000,[0-9.]+e-0[34],")
    message(FATAL_ERROR "export failed: ${rc}\n${first}")
endif()
