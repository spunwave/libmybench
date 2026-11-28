/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* The PPK2 family through the generic device API, on the in-process simulator: registry,
 * facets, power order, streaming (mean current, no gaps, aligned logic), gaps, a slow reader,
 * a lost device, and two threads on one device. Runs in real time (100 kS/s). */

#include "libmybench/device.h"
#include "libmybench/facets.h"
#include "libmybench/ppk2_family.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

static int g_failures, g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                              \
    } while (0)

#define CHECK_RC(expr, want)                                                           \
    do {                                                                               \
        const int rc_ = (expr);                                                        \
        g_checks++;                                                                    \
        if (rc_ != (want)) {                                                           \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: %s = %d (%s), expected %d\n", __FILE__, __LINE__,  \
                    #expr, rc_, lmb_strerror(rc_), (want));                            \
        }                                                                              \
    } while (0)

typedef struct bench {
    lmb_context *ctx;
    lmb_device *dev;
    const lmb_power_supply *ps;
    const lmb_current_stream *cs;
    const lmb_logic_input *li;
} bench;

static int bench_open(bench *b, const char *id)
{
    memset(b, 0, sizeof *b);
    if (lmb_context_new(LMB_ABI_VERSION, &b->ctx) != LMB_OK || lmb_ppk2_register(b->ctx) != LMB_OK)
        return -1;
    if (lmb_device_open(b->ctx, id, &b->dev) != LMB_OK)
        return -1;
    return lmb_device_power_supply(b->dev, &b->ps) == LMB_OK && lmb_device_current_stream(b->dev, &b->cs) == LMB_OK &&
                   lmb_device_logic_input(b->dev, &b->li) == LMB_OK
               ? 0
               : -1;
}

static void bench_close(bench *b)
{
    lmb_device_close(b->dev);
    lmb_context_free(b->ctx);
}

/* Reads n current samples (stops after 3 s without any). Returns how many arrived. */
static size_t collect(const lmb_current_stream *cs, lmb_current_sample *out, size_t n, uint64_t *first)
{
    size_t total = 0;
    int idle = 0;
    while (total < n && idle < 30) {
        size_t got = 0;
        uint64_t f = 0;
        if (cs->read(cs->self, out + total, n - total, 100, &got, &f) != LMB_OK)
            break;
        if (total == 0 && got && first)
            *first = f;
        idle = got ? 0 : idle + 1;
        total += got;
    }
    return total;
}

static void test_registry(void)
{
    lmb_context *ctx;
    CHECK_RC(lmb_context_new(LMB_ABI_VERSION, &ctx), LMB_OK);
    CHECK_RC(lmb_ppk2_register(ctx), LMB_OK);
    CHECK_RC(lmb_ppk2_register(ctx), LMB_ERR_EXISTS);

    size_t n = 0;
    CHECK_RC(lmb_enumerate(ctx, LMB_ENUM_ONLY_SIMULATED, NULL, 0, &n), LMB_OK);
    CHECK(n == 1);
    lmb_device_info info;
    CHECK_RC(lmb_enumerate(ctx, LMB_ENUM_ONLY_SIMULATED, &info, 1, &n), LMB_OK);
    CHECK(strcmp(info.id, "sim:ppk2") == 0 && strcmp(info.family, "ppk2") == 0 && info.simulated == 1);
    CHECK(strcmp(info.transport, "sim") == 0);
    CHECK(info.facets == (LMB_FACET_BIT(LMB_FACET_POWER_SUPPLY) | LMB_FACET_BIT(LMB_FACET_CURRENT_STREAM) |
                          LMB_FACET_BIT(LMB_FACET_LOGIC_INPUT)));
    /* hardware (none on a build machine, or real ones: either is fine) is listed apart from the simulator */
    size_t hw = 99;
    CHECK_RC(lmb_enumerate(ctx, LMB_ENUM_NO_SIMULATED, NULL, 0, &hw), LMB_OK);
    CHECK(hw < 99);
    size_t all = 0;
    CHECK_RC(lmb_enumerate(ctx, 0, NULL, 0, &all), LMB_OK);
    CHECK(all == hw + 1);

    lmb_device *dev = NULL, *second = NULL, *other = NULL;
    CHECK_RC(lmb_device_open(ctx, "sim:ppk2", &dev), LMB_OK);
    CHECK_RC(lmb_device_open(ctx, "sim:ppk2", &second), LMB_ERR_BUSY);
    CHECK_RC(lmb_device_open(ctx, "sim:ppk2:2", &other), LMB_OK); /* a second, independent simulator */
    CHECK_RC(lmb_device_open(ctx, "sim:ppk2:", &second), LMB_ERR_NOT_FOUND);
    CHECK_RC(lmb_device_open(ctx, "sim:other", &second), LMB_ERR_NOT_FOUND);
    CHECK_RC(lmb_device_open(ctx, "ppk2:nonexistent-serial", &second), LMB_ERR_NOT_FOUND);

    /* the facets the PPK2 has, and the ones it does not */
    const void *p;
    CHECK_RC(lmb_device_facet(dev, LMB_FACET_POWER_SUPPLY, &p), LMB_OK);
    CHECK_RC(lmb_device_facet(dev, LMB_FACET_CURRENT_STREAM, &p), LMB_OK);
    CHECK_RC(lmb_device_facet(dev, LMB_FACET_LOGIC_INPUT, &p), LMB_OK);
    CHECK_RC(lmb_device_facet(dev, LMB_FACET_ANALOG_INPUT, &p), LMB_ERR_NO_FACET);
    CHECK_RC(lmb_device_facet(dev, LMB_FACET_ANALOG_OUTPUT, &p), LMB_ERR_NO_FACET);
    CHECK_RC(lmb_device_facet(dev, LMB_FACET_DIGITAL_IO, &p), LMB_ERR_NO_FACET);
    CHECK_RC(lmb_device_facet(dev, (lmb_facet_kind)77, &p), LMB_ERR_INVALID_ARG);
    const lmb_current_stream *cs;
    CHECK_RC(lmb_device_current_stream(dev, &cs), LMB_OK);
    CHECK(cs->abi_version == LMB_ABI_VERSION && LMB_HAS(cs, read) && LMB_HAS(cs, get_stats));

    lmb_stream_info si = LMB_SIZED(lmb_stream_info);
    CHECK_RC(cs->get_info(cs->self, &si), LMB_OK);
    CHECK(si.sample_rate_hz == 100000.0 && (si.flags & LMB_STREAM_GAP_DETECTION));
    const lmb_logic_input *li;
    CHECK_RC(lmb_device_logic_input(dev, &li), LMB_OK);
    CHECK_RC(li->get_info(li->self, &si), LMB_OK);
    CHECK(si.lines == 8);

    /* simulator controls refuse a device that is not one of ours */
    CHECK_RC(lmb_ppk2_sim_set_load(NULL, 1e-3), LMB_ERR_INVALID_ARG);

    lmb_device_close(other);
    lmb_device_close(dev);
    lmb_context_free(ctx);
}

static void test_power_order(void)
{
    bench b;
    CHECK(bench_open(&b, "sim:ppk2") == 0);
    lmb_power_supply_info pi = LMB_SIZED(lmb_power_supply_info);
    CHECK_RC(b.ps->get_info(b.ps->self, &pi), LMB_OK);
    CHECK(pi.modes == ((1u << LMB_SUPPLY_EXTERNAL) | (1u << LMB_SUPPLY_INTERNAL)));
    CHECK(pi.min_v == 0.8 && pi.max_v == 5.0);

    lmb_power_supply_state st = LMB_SIZED(lmb_power_supply_state);
    CHECK_RC(b.ps->get_state(b.ps->self, &st), LMB_OK);
    CHECK(st.mode == LMB_SUPPLY_NONE && st.output_on == 0 && isnan(st.voltage_v));

    /* the safe order is enforced by the library: no output before a mode and a voltage */
    CHECK_RC(b.ps->set_output(b.ps->self, 1), LMB_ERR_STATE);
    CHECK_RC(b.ps->set_mode(b.ps->self, LMB_SUPPLY_NONE), LMB_ERR_INVALID_ARG);
    CHECK_RC(b.ps->set_mode(b.ps->self, LMB_SUPPLY_INTERNAL), LMB_OK);
    CHECK_RC(b.ps->set_output(b.ps->self, 1), LMB_ERR_STATE); /* no voltage yet */
    double applied = 0;
    CHECK_RC(b.ps->set_voltage(b.ps->self, 9.0, &applied), LMB_OK);
    CHECK(applied == 5.0); /* clamped to the hardware maximum */
    CHECK_RC(b.ps->set_voltage(b.ps->self, 0.1, &applied), LMB_OK);
    CHECK(applied == 0.8);
    CHECK_RC(b.ps->set_voltage(b.ps->self, NAN, &applied), LMB_ERR_INVALID_ARG);
    CHECK_RC(b.ps->set_voltage(b.ps->self, -1.0, &applied), LMB_ERR_INVALID_ARG);
    CHECK_RC(b.ps->set_voltage(b.ps->self, 3.3, &applied), LMB_OK);
    CHECK(fabs(applied - 3.3) < 1e-9);
    CHECK_RC(b.ps->set_output(b.ps->self, 1), LMB_OK);
    CHECK_RC(b.ps->get_state(b.ps->self, &st), LMB_OK);
    CHECK(st.mode == LMB_SUPPLY_INTERNAL && st.output_on == 1 && fabs(st.voltage_v - 3.3) < 1e-9);

    /* mode and voltage are frozen while a stream runs; the output switch is not */
    CHECK_RC(b.cs->start(b.cs->self), LMB_OK);
    CHECK_RC(b.cs->start(b.cs->self), LMB_ERR_STATE); /* twice */
    CHECK_RC(b.ps->set_mode(b.ps->self, LMB_SUPPLY_EXTERNAL), LMB_ERR_STATE);
    CHECK_RC(b.ps->set_voltage(b.ps->self, 3.0, NULL), LMB_ERR_STATE);
    CHECK_RC(b.ps->set_output(b.ps->self, 0), LMB_OK);
    CHECK_RC(b.cs->stop(b.cs->self), LMB_OK);
    CHECK_RC(b.cs->stop(b.cs->self), LMB_OK); /* idempotent */
    CHECK_RC(b.ps->set_mode(b.ps->self, LMB_SUPPLY_EXTERNAL), LMB_OK);
    bench_close(&b);
}

static void test_stream(void)
{
    bench b;
    CHECK(bench_open(&b, "sim:ppk2") == 0);
    CHECK_RC(lmb_ppk2_sim_set_load(b.dev, 2.5e-3), LMB_OK);

    /* not started: reads are refused */
    lmb_current_sample s1[16];
    size_t got = 7;
    CHECK_RC(b.cs->read(b.cs->self, s1, 16, 0, &got, NULL), LMB_ERR_STATE);
    CHECK(got == 0);
    CHECK_RC(b.cs->read(b.cs->self, s1, 16, -1, &got, NULL), LMB_ERR_INVALID_ARG);
    CHECK_RC(b.cs->read(b.cs->self, NULL, 16, 0, &got, NULL), LMB_ERR_INVALID_ARG);

    /* A plain start measures: it picks the external-supply mode and 3.3 V on its own. The DUT is
     * connected with the output switch, as in a real unit. */
    lmb_power_supply_state st = LMB_SIZED(lmb_power_supply_state);
    CHECK_RC(b.ps->set_mode(b.ps->self, LMB_SUPPLY_EXTERNAL), LMB_OK);
    CHECK_RC(b.ps->set_voltage(b.ps->self, 3.3, NULL), LMB_OK);
    CHECK_RC(b.ps->set_output(b.ps->self, 1), LMB_OK);
    CHECK_RC(b.cs->start(b.cs->self), LMB_OK);
    CHECK_RC(b.li->start(b.li->self), LMB_OK);

    enum { N = 30000 }; /* 0.3 s */
    lmb_current_sample *cur = malloc(N * sizeof *cur);
    uint64_t first = 99;
    const size_t n = collect(b.cs, cur, N, &first);
    CHECK(n == N);
    CHECK(first == 0); /* the timeline starts at the first sample of the acquisition */
    double sum = 0;
    size_t bad = 0;
    for (size_t i = 0; i < n; i++) {
        if (cur[i].flags)
            bad++;
        else
            sum += cur[i].current_a;
    }
    CHECK(bad == 0);
    if (n > bad)
        CHECK(fabs(sum / (double)(n - bad) - 2.5e-3) < 2.5e-5);
    lmb_stream_stats stats = LMB_SIZED(lmb_stream_stats);
    CHECK_RC(b.cs->get_stats(b.cs->self, &stats), LMB_OK);
    CHECK(stats.delivered == n && stats.gaps == 0 && stats.lost == 0);

    /* The logic facet joined the same acquisition: it started at timeline index 0 as well, and
     * the simulator's lines count up every 1000 samples. */
    lmb_logic_sample *lg = malloc(N * sizeof *lg);
    size_t lgot = 0, ltotal = 0;
    uint64_t lfirst = 99;
    while (ltotal < N) {
        uint64_t f = 0;
        CHECK_RC(b.li->read(b.li->self, lg + ltotal, N - ltotal, 100, &lgot, &f), LMB_OK);
        if (ltotal == 0)
            lfirst = f;
        if (lgot == 0)
            break;
        ltotal += lgot;
    }
    CHECK(ltotal == N && lfirst == 0);
    int counting = ltotal == N;
    for (size_t i = 0; counting && i < ltotal; i++)
        counting = lg[i].levels == (uint32_t)(i / 1000u) && lg[i].flags == 0;
    CHECK(counting);

    /* a stream of the same device from another start: the second start gets a fresh position */
    CHECK_RC(b.cs->stop(b.cs->self), LMB_OK);
    CHECK_RC(b.cs->read(b.cs->self, cur, 16, 0, &got, NULL), LMB_ERR_STATE);
    CHECK_RC(b.cs->start(b.cs->self), LMB_OK); /* the logic facet still runs: the acquisition continues */
    first = 0;
    CHECK(collect(b.cs, cur, 100, &first) == 100);
    CHECK(first >= N); /* joined the running timeline, not restarted */
    CHECK_RC(b.cs->stop(b.cs->self), LMB_OK);
    CHECK_RC(b.li->stop(b.li->self), LMB_OK);

    /* closing leaves the unit in its safe state: nothing observable here, but it must not hang */
    free(cur);
    free(lg);
    CHECK_RC(b.ps->get_state(b.ps->self, &st), LMB_OK);
    bench_close(&b);
}

static void test_gap(void)
{
    bench b;
    CHECK(bench_open(&b, "sim:ppk2") == 0);
    CHECK_RC(b.ps->set_output(b.ps->self, 0), LMB_OK);
    CHECK_RC(b.cs->start(b.cs->self), LMB_OK);
    lmb_current_sample *s = malloc(20000 * sizeof *s);
    uint64_t first = 0;
    CHECK(collect(b.cs, s, 3000, &first) == 3000);
    CHECK_RC(lmb_ppk2_sim_drop(b.dev, 10), LMB_OK);
    /* read on: exactly one sample carries the gap, with the count lost; the timeline skips them */
    size_t gaps = 0, seen = 0;
    uint64_t gap_lost = 0;
    uint64_t next_index = 3000;
    int index_ok = 1;
    while (seen < 6000) {
        size_t got = 0;
        uint64_t f = 0;
        CHECK_RC(b.cs->read(b.cs->self, s, 1000, 100, &got, &f), LMB_OK);
        if (got == 0)
            break;
        for (size_t i = 0; i < got; i++) {
            if (s[i].flags & LMB_SAMPLE_GAP) {
                gaps++;
                gap_lost += s[i].lost;
                if (i == 0)
                    index_ok = index_ok && f == next_index + 10;
            } else if (i == 0) {
                index_ok = index_ok && f == next_index;
            }
        }
        /* continuity of the timeline between reads, counting the lost samples */
        next_index = f + got;
        for (size_t i = 1; i < got; i++)
            if (s[i].flags & LMB_SAMPLE_GAP)
                next_index += s[i].lost;
        seen += got;
    }
    CHECK(gaps == 1 && gap_lost == 10);
    CHECK(index_ok);
    lmb_stream_stats stats = LMB_SIZED(lmb_stream_stats);
    CHECK_RC(b.cs->get_stats(b.cs->self, &stats), LMB_OK);
    CHECK(stats.gaps == 1 && stats.lost == 10);
    free(s);
    bench_close(&b);
}

static void test_slow_reader(void)
{
    /* The logic facet is read, the current facet is not: its position falls out of the ring
     * (about 1.3 s) and its next read reports the loss instead of silently skipping. */
    bench b;
    CHECK(bench_open(&b, "sim:ppk2") == 0);
    CHECK_RC(b.cs->start(b.cs->self), LMB_OK);
    CHECK_RC(b.li->start(b.li->self), LMB_OK);
    lmb_logic_sample *lg = malloc(8192 * sizeof *lg);
    size_t total = 0;
    while (total < 160000) { /* 1.6 s of the logic stream */
        size_t got = 0;
        CHECK_RC(b.li->read(b.li->self, lg, 8192, 100, &got, NULL), LMB_OK);
        if (got == 0)
            break;
        total += got;
    }
    CHECK(total >= 160000);
    lmb_current_sample s[64];
    size_t got = 0;
    uint64_t first = 0;
    CHECK_RC(b.cs->read(b.cs->self, s, 64, 100, &got, &first), LMB_OK);
    CHECK(got == 64);
    CHECK((s[0].flags & LMB_SAMPLE_GAP) && s[0].lost > 20000);
    CHECK(first > 20000); /* the oldest samples are gone: the timeline index says where it resumed */
    CHECK(!(s[1].flags & LMB_SAMPLE_GAP));
    lmb_stream_stats stats = LMB_SIZED(lmb_stream_stats);
    CHECK_RC(b.cs->get_stats(b.cs->self, &stats), LMB_OK);
    CHECK(stats.gaps == 1 && stats.lost == s[0].lost);
    free(lg);
    bench_close(&b);
}

static void test_gone_and_reopen(void)
{
    bench b;
    CHECK(bench_open(&b, "sim:ppk2") == 0);
    CHECK_RC(b.cs->start(b.cs->self), LMB_OK);
    CHECK_RC(lmb_device_status(b.dev), LMB_OK);
    CHECK_RC(lmb_ppk2_sim_disconnect(b.dev), LMB_OK);
    CHECK_RC(lmb_device_status(b.dev), LMB_ERR_DISCONNECTED);
    lmb_current_sample s[8];
    size_t got = 1;
    CHECK_RC(b.cs->read(b.cs->self, s, 8, 10, &got, NULL), LMB_ERR_DISCONNECTED);
    CHECK_RC(b.cs->start(b.cs->self), LMB_ERR_DISCONNECTED);
    CHECK_RC(b.li->start(b.li->self), LMB_ERR_DISCONNECTED);
    CHECK_RC(b.ps->set_output(b.ps->self, 1), LMB_ERR_DISCONNECTED);
    CHECK_RC(b.ps->set_voltage(b.ps->self, 3.0, NULL), LMB_ERR_DISCONNECTED);
    lmb_power_supply_state st = LMB_SIZED(lmb_power_supply_state);
    CHECK_RC(b.ps->get_state(b.ps->self, &st), LMB_ERR_DISCONNECTED);
    CHECK_RC(b.cs->stop(b.cs->self), LMB_OK); /* stopping always works */

    /* close frees the id, a new open works and is healthy */
    lmb_device_close(b.dev);
    CHECK_RC(lmb_device_open(b.ctx, "sim:ppk2", &b.dev), LMB_OK);
    CHECK_RC(lmb_device_status(b.dev), LMB_OK);
    CHECK_RC(lmb_device_current_stream(b.dev, &b.cs), LMB_OK);
    CHECK_RC(b.cs->start(b.cs->self), LMB_OK);
    CHECK(collect(b.cs, s, 8, NULL) == 8);
    bench_close(&b);
}

#if !defined(_WIN32)
/* The power switch is used from one thread while another thread blocks in a stream read. */
typedef struct reader {
    const lmb_current_stream *cs;
    size_t total;
    int rc;
} reader;

static void *read_thread(void *arg)
{
    reader *r = arg;
    lmb_current_sample buf[512];
    while (r->total < 20000 && r->rc == LMB_OK) {
        size_t got = 0;
        r->rc = r->cs->read(r->cs->self, buf, 512, 100, &got, NULL);
        r->total += got;
    }
    return NULL;
}

static void test_threads(void)
{
    bench b;
    CHECK(bench_open(&b, "sim:ppk2") == 0);
    CHECK_RC(b.cs->start(b.cs->self), LMB_OK);
    reader r = {b.cs, 0, LMB_OK};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, read_thread, &r) == 0);
    int ok = 1;
    for (int i = 0; i < 40; i++) {
        ok = ok && b.ps->set_output(b.ps->self, i & 1) == LMB_OK;
        lmb_power_supply_state st = LMB_SIZED(lmb_power_supply_state);
        ok = ok && b.ps->get_state(b.ps->self, &st) == LMB_OK;
    }
    pthread_join(t, NULL);
    CHECK(ok);
    CHECK(r.rc == LMB_OK && r.total >= 20000);
    bench_close(&b);
}
#endif

int main(void)
{
    test_registry();
    test_power_order();
    test_stream();
    test_gap();
    test_slow_reader();
    test_gone_and_reopen();
#if !defined(_WIN32)
    test_threads();
#endif
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
