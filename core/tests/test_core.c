/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* The core without any real driver: a fake family with one device and one facet exercises the
 * registry, enumeration, open/close, busy detection, facet validation and ABI checks. */

#include "libmybench/driver.h"
#include "libmybench/facets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                              \
    } while (0)

/* ---- a fake family "fake": devices fake:1 and fake:2, a DigitalIO facet on both, and a
 * hardware device fake:3 with a facet of the wrong ABI ---- */

typedef struct fake {
    int number;
    lmb_digital_io dio;
    uint32_t value;
} fake;

static int g_closed, g_destroyed;

static int fake_info(void *self, lmb_digital_io_info *out)
{
    (void)self;
    out->lines = 8;
    return LMB_OK;
}

static int fake_write(void *self, uint32_t value, uint32_t mask)
{
    fake *f = self;
    f->value = (f->value & ~mask) | (value & mask);
    return LMB_OK;
}

static int fake_read(void *self, uint32_t *levels)
{
    *levels = ((fake *)self)->value;
    return LMB_OK;
}

static void fake_close(void *drv)
{
    g_closed++;
    free(drv);
}

static int fake_facet(void *drv, lmb_facet_kind kind, const void **out)
{
    fake *f = drv;
    if (kind != LMB_FACET_DIGITAL_IO)
        return LMB_ERR_NO_FACET;
    *out = &f->dio;
    return LMB_OK;
}

static int fake_status(void *drv)
{
    return ((fake *)drv)->number == 2 ? LMB_ERR_DISCONNECTED : LMB_OK;
}

static void fill_info(lmb_device_info *i, int n)
{
    memset(i, 0, sizeof *i);
    snprintf(i->id, sizeof i->id, "fake:%d", n);
    strcpy(i->family, "fake");
    snprintf(i->name, sizeof i->name, "Fake %d", n);
    strcpy(i->transport, "test");
    i->facets = LMB_FACET_BIT(LMB_FACET_DIGITAL_IO);
    i->simulated = n != 3;
}

static int fake_enumerate(void *user, uint32_t flags, lmb_device_info *out, size_t cap, size_t *count)
{
    (void)user;
    *count = 0;
    for (int n = 1; n <= 3; n++) {
        if ((flags & LMB_ENUM_NO_SIMULATED) && n != 3)
            continue;
        if ((flags & LMB_ENUM_ONLY_SIMULATED) && n == 3)
            continue;
        if (*count < cap)
            fill_info(&out[*count], n);
        (*count)++;
    }
    return LMB_OK;
}

static int fake_open(void *user, const char *id, lmb_device_ops *ops, lmb_device_info *info)
{
    (void)user;
    if (strncmp(id, "fake:", 5) != 0 || (id[5] < '1' || id[5] > '3') || id[6])
        return LMB_ERR_NOT_FOUND;
    fake *f = calloc(1, sizeof *f);
    f->number = id[5] - '0';
    f->dio = (lmb_digital_io){LMB_ABI_VERSION, sizeof f->dio, f, fake_info, NULL, NULL, fake_write, fake_read};
    if (f->number == 3)
        f->dio.abi_version = LMB_ABI_VERSION + 1; /* a driver built against another ABI */
    ops->drv = f;
    ops->close = fake_close;
    ops->facet = fake_facet;
    ops->status = fake_status;
    fill_info(info, f->number);
    return LMB_OK;
}

static void fake_destroy(void *user)
{
    (void)user;
    g_destroyed++;
}

static lmb_family fake_family(void)
{
    lmb_family f;
    memset(&f, 0, sizeof f);
    f.abi_version = LMB_ABI_VERSION;
    f.size = sizeof f;
    f.name = "fake";
    f.enumerate = fake_enumerate;
    f.open = fake_open;
    f.destroy = fake_destroy;
    return f;
}

static void test_abi_and_registry(void)
{
    CHECK(lmb_abi_version() == LMB_ABI_VERSION);
    lmb_context *ctx = (lmb_context *)1;
    CHECK(lmb_context_new(LMB_ABI_VERSION + 1, &ctx) == LMB_ERR_ABI && ctx == NULL);
    CHECK(lmb_context_new(0, &ctx) == LMB_ERR_ABI && ctx == NULL);
    CHECK(lmb_context_new(LMB_ABI_VERSION, NULL) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_context_new(LMB_ABI_VERSION, &ctx) == LMB_OK && ctx);

    lmb_family f = fake_family();
    CHECK(lmb_context_register_family(NULL, &f) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_context_register_family(ctx, NULL) == LMB_ERR_INVALID_ARG);
    lmb_family bad = f;
    bad.abi_version = LMB_ABI_VERSION + 1;
    CHECK(lmb_context_register_family(ctx, &bad) == LMB_ERR_ABI);
    bad = f;
    bad.size = 8; /* a struct shorter than ours: built against an older header */
    CHECK(lmb_context_register_family(ctx, &bad) == LMB_ERR_ABI);
    bad = f;
    bad.open = NULL;
    CHECK(lmb_context_register_family(ctx, &bad) == LMB_ERR_INVALID_ARG);
    bad = f;
    bad.name = "a-name-that-is-far-too-long-for-the-fixed-field";
    CHECK(lmb_context_register_family(ctx, &bad) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_context_register_family(ctx, &f) == LMB_OK);
    CHECK(lmb_context_register_family(ctx, &f) == LMB_ERR_EXISTS);

    lmb_context_free(ctx);
    CHECK(g_destroyed == 1); /* the family was destroyed once, the rejected ones never */
    lmb_context_free(NULL);
}

static void test_enumerate_open(void)
{
    lmb_context *ctx;
    CHECK(lmb_context_new(LMB_ABI_VERSION, &ctx) == LMB_OK);
    size_t n = 99;
    CHECK(lmb_enumerate(ctx, 0, NULL, 0, &n) == LMB_OK && n == 0); /* no family yet */
    lmb_family f = fake_family();
    CHECK(lmb_context_register_family(ctx, &f) == LMB_OK);

    CHECK(lmb_enumerate(ctx, 0, NULL, 0, &n) == LMB_OK && n == 3);
    lmb_device_info two[2];
    CHECK(lmb_enumerate(ctx, 0, two, 2, &n) == LMB_OK && n == 3); /* total, even when cap is smaller */
    CHECK(strcmp(two[0].id, "fake:1") == 0 && strcmp(two[1].id, "fake:2") == 0);
    CHECK(lmb_enumerate(ctx, LMB_ENUM_NO_SIMULATED, NULL, 0, &n) == LMB_OK && n == 1);
    CHECK(lmb_enumerate(ctx, LMB_ENUM_ONLY_SIMULATED, NULL, 0, &n) == LMB_OK && n == 2);
    CHECK(lmb_enumerate(ctx, LMB_ENUM_NO_SIMULATED | LMB_ENUM_ONLY_SIMULATED, NULL, 0, &n) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_enumerate(ctx, 0x80, NULL, 0, &n) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_enumerate(ctx, 0, NULL, 2, &n) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_enumerate(ctx, 0, NULL, 0, NULL) == LMB_ERR_INVALID_ARG);

    lmb_device *dev = (lmb_device *)1;
    CHECK(lmb_device_open(ctx, "nothing:0", &dev) == LMB_ERR_NOT_FOUND && dev == NULL);
    CHECK(lmb_device_open(ctx, "", &dev) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_device_open(ctx, NULL, &dev) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_device_open(NULL, "fake:1", &dev) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_device_open(ctx, "fake:1", NULL) == LMB_ERR_INVALID_ARG);

    CHECK(lmb_device_open(ctx, "fake:1", &dev) == LMB_OK && dev);
    lmb_device *again = NULL;
    CHECK(lmb_device_open(ctx, "fake:1", &again) == LMB_ERR_BUSY && again == NULL);
    lmb_device_info info;
    CHECK(lmb_device_get_info(dev, &info) == LMB_OK && strcmp(info.name, "Fake 1") == 0);
    CHECK(lmb_device_status(dev) == LMB_OK);

    /* facets: present, absent, out of range */
    const lmb_digital_io *dio = NULL;
    CHECK(lmb_device_digital_io(dev, &dio) == LMB_OK && dio);
    const lmb_digital_io *dio2 = NULL;
    CHECK(lmb_device_digital_io(dev, &dio2) == LMB_OK && dio2 == dio); /* the same pointer each time */
    CHECK(LMB_HAS(dio, write) && LMB_HAS(dio, read));
    CHECK(!LMB_HAS(dio, set_direction)); /* the driver left it NULL */
    uint32_t v = 0;
    CHECK(dio->write(dio->self, 0xA5, 0xFF) == LMB_OK && dio->read(dio->self, &v) == LMB_OK && v == 0xA5);
    const lmb_current_stream *cs = (const lmb_current_stream *)1;
    CHECK(lmb_device_current_stream(dev, &cs) == LMB_ERR_NO_FACET && cs == NULL);
    const void *p = (const void *)1;
    CHECK(lmb_device_facet(dev, (lmb_facet_kind)0, &p) == LMB_ERR_INVALID_ARG && p == NULL);
    CHECK(lmb_device_facet(dev, LMB_FACET_KIND_END_, &p) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_device_facet(dev, (lmb_facet_kind)-3, &p) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_device_facet(dev, LMB_FACET_DIGITAL_IO, NULL) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_device_facet(NULL, LMB_FACET_DIGITAL_IO, &p) == LMB_ERR_INVALID_ARG);
    CHECK(lmb_device_get_info(NULL, &info) == LMB_ERR_INVALID_ARG);

    /* driver data only for the right family */
    CHECK(lmb_device_driver_data(dev, "fake") != NULL);
    CHECK(lmb_device_driver_data(dev, "ppk2") == NULL);

    /* close, then the id is free again */
    lmb_device_close(dev);
    CHECK(g_closed == 1);
    CHECK(lmb_device_open(ctx, "fake:1", &again) == LMB_OK);
    lmb_device_close(again);
    lmb_device_close(NULL);

    /* a device that went away reports it; a driver with a facet of another ABI is refused */
    lmb_device *gone = NULL, *odd = NULL;
    CHECK(lmb_device_open(ctx, "fake:2", &gone) == LMB_OK);
    CHECK(lmb_device_status(gone) == LMB_ERR_DISCONNECTED);
    CHECK(lmb_device_open(ctx, "fake:3", &odd) == LMB_OK);
    CHECK(lmb_device_facet(odd, LMB_FACET_DIGITAL_IO, &p) == LMB_ERR_ABI && p == NULL);

    CHECK(lmb_set_hotplug(ctx, NULL, NULL) == LMB_ERR_UNSUPPORTED); /* designed, not implemented */

    /* freeing the context closes what is still open */
    const int closed_before = g_closed;
    lmb_context_free(ctx);
    CHECK(g_closed == closed_before + 2);
}

static void test_strings(void)
{
    for (int c = LMB_OK; c >= LMB_ERR_OVERFLOW; c--)
        CHECK(strcmp(lmb_strerror(c), "unknown error") != 0);
    CHECK(strcmp(lmb_strerror(-999), "unknown error") == 0);
    CHECK(strcmp(lmb_facet_name(LMB_FACET_CURRENT_STREAM), "current-stream") == 0);
    CHECK(strcmp(lmb_facet_name(0), "?") == 0);
}

int main(void)
{
    test_abi_and_registry();
    test_enumerate_open();
    test_strings();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
