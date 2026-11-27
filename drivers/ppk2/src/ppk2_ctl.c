/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Command bytes and the power sequence (report: "Measurement connection", "Board connections"). */

#include "libmybench/ppk2.h"

#include <math.h>
#include <stdlib.h>

enum {
    OP_START = 0x06,
    OP_STOP = 0x07,
    OP_POWER = 0x0C,
    OP_VOLTAGE = 0x0D,
    OP_SUPPLY = 0x11,
    OP_METADATA = 0x19,
};

struct ppk2_ctl {
    unsigned cap_mv;
    ppk2_supply supply; /* PPK2_SUPPLY_NONE when unknown */
    int gate;           /* 1 on, 0 off, -1 unknown */
    int sampling;       /* 1, 0, -1 unknown */
    unsigned mv;
    int mv_set;
};

const char *ppk2_strerror(int code)
{
    switch (code) {
    case PPK2_OK: return "success";
    case PPK2_ERR_INVALID_ARG: return "invalid argument";
    case PPK2_ERR_STATE: return "not allowed in the current state";
    case PPK2_ERR_NO_SPACE: return "output buffer too small";
    case PPK2_ERR_FORMAT: return "malformed input";
    case PPK2_ERR_NO_MEM: return "out of memory";
    case PPK2_ERR_IO: return "input/output error";
    case PPK2_ERR_UNSUPPORTED: return "not supported";
    case PPK2_ERR_TIMEOUT: return "timed out";
    case PPK2_ERR_DISCONNECTED: return "device disconnected";
    default: return "unknown error";
    }
}

static void push(ppk2_tx *tx, uint8_t len, uint8_t b0, uint8_t b1, uint8_t b2)
{
    /* Every public call pushes at most PPK2_TX_MAX frames. */
    ppk2_frame *f = &tx->frames[tx->count++];
    f->len = len;
    f->bytes[0] = b0;
    f->bytes[1] = b1;
    f->bytes[2] = b2;
}

ppk2_ctl *ppk2_ctl_new(unsigned cap_mv)
{
    ppk2_ctl *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    if (cap_mv == 0 || cap_mv > PPK2_VOLTAGE_MAX_MV)
        cap_mv = PPK2_VOLTAGE_MAX_MV;
    if (cap_mv < PPK2_VOLTAGE_MIN_MV)
        cap_mv = PPK2_VOLTAGE_MIN_MV;
    c->cap_mv = cap_mv;
    ppk2_ctl_invalidate(c);
    return c;
}

void ppk2_ctl_free(ppk2_ctl *ctl) { free(ctl); }

void ppk2_ctl_invalidate(ppk2_ctl *ctl)
{
    ctl->supply = PPK2_SUPPLY_NONE;
    ctl->gate = -1;
    ctl->sampling = -1;
    ctl->mv_set = 0;
}

int ppk2_ctl_request_metadata(ppk2_ctl *ctl, ppk2_tx *tx)
{
    if (!ctl || !tx)
        return PPK2_ERR_INVALID_ARG;
    tx->count = 0;
    if (ctl->sampling != 0)
        return PPK2_ERR_STATE;
    push(tx, 1, OP_METADATA, 0, 0);
    return PPK2_OK;
}

int ppk2_ctl_select_supply(ppk2_ctl *ctl, ppk2_supply supply, ppk2_tx *tx)
{
    if (!ctl || !tx || (supply != PPK2_SUPPLY_AMMETER && supply != PPK2_SUPPLY_SOURCE))
        return PPK2_ERR_INVALID_ARG;
    tx->count = 0;
    if (supply == ctl->supply && ctl->gate != -1)
        return PPK2_OK;
    /* Report: disconnect the output and deselect the previous rail before enabling the new one. */
    if (ctl->gate != 0) {
        push(tx, 2, OP_POWER, 0x00, 0);
        ctl->gate = 0;
    }
    push(tx, 2, OP_SUPPLY, (uint8_t)supply, 0);
    ctl->supply = supply;
    return PPK2_OK;
}

int ppk2_ctl_set_voltage(ppk2_ctl *ctl, unsigned mv, unsigned *applied_mv, ppk2_tx *tx)
{
    if (!ctl || !tx)
        return PPK2_ERR_INVALID_ARG;
    tx->count = 0;
    if (mv < PPK2_VOLTAGE_MIN_MV)
        mv = PPK2_VOLTAGE_MIN_MV;
    if (mv > ctl->cap_mv)
        mv = ctl->cap_mv;
    push(tx, 3, OP_VOLTAGE, (uint8_t)(mv >> 8), (uint8_t)(mv & 0xFF));
    ctl->mv = mv;
    ctl->mv_set = 1;
    if (applied_mv)
        *applied_mv = mv;
    return PPK2_OK;
}

int ppk2_ctl_set_power(ppk2_ctl *ctl, int on, ppk2_tx *tx)
{
    if (!ctl || !tx)
        return PPK2_ERR_INVALID_ARG;
    tx->count = 0;
    if (on) {
        if (ctl->supply == PPK2_SUPPLY_NONE)
            return PPK2_ERR_STATE;
        if (ctl->supply == PPK2_SUPPLY_SOURCE && !ctl->mv_set)
            return PPK2_ERR_STATE;
    }
    push(tx, 2, OP_POWER, on ? 0x01 : 0x00, 0);
    ctl->gate = on ? 1 : 0;
    return PPK2_OK;
}

int ppk2_ctl_start(ppk2_ctl *ctl, ppk2_tx *tx)
{
    if (!ctl || !tx)
        return PPK2_ERR_INVALID_ARG;
    tx->count = 0;
    if (ctl->sampling == 1)
        return PPK2_ERR_STATE;
    push(tx, 1, OP_START, 0, 0);
    ctl->sampling = 1;
    return PPK2_OK;
}

int ppk2_ctl_stop(ppk2_ctl *ctl, ppk2_tx *tx)
{
    if (!ctl || !tx)
        return PPK2_ERR_INVALID_ARG;
    tx->count = 0;
    push(tx, 1, OP_STOP, 0, 0);
    ctl->sampling = 0;
    return PPK2_OK;
}

int ppk2_ctl_shutdown(ppk2_ctl *ctl, ppk2_tx *tx)
{
    if (!ctl || !tx)
        return PPK2_ERR_INVALID_ARG;
    tx->count = 0;
    if (ctl->sampling != 0)
        push(tx, 1, OP_STOP, 0, 0);
    push(tx, 2, OP_POWER, 0x00, 0);
    ctl->sampling = 0;
    ctl->gate = 0;
    return PPK2_OK;
}

ppk2_supply ppk2_ctl_supply(const ppk2_ctl *ctl) { return ctl->supply; }
int ppk2_ctl_power_on(const ppk2_ctl *ctl) { return ctl->gate; }
int ppk2_ctl_sampling(const ppk2_ctl *ctl) { return ctl->sampling; }

double ppk2_ctl_source_volts(const ppk2_ctl *ctl)
{
    return ctl->mv_set ? ctl->mv / 1000.0 : NAN;
}
