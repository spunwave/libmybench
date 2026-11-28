/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * libmybench serial transport: list serial ports with their USB identity, open one raw (8N1,
 * no flow control, DTR as asked), and move bytes with timeouts. Linux (sysfs + termios),
 * macOS (IOKit + termios) and Windows (SetupAPI + Win32 comm API). Other POSIX systems can
 * open ports by path but list none.
 *
 * Errors: functions returning int return LMB_SERIAL_OK (0) or a negative lmb_serial_status.
 * Threading: a port is used from one thread at a time; listing is thread-safe.
 */
#ifndef LIBMYBENCH_SERIAL_H
#define LIBMYBENCH_SERIAL_H

#include <stddef.h>
#include <stdint.h>

#include "libmybench/export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum lmb_serial_status {
    LMB_SERIAL_OK = 0,
    LMB_SERIAL_ERR_INVALID_ARG = -1,
    LMB_SERIAL_ERR_NOT_FOUND = -2,    /**< no such port */
    LMB_SERIAL_ERR_ACCESS = -3,       /**< permission denied (Linux: udev rule / dialout group) */
    LMB_SERIAL_ERR_BUSY = -4,         /**< another program has the port open */
    LMB_SERIAL_ERR_IO = -5,           /**< any other system error */
    LMB_SERIAL_ERR_DISCONNECTED = -6, /**< the device went away */
    LMB_SERIAL_ERR_NO_MEM = -7,
    LMB_SERIAL_ERR_UNSUPPORTED = -8   /**< not available on this system */
} lmb_serial_status;

LMB_API const char *lmb_serial_strerror(int code);

typedef struct lmb_serial_port {
    char path[256];          /**< what lmb_serial_open takes: /dev/ttyACM0, /dev/cu.usbmodem..., COM5 */
    uint16_t usb_vid;        /**< 0 when the port is not on USB or the IDs are unknown */
    uint16_t usb_pid;
    int usb_interface;       /**< USB interface number of the port, -1 when unknown */
    char serial_number[64];  /**< USB serial number of the device, "" when unknown */
    char description[128];   /**< product or friendly name, "" when unknown */
} lmb_serial_port;

/**
 * List the USB serial ports present now, sorted by path. Writes up to cap entries; *count gets
 * the number found (may exceed cap). ports may be NULL when cap is 0.
 */
LMB_API int lmb_serial_list(lmb_serial_port *ports, size_t cap, size_t *count);

typedef struct lmb_serial lmb_serial;

/**
 * Open a port raw at baud (a standard rate) with DTR asserted (dtr != 0) or cleared, RTS
 * asserted. On POSIX the port is locked against other programs that lock it too (flock), and
 * a pseudo-terminal, which has no modem lines, is accepted without DTR.
 */
LMB_API int lmb_serial_open(const char *path, unsigned baud, int dtr, lmb_serial **out);
LMB_API void lmb_serial_close(lmb_serial *s);

/** Write all len bytes (one system write when the driver takes them at once). */
LMB_API int lmb_serial_write(lmb_serial *s, const void *data, size_t len);
/**
 * Read what is available, waiting up to timeout_ms (0: don't wait, < 0: wait forever) for the
 * first byte. *got is 0 after a timeout.
 */
LMB_API int lmb_serial_read(lmb_serial *s, void *buf, size_t cap, int timeout_ms, size_t *got);
/** Discard bytes received but not read yet. */
LMB_API int lmb_serial_flush_input(lmb_serial *s);

#ifdef __cplusplus
}
#endif

#endif
