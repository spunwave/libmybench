/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* PPK2 device on a serial port (report: "Measurement connection"): 115200 baud nominal, DTR
 * asserted, one write per command, no acknowledgements. */

#include "libmybench/ppk2_dev.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BAUD 115200u
#define DRAIN_QUIET_MS 60    /* the port is drained once nothing arrived for this long */
#define DRAIN_MAX_MS 2000    /* give up on a port that never goes quiet */
#define METADATA_TIMEOUT_MS 3000

struct ppk2_dev {
    lmb_serial *port;
    ppk2_ctl *ctl;
    ppk2_stream *stream;
    ppk2_metadata meta;
    ppk2_cal cal;
    uint8_t buf[16384];
    size_t pending;   /* bytes in buf not decoded yet (a full sample buffer stopped the decoder) */
    size_t pending_at;
};

static int from_serial(int rc)
{
    switch (rc) {
    case LMB_SERIAL_OK: return PPK2_OK;
    case LMB_SERIAL_ERR_INVALID_ARG: return PPK2_ERR_INVALID_ARG;
    case LMB_SERIAL_ERR_NOT_FOUND: return PPK2_ERR_NOT_FOUND;
    case LMB_SERIAL_ERR_ACCESS: return PPK2_ERR_ACCESS;
    case LMB_SERIAL_ERR_BUSY: return PPK2_ERR_BUSY;
    case LMB_SERIAL_ERR_DISCONNECTED: return PPK2_ERR_DISCONNECTED;
    case LMB_SERIAL_ERR_NO_MEM: return PPK2_ERR_NO_MEM;
    case LMB_SERIAL_ERR_UNSUPPORTED: return PPK2_ERR_UNSUPPORTED;
    default: return PPK2_ERR_IO;
    }
}

const char *ppk2_dev_strerror(int code)
{
    switch (code) {
    case PPK2_ERR_NOT_FOUND: return "no such port";
    case PPK2_ERR_ACCESS: return "permission denied (Linux: see the udev rule in the README)";
    case PPK2_ERR_BUSY: return "port in use by another program";
    default: return ppk2_strerror(code);
    }
}

int ppk2_dev_find(lmb_serial_port *ports, size_t cap, size_t *count)
{
    if (!count || (cap && !ports))
        return PPK2_ERR_INVALID_ARG;
    *count = 0;
    size_t total = 0;
    int rc = lmb_serial_list(NULL, 0, &total);
    if (rc != LMB_SERIAL_OK)
        return from_serial(rc);
    if (total == 0)
        return PPK2_OK;
    lmb_serial_port *all = calloc(total, sizeof *all);
    if (!all)
        return PPK2_ERR_NO_MEM;
    size_t n = 0;
    rc = lmb_serial_list(all, total, &n);
    if (rc == LMB_SERIAL_OK) {
        if (n > total)
            n = total; /* a port appeared in between */
        for (size_t i = 0; i < n; i++)
            if (all[i].usb_vid == PPK2_USB_VID && all[i].usb_pid == PPK2_USB_PID) {
                if (*count < cap)
                    ports[*count] = all[i];
                (*count)++;
            }
    }
    free(all);
    return from_serial(rc);
}

/* Sends what a ppk2_ctl call produced, one write per command. */
static int send(ppk2_dev *d, int built, const ppk2_tx *tx)
{
    if (built != PPK2_OK)
        return built;
    for (size_t i = 0; i < tx->count; i++) {
        const int rc = lmb_serial_write(d->port, tx->frames[i].bytes, tx->frames[i].len);
        if (rc != LMB_SERIAL_OK) {
            ppk2_ctl_invalidate(d->ctl);
            return from_serial(rc);
        }
    }
    return PPK2_OK;
}

/* Reads and drops bytes until the port has been quiet for DRAIN_QUIET_MS. A port that keeps
 * sending more than DRAIN_MAX_MS worth of samples after a stop is not a working PPK2. */
static int drain(ppk2_dev *d)
{
    d->pending = d->pending_at = 0;
    const uint64_t limit = (uint64_t)PPK2_SAMPLE_RATE * 4u * DRAIN_MAX_MS / 1000u;
    uint64_t dropped = 0;
    for (;;) {
        size_t got = 0;
        const int rc = lmb_serial_read(d->port, d->buf, sizeof d->buf, DRAIN_QUIET_MS, &got);
        if (rc != LMB_SERIAL_OK)
            return from_serial(rc);
        if (got == 0)
            return PPK2_OK;
        dropped += got;
        if (dropped > limit)
            return PPK2_ERR_STATE;
    }
}

static int read_metadata(ppk2_dev *d)
{
    ppk2_tx tx;
    int rc = send(d, ppk2_ctl_request_metadata(d->ctl, &tx), &tx);
    if (rc != PPK2_OK)
        return rc;
    ppk2_meta_parser *p = ppk2_meta_parser_new();
    if (!p)
        return PPK2_ERR_NO_MEM;
    int left = METADATA_TIMEOUT_MS;
    rc = PPK2_ERR_TIMEOUT;
    while (left > 0) {
        size_t got = 0;
        const int r = lmb_serial_read(d->port, d->buf, sizeof d->buf, 100, &got);
        if (r != LMB_SERIAL_OK) {
            rc = from_serial(r);
            break;
        }
        if (got == 0) {
            left -= 100;
            continue;
        }
        ppk2_meta_parser_feed(p, d->buf, got); /* bytes after END are dropped: the unit is idle */
        if (ppk2_meta_parser_done(p)) {
            d->meta = *ppk2_meta_parser_result(p);
            rc = PPK2_OK;
            break;
        }
    }
    ppk2_meta_parser_free(p);
    return rc;
}

int ppk2_dev_open(const char *path, unsigned cap_mv, ppk2_dev **out)
{
    if (!path || !out)
        return PPK2_ERR_INVALID_ARG;
    *out = NULL;
    ppk2_dev *d = calloc(1, sizeof *d);
    if (!d)
        return PPK2_ERR_NO_MEM;
    d->ctl = ppk2_ctl_new(cap_mv);
    d->stream = ppk2_stream_new();
    if (!d->ctl || !d->stream) {
        ppk2_ctl_free(d->ctl);
        ppk2_stream_free(d->stream);
        free(d);
        return PPK2_ERR_NO_MEM;
    }
    int rc = from_serial(lmb_serial_open(path, BAUD, 1, &d->port));
    if (rc != PPK2_OK) {
        ppk2_ctl_free(d->ctl);
        ppk2_stream_free(d->stream);
        free(d);
        return rc;
    }
    /* A unit left streaming by another program: stop it and drop what it sent. */
    ppk2_tx tx;
    rc = send(d, ppk2_ctl_stop(d->ctl, &tx), &tx);
    if (rc == PPK2_OK)
        rc = drain(d);
    if (rc == PPK2_OK)
        rc = read_metadata(d);
    if (rc != PPK2_OK) {
        ppk2_dev_close(d);
        return rc;
    }
    ppk2_cal_from_metadata(&d->cal, &d->meta);
    *out = d;
    return PPK2_OK;
}

void ppk2_dev_close(ppk2_dev *d)
{
    if (!d)
        return;
    if (d->port) {
        ppk2_tx tx;
        if (ppk2_ctl_shutdown(d->ctl, &tx) == PPK2_OK)
            (void)send(d, PPK2_OK, &tx);
        lmb_serial_close(d->port);
    }
    ppk2_ctl_free(d->ctl);
    ppk2_stream_free(d->stream);
    free(d);
}

const ppk2_metadata *ppk2_dev_metadata(const ppk2_dev *d)
{
    return d ? &d->meta : NULL;
}

const ppk2_cal *ppk2_dev_calibration(const ppk2_dev *d)
{
    return d ? &d->cal : NULL;
}

const ppk2_ctl *ppk2_dev_ctl(const ppk2_dev *d)
{
    return d ? d->ctl : NULL;
}

int ppk2_dev_select_supply(ppk2_dev *d, ppk2_supply supply)
{
    if (!d)
        return PPK2_ERR_INVALID_ARG;
    ppk2_tx tx;
    return send(d, ppk2_ctl_select_supply(d->ctl, supply, &tx), &tx);
}

int ppk2_dev_set_voltage(ppk2_dev *d, unsigned mv, unsigned *applied_mv)
{
    if (!d)
        return PPK2_ERR_INVALID_ARG;
    ppk2_tx tx;
    return send(d, ppk2_ctl_set_voltage(d->ctl, mv, applied_mv, &tx), &tx);
}

int ppk2_dev_set_power(ppk2_dev *d, int on)
{
    if (!d)
        return PPK2_ERR_INVALID_ARG;
    ppk2_tx tx;
    return send(d, ppk2_ctl_set_power(d->ctl, on, &tx), &tx);
}

int ppk2_dev_start(ppk2_dev *d)
{
    if (!d)
        return PPK2_ERR_INVALID_ARG;
    ppk2_stream_reset(d->stream);
    ppk2_stream_set_calibration(d->stream, &d->cal, ppk2_ctl_source_volts(d->ctl));
    d->pending = d->pending_at = 0;
    ppk2_tx tx;
    return send(d, ppk2_ctl_start(d->ctl, &tx), &tx);
}

int ppk2_dev_read(ppk2_dev *d, ppk2_sample *out, size_t cap, int timeout_ms, size_t *got)
{
    if (!d || !got || (cap && !out))
        return PPK2_ERR_INVALID_ARG;
    *got = 0;
    if (cap == 0)
        return PPK2_OK;
    if (d->pending == 0) {
        /* Read at most what fits as samples, so a read is never left half decoded for long. */
        size_t want = cap * 4 < sizeof d->buf ? cap * 4 : sizeof d->buf;
        size_t n = 0;
        const int rc = lmb_serial_read(d->port, d->buf, want, timeout_ms, &n);
        if (rc != LMB_SERIAL_OK)
            return from_serial(rc);
        d->pending = n;
        d->pending_at = 0;
    }
    size_t used = 0;
    *got = ppk2_stream_decode(d->stream, d->buf + d->pending_at, d->pending, &used, out, cap);
    d->pending -= used;
    d->pending_at += used;
    return PPK2_OK;
}

int ppk2_dev_stop(ppk2_dev *d)
{
    if (!d)
        return PPK2_ERR_INVALID_ARG;
    ppk2_tx tx;
    const int rc = send(d, ppk2_ctl_stop(d->ctl, &tx), &tx);
    return rc == PPK2_OK ? drain(d) : rc;
}

void ppk2_dev_stats(const ppk2_dev *d, ppk2_stream_stats *stats)
{
    if (d && stats)
        ppk2_stream_get_stats(d->stream, stats);
}
