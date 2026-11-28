/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * libmybench PPK2 device: the protocol core (ppk2.h) on a serial port (serial.h). Blocking calls,
 * one thread at a time. Opening stops a stream left running by another program, drains the
 * port, reads the metadata and sets up the calibration; closing always stops the stream and
 * disconnects the DUT. The safe power order of ppk2_ctl applies to every call.
 *
 * Not tested on hardware yet: which of the PPK2's two serial ports is the measurement port is
 * unknown (drivers/ppk2/CLEANROOM.md, open question 1), so ppk2_dev_find reports both with their
 * USB interface numbers and the caller picks one.
 */
#ifndef LIBMYBENCH_PPK2_DEV_H
#define LIBMYBENCH_PPK2_DEV_H

#include "libmybench/ppk2.h"
#include "libmybench/serial.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes in addition to ppk2_status, for opening a port. */
#define PPK2_ERR_NOT_FOUND (-10) /**< no such port */
#define PPK2_ERR_ACCESS (-11)    /**< permission denied */
#define PPK2_ERR_BUSY (-12)      /**< another program has the port open (nRF Connect?) */

/** Description of any status these functions return (ppk2_strerror plus the three above). */
LMB_API const char *ppk2_dev_strerror(int code);

/**
 * Serial ports whose USB IDs are the PPK2's (1915:C00A), sorted by path: up to cap into
 * ports, *count = number found. Each PPK2 shows two ports (measurement and console).
 */
LMB_API int ppk2_dev_find(lmb_serial_port *ports, size_t cap, size_t *count);

typedef struct ppk2_dev ppk2_dev;

/**
 * Open the measurement port at path, with a cap on the source voltage (0 = 5000 mV).
 * PPK2_ERR_TIMEOUT if no metadata reply arrives (wrong port, or not a PPK2).
 */
LMB_API int ppk2_dev_open(const char *path, unsigned cap_mv, ppk2_dev **out);
/** Stop, disconnect the DUT, close. Never fails; NULL is fine. */
LMB_API void ppk2_dev_close(ppk2_dev *d);

LMB_API const ppk2_metadata *ppk2_dev_metadata(const ppk2_dev *d);
LMB_API const ppk2_cal *ppk2_dev_calibration(const ppk2_dev *d);
/** The command state: supply, gate, sampling, voltage setting. */
LMB_API const ppk2_ctl *ppk2_dev_ctl(const ppk2_dev *d);

LMB_API int ppk2_dev_select_supply(ppk2_dev *d, ppk2_supply supply);
LMB_API int ppk2_dev_set_voltage(ppk2_dev *d, unsigned mv, unsigned *applied_mv);
/** Connect (on != 0) or disconnect the DUT. */
LMB_API int ppk2_dev_set_power(ppk2_dev *d, int on);

/** Start sampling; current is computed with the voltage setting at this moment. */
LMB_API int ppk2_dev_start(ppk2_dev *d);
/**
 * Decode what arrived into at most cap samples, waiting up to timeout_ms for the first bytes.
 * *got is 0 after a timeout. Call often: at 100 kS/s the port delivers 400 kB/s.
 */
LMB_API int ppk2_dev_read(ppk2_dev *d, ppk2_sample *out, size_t cap, int timeout_ms, size_t *got);
/** Stop sampling and drain the port. The DUT stays as it is. */
LMB_API int ppk2_dev_stop(ppk2_dev *d);
LMB_API void ppk2_dev_stats(const ppk2_dev *d, ppk2_stream_stats *stats);

#ifdef __cplusplus
}
#endif

#endif
