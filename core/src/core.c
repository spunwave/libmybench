/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* Registry, enumeration, open and close, facet lookup (device.h, driver.h). No family-specific code. */

#include "libmybench/driver.h"

#include <stdlib.h>
#include <string.h>

typedef struct fam_node {
    lmb_family f;
    char name[32];
    struct fam_node *next;
} fam_node;

struct lmb_device {
    lmb_context *ctx;
    char key[sizeof(((lmb_device_info *)0)->id)];
    lmb_device_ops ops;
    lmb_device_info info;
    int opening; /* placeholder while a family is opening it: no ops yet */
    int last_status; /* what lmb_poll_events last saw of lmb_device_status */
    struct lmb_device *next;
};

struct lmb_context {
    lmb_mutex *mu; /* guards devs */
    fam_node *fams, **fams_tail;
    lmb_device *devs;

    /* lmb_poll_events: serialised by poll_mu; snap is the list as of the previous poll, queue holds
     * the events not handed out yet. */
    lmb_mutex *poll_mu;
    lmb_device_info *snap;
    size_t nsnap;
    lmb_event *queue;
    size_t nqueue, capqueue;
};

/* Common start of every facet vtable. */
typedef struct facet_head {
    uint32_t abi_version;
    uint32_t size;
    void *self;
} facet_head;

uint32_t lmb_abi_version(void)
{
    return LMB_ABI_VERSION;
}

const char *lmb_strerror(int code)
{
    switch (code) {
    case LMB_OK: return "ok";
    case LMB_ERR_INVALID_ARG: return "invalid argument";
    case LMB_ERR_ABI: return "ABI version mismatch";
    case LMB_ERR_NOT_FOUND: return "no such device";
    case LMB_ERR_NO_FACET: return "the device does not have that facet";
    case LMB_ERR_UNSUPPORTED: return "not supported by this device";
    case LMB_ERR_STATE: return "not allowed in the current state";
    case LMB_ERR_BUSY: return "device in use";
    case LMB_ERR_ACCESS: return "permission denied";
    case LMB_ERR_DISCONNECTED: return "device disconnected";
    case LMB_ERR_TIMEOUT: return "timed out";
    case LMB_ERR_IO: return "I/O error";
    case LMB_ERR_NO_MEM: return "out of memory";
    case LMB_ERR_EXISTS: return "already registered";
    case LMB_ERR_OVERFLOW: return "data lost: not read fast enough";
    default: return "unknown error";
    }
}

const char *lmb_facet_name(int kind)
{
    switch (kind) {
    case LMB_FACET_POWER_SUPPLY: return "power-supply";
    case LMB_FACET_CURRENT_STREAM: return "current-stream";
    case LMB_FACET_LOGIC_INPUT: return "logic-input";
    case LMB_FACET_ANALOG_INPUT: return "analog-input";
    case LMB_FACET_ANALOG_OUTPUT: return "analog-output";
    case LMB_FACET_DIGITAL_IO: return "digital-io";
    case LMB_FACET_SIM_CONTROL: return "sim-control";
    default: return "?";
    }
}

int lmb_context_new(uint32_t abi_version, lmb_context **out)
{
    if (!out)
        return LMB_ERR_INVALID_ARG;
    *out = NULL;
    if (abi_version != LMB_ABI_VERSION)
        return LMB_ERR_ABI;
    lmb_context *c = calloc(1, sizeof *c);
    if (!c)
        return LMB_ERR_NO_MEM;
    c->mu = lmb_mutex_new();
    c->poll_mu = lmb_mutex_new();
    if (!c->mu || !c->poll_mu) {
        lmb_mutex_free(c->mu);
        lmb_mutex_free(c->poll_mu);
        free(c);
        return LMB_ERR_NO_MEM;
    }
    c->fams_tail = &c->fams;
    *out = c;
    return LMB_OK;
}

void lmb_context_free(lmb_context *ctx)
{
    if (!ctx)
        return;
    for (;;) {
        lmb_device *d = ctx->devs;
        while (d && d->opening)
            d = d->next;
        if (!d)
            break;
        lmb_device_close(d);
    }
    for (fam_node *n = ctx->fams; n;) {
        fam_node *next = n->next;
        if (n->f.destroy)
            n->f.destroy(n->f.user);
        free(n);
        n = next;
    }
    lmb_mutex_free(ctx->mu);
    lmb_mutex_free(ctx->poll_mu);
    free(ctx->snap);
    free(ctx->queue);
    free(ctx);
}

int lmb_context_register_family(lmb_context *ctx, const lmb_family *family)
{
    if (!ctx || !family || !family->name || !family->enumerate || !family->open)
        return LMB_ERR_INVALID_ARG;
    if (family->abi_version != LMB_ABI_VERSION)
        return LMB_ERR_ABI;
    if (family->size < sizeof *family)
        return LMB_ERR_ABI; /* built against an older, shorter lmb_family */
    if (!family->name[0] || strlen(family->name) >= sizeof((fam_node *)0)->name)
        return LMB_ERR_INVALID_ARG;
    for (fam_node *n = ctx->fams; n; n = n->next)
        if (strcmp(n->name, family->name) == 0)
            return LMB_ERR_EXISTS;
    fam_node *n = calloc(1, sizeof *n);
    if (!n)
        return LMB_ERR_NO_MEM;
    n->f = *family;
    strcpy(n->name, family->name);
    n->f.name = n->name;
    *ctx->fams_tail = n;
    ctx->fams_tail = &n->next;
    return LMB_OK;
}

int lmb_enumerate(lmb_context *ctx, uint32_t flags, lmb_device_info *out, size_t cap, size_t *count)
{
    if (!ctx || !count || (cap && !out))
        return LMB_ERR_INVALID_ARG;
    if ((flags & ~(LMB_ENUM_NO_SIMULATED | LMB_ENUM_ONLY_SIMULATED)) ||
        (flags & LMB_ENUM_NO_SIMULATED && flags & LMB_ENUM_ONLY_SIMULATED))
        return LMB_ERR_INVALID_ARG;
    size_t total = 0;
    for (const fam_node *n = ctx->fams; n; n = n->next) {
        const size_t room = total < cap ? cap - total : 0;
        size_t found = 0;
        /* A family that fails (no permission to list ports, say) must not hide the others. */
        if (n->f.enumerate(n->f.user, flags, room ? out + total : NULL, room, &found) != LMB_OK)
            continue;
        total += found;
    }
    *count = total;
    return LMB_OK;
}

static lmb_device *find_key(lmb_context *ctx, const char *key, const lmb_device *except)
{
    for (lmb_device *d = ctx->devs; d; d = d->next)
        if (d != except && strcmp(d->key, key) == 0)
            return d;
    return NULL;
}

static void unlink_device(lmb_context *ctx, lmb_device *dev)
{
    lmb_mutex_lock(ctx->mu);
    for (lmb_device **p = &ctx->devs; *p; p = &(*p)->next)
        if (*p == dev) {
            *p = dev->next;
            break;
        }
    lmb_mutex_unlock(ctx->mu);
}

int lmb_device_open(lmb_context *ctx, const char *id, lmb_device **out)
{
    if (!out)
        return LMB_ERR_INVALID_ARG;
    *out = NULL;
    if (!ctx || !id || !id[0] || strlen(id) >= sizeof((lmb_device *)0)->key)
        return LMB_ERR_INVALID_ARG;
    lmb_device *dev = calloc(1, sizeof *dev);
    if (!dev)
        return LMB_ERR_NO_MEM;
    dev->ctx = ctx;
    dev->opening = 1;
    strcpy(dev->key, id);
    lmb_mutex_lock(ctx->mu);
    if (find_key(ctx, id, NULL)) {
        lmb_mutex_unlock(ctx->mu);
        free(dev);
        return LMB_ERR_BUSY;
    }
    dev->next = ctx->devs;
    ctx->devs = dev;
    lmb_mutex_unlock(ctx->mu);

    int rc = LMB_ERR_NOT_FOUND;
    /* Zeroed up front too: MSVC (C4701) can't see that they are only read after a family's open. */
    lmb_device_ops ops;
    lmb_device_info info;
    memset(&ops, 0, sizeof ops);
    memset(&info, 0, sizeof info);
    for (const fam_node *n = ctx->fams; n; n = n->next) {
        memset(&ops, 0, sizeof ops);
        memset(&info, 0, sizeof info);
        ops.size = sizeof ops;
        rc = n->f.open(n->f.user, id, &ops, &info);
        if (rc != LMB_ERR_NOT_FOUND)
            break;
    }
    if (rc == LMB_OK && (!ops.close || !ops.facet || !info.id[0])) {
        if (ops.close)
            ops.close(ops.drv);
        rc = LMB_ERR_INVALID_ARG; /* a broken driver */
    }
    if (rc == LMB_OK) {
        info.id[sizeof info.id - 1] = '\0';
        lmb_mutex_lock(ctx->mu);
        const int dup = find_key(ctx, info.id, dev) != NULL;
        if (!dup) {
            dev->ops = ops;
            dev->info = info;
            dev->last_status = LMB_OK;
            strcpy(dev->key, info.id);
            dev->opening = 0;
        }
        lmb_mutex_unlock(ctx->mu);
        if (dup) {
            ops.close(ops.drv);
            rc = LMB_ERR_BUSY;
        }
    }
    if (rc != LMB_OK) {
        unlink_device(ctx, dev);
        free(dev);
        return rc;
    }
    *out = dev;
    return LMB_OK;
}

void lmb_device_close(lmb_device *dev)
{
    if (!dev)
        return;
    dev->ops.close(dev->ops.drv);
    unlink_device(dev->ctx, dev);
    free(dev);
}

int lmb_device_get_info(const lmb_device *dev, lmb_device_info *out)
{
    if (!dev || !out)
        return LMB_ERR_INVALID_ARG;
    *out = dev->info;
    return LMB_OK;
}

int lmb_device_status(lmb_device *dev)
{
    if (!dev)
        return LMB_ERR_INVALID_ARG;
    return dev->ops.status ? dev->ops.status(dev->ops.drv) : LMB_OK;
}

int lmb_device_facet(lmb_device *dev, lmb_facet_kind kind, const void **out)
{
    if (!out)
        return LMB_ERR_INVALID_ARG;
    *out = NULL;
    if (!dev || (int)kind < 1 || (int)kind >= (int)LMB_FACET_KIND_END_)
        return LMB_ERR_INVALID_ARG;
    const void *p = NULL;
    const int rc = dev->ops.facet(dev->ops.drv, kind, &p);
    if (rc != LMB_OK)
        return rc;
    if (!p)
        return LMB_ERR_NO_FACET;
    const facet_head *h = p;
    if (h->abi_version != LMB_ABI_VERSION || h->size < sizeof *h)
        return LMB_ERR_ABI;
    *out = p;
    return LMB_OK;
}

void *lmb_device_driver_data(lmb_device *dev, const char *family_name)
{
    if (!dev || !family_name || strcmp(dev->info.family, family_name) != 0)
        return NULL;
    return dev->ops.drv;
}

/* ---- polled events ---- */

static int queue_event(lmb_context *ctx, lmb_event_type type, int status, const lmb_device_info *info)
{
    if (ctx->nqueue == ctx->capqueue) {
        const size_t cap = ctx->capqueue ? ctx->capqueue * 2 : 16;
        lmb_event *q = realloc(ctx->queue, cap * sizeof *q);
        if (!q)
            return LMB_ERR_NO_MEM;
        ctx->queue = q;
        ctx->capqueue = cap;
    }
    lmb_event *e = &ctx->queue[ctx->nqueue++];
    memset(e, 0, sizeof *e);
    e->size = sizeof *e;
    e->type = (uint32_t)type;
    e->status = status;
    e->info = *info;
    return LMB_OK;
}

static const lmb_device_info *find_info(const lmb_device_info *list, size_t n, const char *id)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(list[i].id, id) == 0)
            return &list[i];
    return NULL;
}

int lmb_poll_events(lmb_context *ctx, lmb_event *out, size_t cap, size_t *count)
{
    if (!ctx || !count || (cap && !out))
        return LMB_ERR_INVALID_ARG;
    *count = 0;
    for (size_t i = 0; i < cap; i++)
        if (out[i].size < 4 * sizeof(uint32_t))
            return LMB_ERR_INVALID_ARG; /* `size` not set: the caller must set it to sizeof(lmb_event) */
    lmb_mutex_lock(ctx->poll_mu);

    /* The list now. A family that cannot list is skipped, as in lmb_enumerate. */
    size_t total = 0;
    int rc = lmb_enumerate(ctx, 0, NULL, 0, &total);
    lmb_device_info *now = NULL;
    size_t nnow = 0;
    if (rc == LMB_OK && total) {
        now = calloc(total, sizeof *now);
        if (!now)
            rc = LMB_ERR_NO_MEM;
        else
            rc = lmb_enumerate(ctx, 0, now, total, &nnow);
        if (rc == LMB_OK && nnow > total)
            nnow = total; /* a device appeared between the two calls: the next poll reports it */
    }
    if (rc == LMB_OK) {
        for (size_t i = 0; i < nnow; i++)
            if (!find_info(ctx->snap, ctx->nsnap, now[i].id))
                (void)queue_event(ctx, LMB_EVENT_ARRIVED, LMB_OK, &now[i]);
        for (size_t i = 0; i < ctx->nsnap; i++)
            if (!find_info(now, nnow, ctx->snap[i].id))
                (void)queue_event(ctx, LMB_EVENT_LEFT, LMB_OK, &ctx->snap[i]);
        free(ctx->snap);
        ctx->snap = now;
        ctx->nsnap = nnow;
    } else {
        free(now);
    }

    /* Open devices whose status changed. */
    lmb_mutex_lock(ctx->mu);
    for (lmb_device *d = ctx->devs; d; d = d->next) {
        if (d->opening)
            continue;
        const int st = lmb_device_status(d);
        if (st != d->last_status) {
            d->last_status = st;
            (void)queue_event(ctx, LMB_EVENT_STATUS, st, &d->info);
        }
    }
    lmb_mutex_unlock(ctx->mu);

    size_t n = ctx->nqueue < cap ? ctx->nqueue : cap;
    if (out) {
        for (size_t i = 0; i < n; i++) {
            /* Only what fits in the caller's struct (it may be older and shorter). */
            const uint32_t want = out[i].size;
            memcpy(&out[i], &ctx->queue[i], want < sizeof(lmb_event) ? want : sizeof(lmb_event));
            out[i].size = want;
        }
        memmove(ctx->queue, ctx->queue + n, (ctx->nqueue - n) * sizeof *ctx->queue);
        ctx->nqueue -= n;
        *count = n;
    } else {
        *count = ctx->nqueue; /* cap 0: how many are pending */
    }
    lmb_mutex_unlock(ctx->poll_mu);
    return rc;
}

int lmb_set_hotplug(lmb_context *ctx, lmb_hotplug_fn cb, void *user)
{
    (void)user;
    if (!ctx)
        return LMB_ERR_INVALID_ARG;
    (void)cb;
    return LMB_ERR_UNSUPPORTED;
}
