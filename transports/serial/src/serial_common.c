/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "serial_common.h"

#include <stdlib.h>
#include <string.h>

const char *lmb_serial_strerror(int code)
{
    switch (code) {
    case LMB_SERIAL_OK: return "success";
    case LMB_SERIAL_ERR_INVALID_ARG: return "invalid argument";
    case LMB_SERIAL_ERR_NOT_FOUND: return "no such port";
    case LMB_SERIAL_ERR_ACCESS: return "permission denied";
    case LMB_SERIAL_ERR_BUSY: return "port in use by another program";
    case LMB_SERIAL_ERR_IO: return "input/output error";
    case LMB_SERIAL_ERR_DISCONNECTED: return "device disconnected";
    case LMB_SERIAL_ERR_NO_MEM: return "out of memory";
    case LMB_SERIAL_ERR_UNSUPPORTED: return "not supported on this system";
    default: return "unknown error";
    }
}

void lmb_serial_copy(char *dst, size_t cap, const char *src)
{
    if (!cap)
        return;
    size_t n = strlen(src);
    if (n >= cap)
        n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void lmb_serial_list_begin(lmb_serial_list_builder *b, lmb_serial_port *out, size_t cap)
{
    memset(b, 0, sizeof *b);
    b->out = out;
    b->cap = cap;
}

void lmb_serial_list_add(lmb_serial_list_builder *b, const lmb_serial_port *port)
{
    if (b->count == b->alloc) {
        const size_t n = b->alloc ? b->alloc * 2 : 16;
        lmb_serial_port *bigger = realloc(b->all, n * sizeof *bigger);
        if (!bigger) {
            b->lost++;
            return;
        }
        b->all = bigger;
        b->alloc = n;
    }
    b->all[b->count++] = *port;
}

static int by_path(const void *a, const void *b)
{
    return strcmp(((const lmb_serial_port *)a)->path, ((const lmb_serial_port *)b)->path);
}

size_t lmb_serial_list_end(lmb_serial_list_builder *b)
{
    if (b->count)
        qsort(b->all, b->count, sizeof *b->all, by_path);
    const size_t keep = b->count < b->cap ? b->count : b->cap;
    if (keep)
        memcpy(b->out, b->all, keep * sizeof *b->all);
    free(b->all);
    b->all = NULL;
    return b->count + b->lost;
}
