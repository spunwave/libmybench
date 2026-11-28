# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez
# lmb-tool through the generic API only: list, info, and half a second of the simulated PPK2.
# Nothing is written to disk, so it can run beside the other tests.
execute_process(COMMAND ${TOOL} list --sim-only OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT out MATCHES "sim:ppk2\tppk2\tSimulated PPK2\tsim\t[^\t]*\tpower-supply,current-stream,logic-input")
    message(FATAL_ERROR "list failed: ${rc}\n${out}")
endif()
execute_process(COMMAND ${TOOL} info --device sim:ppk2 OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT out MATCHES "current-stream: 100000 S/s" OR NOT out MATCHES "logic-input: 8 lines")
    message(FATAL_ERROR "info failed: ${rc}\n${out}")
endif()
execute_process(COMMAND ${TOOL} stream --device sim:ppk2 --seconds 0.5 --power
    OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT out MATCHES "samples: 50000 " OR NOT out MATCHES "gaps: 0, lost: 0, invalid: 0")
    message(FATAL_ERROR "stream failed: ${rc}\n${out}\n${err}")
endif()
# The simulator's default load is 1 mA.
if(NOT out MATCHES "current: mean (9\\.9[0-9]+|1\\.00[0-9]+)e-0[34] A")
    message(FATAL_ERROR "unexpected mean current:\n${out}")
endif()
execute_process(COMMAND ${TOOL} info --device nope:0 RESULT_VARIABLE rc ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "no such device")
    message(FATAL_ERROR "an unknown device must fail with a message: ${rc}\n${err}")
endif()
