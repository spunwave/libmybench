# SPDX-License-Identifier: MPL-2.0
# SPDX-FileCopyrightText: 2026 Martin Dolez

if(WIN32)
    set(_serial_platform src/serial_win32.c)
else()
    set(_serial_platform src/serial_posix.c)
endif()
libmybench_module(serial SOURCES src/serial_common.c ${_serial_platform})
if(WIN32)
    target_link_libraries(libmybench_serial PRIVATE setupapi cfgmgr32)
elseif(APPLE)
    target_link_libraries(libmybench_serial PRIVATE "-framework IOKit" "-framework CoreFoundation")
endif()
