/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* lmb-tool: a generic command-line client of the device model. It includes only the public
 * device and facet headers: no family-specific code, so what works here for the simulated PPK2
 * works for any family that registers (the built-in list is lmb_register_builtin_families).
 *
 *   lmb-tool list [--no-sim | --sim-only]
 *   lmb-tool info --device ID
 *   lmb-tool stream --device ID [--seconds S] [--supply external|internal] [--volts V] [--power] [--csv]
 */

#include "libmybench/device.h"
#include "libmybench/facets.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(const char *what, int rc)
{
    fprintf(stderr, "lmb-tool: %s: %s\n", what, lmb_strerror(rc));
    return 1;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: lmb-tool list [--no-sim | --sim-only]\n"
            "       lmb-tool info --device ID\n"
            "       lmb-tool stream --device ID [--seconds S] [--supply external|internal] [--volts V]\n"
            "                       [--power] [--csv]\n");
}

static void print_facets(uint32_t mask)
{
    const char *sep = "";
    for (int k = 1; k < (int)LMB_FACET_KIND_END_; k++)
        if (mask & LMB_FACET_BIT(k)) {
            printf("%s%s", sep, lmb_facet_name(k));
            sep = ",";
        }
    if (!*sep)
        printf("-");
}

static int cmd_list(lmb_context *ctx, uint32_t flags)
{
    size_t n = 0;
    int rc = lmb_enumerate(ctx, flags, NULL, 0, &n);
    if (rc != LMB_OK)
        return fail("enumerate", rc);
    lmb_device_info *v = calloc(n ? n : 1, sizeof *v);
    if (!v)
        return fail("list", LMB_ERR_NO_MEM);
    size_t m = 0;
    rc = lmb_enumerate(ctx, flags, v, n, &m);
    if (rc != LMB_OK) {
        free(v);
        return fail("enumerate", rc);
    }
    for (size_t i = 0; i < m && i < n; i++) {
        printf("%s\t%s\t%s\t%s\t%s\t", v[i].id, v[i].family, v[i].name, v[i].transport,
               v[i].serial[0] ? v[i].serial : "-");
        print_facets(v[i].facets);
        printf("\n");
    }
    free(v);
    return 0;
}

static int cmd_info(lmb_device *dev)
{
    lmb_device_info info;
    lmb_device_get_info(dev, &info);
    printf("id: %s\nfamily: %s\nname: %s\nserial: %s\ntransport: %s\nsimulated: %d\nfacets: ", info.id,
           info.family, info.name, info.serial, info.transport, info.simulated);
    print_facets(info.facets);
    printf("\n");
    const lmb_power_supply *ps;
    if (lmb_device_power_supply(dev, &ps) == LMB_OK && LMB_HAS(ps, get_info)) {
        lmb_power_supply_info pi = LMB_SIZED(lmb_power_supply_info);
        if (ps->get_info(ps->self, &pi) == LMB_OK)
            printf("power-supply: modes 0x%x, %.3f..%.3f V\n", pi.modes, pi.min_v, pi.max_v);
    }
    const lmb_current_stream *cs;
    if (lmb_device_current_stream(dev, &cs) == LMB_OK && LMB_HAS(cs, get_info)) {
        lmb_stream_info si = LMB_SIZED(lmb_stream_info);
        if (cs->get_info(cs->self, &si) == LMB_OK)
            printf("current-stream: %.0f S/s\n", si.sample_rate_hz);
    }
    const lmb_logic_input *li;
    if (lmb_device_logic_input(dev, &li) == LMB_OK && LMB_HAS(li, get_info)) {
        lmb_stream_info si = LMB_SIZED(lmb_stream_info);
        if (li->get_info(li->self, &si) == LMB_OK)
            printf("logic-input: %u lines, %.0f S/s\n", si.lines, si.sample_rate_hz);
    }
    return 0;
}

typedef struct opts {
    const char *device;
    double seconds, volts;
    lmb_supply_mode supply;
    int power, csv;
} opts;

static int cmd_stream(lmb_device *dev, const opts *o)
{
    const lmb_power_supply *ps = NULL;
    const lmb_current_stream *cs = NULL;
    const lmb_logic_input *li = NULL;
    int rc = lmb_device_current_stream(dev, &cs);
    if (rc != LMB_OK)
        return fail("this device has no current stream", rc);
    (void)lmb_device_logic_input(dev, &li);
    const int powered = o->power;
    if (powered || o->supply != LMB_SUPPLY_NONE || o->volts > 0) {
        rc = lmb_device_power_supply(dev, &ps);
        if (rc != LMB_OK)
            return fail("this device has no power supply", rc);
        if (powered || o->supply != LMB_SUPPLY_NONE) {
            rc = ps->set_mode(ps->self, o->supply != LMB_SUPPLY_NONE ? o->supply : LMB_SUPPLY_EXTERNAL);
            if (rc != LMB_OK)
                return fail("set supply mode", rc);
        }
        if (powered || o->volts > 0) {
            rc = ps->set_voltage(ps->self, o->volts > 0 ? o->volts : 3.3, NULL);
            if (rc != LMB_OK)
                return fail("set voltage", rc);
        }
        if (powered && (rc = ps->set_output(ps->self, 1)) != LMB_OK)
            return fail("power on", rc);
    }

    lmb_stream_info si = LMB_SIZED(lmb_stream_info);
    rc = cs->get_info(cs->self, &si);
    if (rc != LMB_OK)
        return fail("stream info", rc);
    const uint64_t want = (uint64_t)(o->seconds * si.sample_rate_hz);
    rc = cs->start(cs->self);
    if (rc != LMB_OK)
        return fail("start", rc);
    if (li && li->start(li->self) != LMB_OK)
        li = NULL;

    FILE *summary = o->csv ? stderr : stdout;
    if (o->csv)
        printf("index,time_s,current_a\n");
    lmb_current_sample buf[1024];
    lmb_logic_sample lbuf[1024];
    uint64_t total = 0, flagged = 0, gaps = 0, lost = 0, transitions = 0;
    double sum = 0, mn = INFINITY, mx = -INFINITY;
    uint32_t last_levels = 0;
    int have_levels = 0, idle = 0, status = 0;
    while (total < want) {
        size_t got = 0;
        uint64_t first = 0;
        rc = cs->read(cs->self, buf, sizeof buf / sizeof *buf, 100, &got, &first);
        if (rc != LMB_OK) {
            status = fail("read", rc);
            break;
        }
        if (got == 0 && ++idle > 30) {
            status = fail("read", LMB_ERR_TIMEOUT);
            break;
        }
        idle = got ? 0 : idle;
        uint64_t idx = first;
        for (size_t i = 0; i < got && total < want; i++, total++) {
            const lmb_current_sample *s = &buf[i];
            if (s->flags & LMB_SAMPLE_GAP) {
                gaps++;
                lost += s->lost;
                if (i > 0)
                    idx += s->lost;
            }
            if (s->flags & LMB_SAMPLE_INVALID) {
                flagged++;
            } else {
                sum += s->current_a;
                mn = fmin(mn, s->current_a);
                mx = fmax(mx, s->current_a);
            }
            if (o->csv)
                printf("%llu,%.6f,%.9e\n", (unsigned long long)idx, (double)idx / si.sample_rate_hz, s->current_a);
            idx++;
        }
        if (li) {
            size_t lgot = 0;
            while (li->read(li->self, lbuf, sizeof lbuf / sizeof *lbuf, 0, &lgot, NULL) == LMB_OK && lgot) {
                for (size_t i = 0; i < lgot; i++) {
                    if (have_levels && lbuf[i].levels != last_levels)
                        transitions++;
                    last_levels = lbuf[i].levels;
                    have_levels = 1;
                }
            }
        }
    }
    cs->stop(cs->self);
    if (li)
        li->stop(li->self);
    if (powered)
        ps->set_output(ps->self, 0);

    const uint64_t valid = total - flagged;
    fprintf(summary, "samples: %llu (%.3f s at %.0f S/s)\n", (unsigned long long)total,
            (double)total / si.sample_rate_hz, si.sample_rate_hz);
    if (valid)
        fprintf(summary, "current: mean %.6e A, min %.6e A, max %.6e A\n", sum / (double)valid, mn, mx);
    fprintf(summary, "gaps: %llu, lost: %llu, invalid: %llu\n", (unsigned long long)gaps,
            (unsigned long long)lost, (unsigned long long)flagged);
    if (li)
        fprintf(summary, "logic: last levels 0x%02x, %llu transitions\n", last_levels,
                (unsigned long long)transitions);
    return status;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *cmd = argv[1];
    opts o = {NULL, 1.0, 0.0, LMB_SUPPLY_NONE, 0, 0};
    uint32_t flags = 0;
    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        const int more = i + 1 < argc;
        if (!strcmp(a, "--device") && more)
            o.device = argv[++i];
        else if (!strcmp(a, "--seconds") && more)
            o.seconds = atof(argv[++i]);
        else if (!strcmp(a, "--volts") && more)
            o.volts = atof(argv[++i]);
        else if (!strcmp(a, "--supply") && more) {
            const char *v = argv[++i];
            o.supply = !strcmp(v, "external") ? LMB_SUPPLY_EXTERNAL : !strcmp(v, "internal") ? LMB_SUPPLY_INTERNAL : LMB_SUPPLY_NONE;
            if (o.supply == LMB_SUPPLY_NONE) {
                usage();
                return 2;
            }
        } else if (!strcmp(a, "--power"))
            o.power = 1;
        else if (!strcmp(a, "--csv"))
            o.csv = 1;
        else if (!strcmp(a, "--no-sim"))
            flags |= LMB_ENUM_NO_SIMULATED;
        else if (!strcmp(a, "--sim-only"))
            flags |= LMB_ENUM_ONLY_SIMULATED;
        else {
            usage();
            return 2;
        }
    }
    const int is_list = !strcmp(cmd, "list"), is_info = !strcmp(cmd, "info"), is_stream = !strcmp(cmd, "stream");
    if (!is_list && !is_info && !is_stream) {
        usage();
        return 2;
    }
    if (!is_list && (!o.device || o.seconds <= 0.0 || !isfinite(o.seconds))) {
        usage();
        return 2;
    }

    lmb_context *ctx = NULL;
    int rc = lmb_context_new(LMB_ABI_VERSION, &ctx);
    if (rc == LMB_OK)
        rc = lmb_register_builtin_families(ctx);
    if (rc != LMB_OK) {
        lmb_context_free(ctx);
        return fail("start", rc);
    }
    int status = 0;
    if (is_list) {
        status = cmd_list(ctx, flags);
    } else {
        lmb_device *dev = NULL;
        rc = lmb_device_open(ctx, o.device, &dev);
        if (rc != LMB_OK) {
            status = fail(o.device, rc);
        } else {
            status = is_info ? cmd_info(dev) : cmd_stream(dev, &o);
            lmb_device_close(dev);
        }
    }
    lmb_context_free(ctx);
    return status;
}
