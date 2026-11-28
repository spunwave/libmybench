/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* The PPK2 family of the device model: facets over ppk2_dev (hardware) or over the in-process
 * simulator, with one sample ring shared by the stream facets. See ppk2_family.h. */

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS /* getenv, for LMB_PPK2_SIMULATORS */
#endif

#include "libmybench/ppk2_family.h"

#include "libmybench/driver.h"
#include "libmybench/facets.h"
#include "libmybench/ppk2.h"
#ifdef LMB_PPK2_HAVE_SERIAL
#include "libmybench/ppk2_dev.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FAMILY "ppk2"
#define SIM_ID "sim:ppk2"
#define RING_BITS 17 /* 131072 samples: 1.3 s at 100 kS/s */
#define RING_SIZE ((uint64_t)1 << RING_BITS)
#define POLL_MAX 2048 /* samples per backend poll */
#define DEFAULT_MV 3300u
#define FACETS (LMB_FACET_BIT(LMB_FACET_POWER_SUPPLY) | LMB_FACET_BIT(LMB_FACET_CURRENT_STREAM) | \
                LMB_FACET_BIT(LMB_FACET_LOGIC_INPUT))
#define SIM_RATE_HZ 100000.0 /* the simulated unit's sample rate */

typedef struct entry {
    double current_a;
    uint64_t tl; /* index on the acquisition's timeline, lost samples included */
    uint32_t lost;
    uint8_t logic;
    uint8_t flags; /* LMB_SAMPLE_* */
} entry;

typedef struct cursor {
    int active;
    uint64_t pos; /* next ring position to read */
    lmb_stream_stats st;
} cursor;

typedef struct p2dev {
    lmb_mutex *mu; /* guards everything below */
    int sim;
    int gone; /* the unit went away */

    /* hardware */
    #ifdef LMB_PPK2_HAVE_SERIAL
    ppk2_dev *hw;
    #endif
    /* simulator: the same command builder, simulator and decoder the hardware path uses */
    ppk2_ctl *sctl;
    ppk2_sim *ssim;
    ppk2_stream *sstream;
    ppk2_cal scal;
    double load_a;
    double pulse_peak_a, pulse_period_s, pulse_duty; /* a pulsed load when pulse_period_s > 0 */
    double last_t, carry;

    /* acquisition shared by the stream facets */
    int running;
    entry *ring;
    uint64_t head;    /* ring positions written so far */
    uint64_t tl_next;
    cursor cs, li;
    ppk2_sample tmp[POLL_MAX + 8];
    uint8_t wire[POLL_MAX * 4];

    lmb_power_supply ps;
    lmb_current_stream cstream;
    lmb_logic_input linput;
    lmb_sim_control sctrl; /* simulated units only */
} p2dev;

/* ---------------------------------------------------------------------------------------- */

static int map(int rc)
{
    switch (rc) {
    case PPK2_OK: return LMB_OK;
    case PPK2_ERR_INVALID_ARG:
    case PPK2_ERR_NO_SPACE: return LMB_ERR_INVALID_ARG;
    case PPK2_ERR_STATE: return LMB_ERR_STATE;
    case PPK2_ERR_NO_MEM: return LMB_ERR_NO_MEM;
    case PPK2_ERR_UNSUPPORTED: return LMB_ERR_UNSUPPORTED;
    case PPK2_ERR_TIMEOUT: return LMB_ERR_TIMEOUT;
    case PPK2_ERR_DISCONNECTED: return LMB_ERR_DISCONNECTED;
    #ifdef LMB_PPK2_HAVE_SERIAL
    case PPK2_ERR_NOT_FOUND: return LMB_ERR_NOT_FOUND;
    case PPK2_ERR_ACCESS: return LMB_ERR_ACCESS;
    case PPK2_ERR_BUSY: return LMB_ERR_BUSY;
    #endif
    default: return LMB_ERR_IO;
    }
}

/* Result of a backend call, remembering that the unit is gone. Caller holds the lock. */
static int note(p2dev *d, int rc)
{
    const int r = map(rc);
    if (r == LMB_ERR_DISCONNECTED)
        d->gone = 1;
    return r;
}

static const ppk2_ctl *ctl_of(const p2dev *d)
{
    #ifdef LMB_PPK2_HAVE_SERIAL
    if (d->hw)
        return ppk2_dev_ctl(d->hw);
    #endif
    return d->sctl;
}

/* ---- simulator backend ---- */

static double sim_current(void *user, uint64_t i)
{
    const p2dev *d = user;
    if (d->pulse_period_s > 0.0) {
        const double t = (double)i / SIM_RATE_HZ;
        const double phase = fmod(t, d->pulse_period_s) / d->pulse_period_s;
        return phase < d->pulse_duty ? d->pulse_peak_a : d->load_a;
    }
    return d->load_a;
}

static uint8_t sim_logic(void *user, uint64_t i)
{
    (void)user;
    return (uint8_t)(i / 1000u); /* D0-D7 count up every 1000 samples */
}

static int sim_send(p2dev *d, int built, const ppk2_tx *tx)
{
    if (built != PPK2_OK)
        return built;
    for (size_t i = 0; i < tx->count; i++) {
        const int rc = ppk2_sim_write(d->ssim, tx->frames[i].bytes, tx->frames[i].len);
        if (rc != PPK2_OK) {
            ppk2_ctl_invalidate(d->sctl);
            return rc;
        }
    }
    return PPK2_OK;
}

static int sim_init(p2dev *d)
{
    ppk2_cal cal;
    ppk2_cal_default(&cal);
    cal.missing = 0; /* a fully calibrated unit: every coefficient in its metadata */
    cal.zero = 0;
    d->sctl = ppk2_ctl_new(0);
    d->ssim = ppk2_sim_new(&cal);
    d->sstream = ppk2_stream_new();
    ppk2_meta_parser *mp = ppk2_meta_parser_new();
    int rc = (d->sctl && d->ssim && d->sstream && mp) ? PPK2_OK : PPK2_ERR_NO_MEM;
    d->load_a = 1e-3;
    if (rc == PPK2_OK) {
        ppk2_sim_set_load(d->ssim, sim_current, sim_logic, d);
        /* The same opening as the device layer: stop, then ask for the metadata. */
        ppk2_tx tx;
        rc = sim_send(d, ppk2_ctl_stop(d->sctl, &tx), &tx);
        if (rc == PPK2_OK)
            rc = sim_send(d, ppk2_ctl_request_metadata(d->sctl, &tx), &tx);
        if (rc == PPK2_OK) {
            const size_t n = ppk2_sim_read(d->ssim, d->wire, sizeof d->wire, 0);
            ppk2_meta_parser_feed(mp, d->wire, n);
            if (!ppk2_meta_parser_done(mp))
                rc = PPK2_ERR_FORMAT;
        }
        if (rc == PPK2_OK)
            ppk2_cal_from_metadata(&d->scal, ppk2_meta_parser_result(mp));
    }
    ppk2_meta_parser_free(mp);
    return rc;
}

/* ---- backend calls (the lock is held) ---- */

static int b_select_supply(p2dev *d, ppk2_supply s)
{
    ppk2_tx tx;
    #ifdef LMB_PPK2_HAVE_SERIAL
    if (d->hw)
        return ppk2_dev_select_supply(d->hw, s);
    #endif
    return sim_send(d, ppk2_ctl_select_supply(d->sctl, s, &tx), &tx);
}

static int b_set_voltage(p2dev *d, unsigned mv, unsigned *applied)
{
    ppk2_tx tx;
    #ifdef LMB_PPK2_HAVE_SERIAL
    if (d->hw)
        return ppk2_dev_set_voltage(d->hw, mv, applied);
    #endif
    return sim_send(d, ppk2_ctl_set_voltage(d->sctl, mv, applied, &tx), &tx);
}

static int b_set_power(p2dev *d, int on)
{
    ppk2_tx tx;
    #ifdef LMB_PPK2_HAVE_SERIAL
    if (d->hw)
        return ppk2_dev_set_power(d->hw, on);
    #endif
    return sim_send(d, ppk2_ctl_set_power(d->sctl, on, &tx), &tx);
}

static int b_start(p2dev *d)
{
    ppk2_tx tx;
    #ifdef LMB_PPK2_HAVE_SERIAL
    if (d->hw)
        return ppk2_dev_start(d->hw);
    #endif
    ppk2_stream_reset(d->sstream);
    ppk2_stream_set_calibration(d->sstream, &d->scal, ppk2_ctl_source_volts(d->sctl));
    const int rc = sim_send(d, ppk2_ctl_start(d->sctl, &tx), &tx);
    d->last_t = lmb_monotonic_s();
    d->carry = 0.0;
    return rc;
}

static int b_stop(p2dev *d)
{
    ppk2_tx tx;
    #ifdef LMB_PPK2_HAVE_SERIAL
    if (d->hw)
        return ppk2_dev_stop(d->hw);
    #endif
    return sim_send(d, ppk2_ctl_stop(d->sctl, &tx), &tx);
}

/* Whatever is available now, never waiting: into d->tmp. */
static int b_poll(p2dev *d, size_t *got)
{
    *got = 0;
    #ifdef LMB_PPK2_HAVE_SERIAL
    if (d->hw)
        return ppk2_dev_read(d->hw, d->tmp, POLL_MAX, 0, got);
    #endif
    const double t = lmb_monotonic_s();
    double due = (t - d->last_t) * PPK2_SAMPLE_RATE + d->carry;
    d->last_t = t;
    if (due > 20000.0)
        due = 20000.0; /* after a stall, no burst of more than 0.2 s */
    uint64_t words = (uint64_t)due;
    if (words > POLL_MAX)
        words = POLL_MAX;
    d->carry = due - (double)words;
    const size_t n = ppk2_sim_read(d->ssim, d->wire, sizeof d->wire, words);
    size_t used = 0;
    *got = ppk2_stream_decode(d->sstream, d->wire, n, &used, d->tmp, POLL_MAX + 8);
    return PPK2_OK;
}

/* ---- acquisition ---- */

static void push(p2dev *d, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        const ppk2_sample *s = &d->tmp[i];
        entry e;
        e.flags = 0;
        e.lost = 0;
        if (s->flags & PPK2_SAMPLE_GAP) {
            e.flags |= LMB_SAMPLE_GAP;
            e.lost = s->missing;
            d->tl_next += s->missing;
        }
        if ((s->flags & PPK2_SAMPLE_BAD_RANGE) || !isfinite(s->current_a))
            e.flags |= LMB_SAMPLE_INVALID;
        e.current_a = s->current_a;
        e.logic = s->logic;
        e.tl = d->tl_next++;
        d->ring[d->head & (RING_SIZE - 1)] = e;
        d->head++;
    }
}

static int pump(p2dev *d)
{
    size_t got = 0;
    const int rc = note(d, b_poll(d, &got));
    if (rc == LMB_OK && got)
        push(d, got);
    return rc;
}

static int stream_start(p2dev *d, cursor *c)
{
    lmb_mutex_lock(d->mu);
    int rc = d->gone ? LMB_ERR_DISCONNECTED : (c->active ? LMB_ERR_STATE : LMB_OK);
    if (rc == LMB_OK && !d->running) {
        const ppk2_ctl *ctl = ctl_of(d);
        if (ppk2_ctl_supply(ctl) == PPK2_SUPPLY_NONE)
            rc = note(d, b_select_supply(d, PPK2_SUPPLY_AMMETER));
        if (rc == LMB_OK && isnan(ppk2_ctl_source_volts(ctl)))
            rc = note(d, b_set_voltage(d, DEFAULT_MV, NULL));
        if (rc == LMB_OK)
            rc = note(d, b_start(d));
        if (rc == LMB_OK) {
            d->running = 1;
            d->head = 0;
            d->tl_next = 0;
        }
    }
    if (rc == LMB_OK) {
        c->active = 1;
        c->pos = d->head;
        memset(&c->st, 0, sizeof c->st);
        c->st.size = sizeof c->st;
    }
    lmb_mutex_unlock(d->mu);
    return rc;
}

static int stream_stop(p2dev *d, cursor *c)
{
    lmb_mutex_lock(d->mu);
    int rc = LMB_OK;
    c->active = 0;
    if (d->running && !d->cs.active && !d->li.active) {
        d->running = 0;
        if (!d->gone)
            rc = note(d, b_stop(d));
    }
    lmb_mutex_unlock(d->mu);
    return rc;
}

/* Copy up to cap entries from the cursor into out (a lmb_current_sample or lmb_logic_sample
 * array), under the lock. */
static size_t take(p2dev *d, cursor *c, int logic, void *out, size_t cap, uint64_t *first)
{
    uint64_t skipped = 0;
    if (d->head - c->pos > RING_SIZE) { /* the reader fell behind: the oldest are gone */
        skipped = d->head - RING_SIZE - c->pos;
        c->pos += skipped;
        c->st.lost += skipped;
    }
    uint64_t n = d->head - c->pos;
    if (n > cap)
        n = cap;
    for (uint64_t i = 0; i < n; i++) {
        const entry *e = &d->ring[(c->pos + i) & (RING_SIZE - 1)];
        uint32_t flags = e->flags, lost = e->lost;
        if (i == 0 && skipped) {
            flags |= LMB_SAMPLE_GAP;
            lost = (skipped > UINT32_MAX - lost) ? UINT32_MAX : lost + (uint32_t)skipped;
        }
        if (flags & LMB_SAMPLE_GAP) {
            c->st.gaps++;
            if (!(i == 0 && skipped))
                c->st.lost += e->lost;
        }
        if (logic) {
            lmb_logic_sample *o = (lmb_logic_sample *)out + i;
            o->levels = e->logic;
            o->flags = flags;
            o->lost = lost;
            o->reserved = 0;
        } else {
            lmb_current_sample *o = (lmb_current_sample *)out + i;
            o->current_a = e->current_a;
            o->flags = flags;
            o->lost = lost;
        }
    }
    if (first && n)
        *first = d->ring[c->pos & (RING_SIZE - 1)].tl;
    c->pos += n;
    c->st.delivered += n;
    return (size_t)n;
}

static int stream_read(p2dev *d, cursor *c, int logic, void *out, size_t cap, int timeout_ms, size_t *got,
                       uint64_t *first)
{
    if (!got || (cap && !out) || timeout_ms < 0)
        return LMB_ERR_INVALID_ARG;
    *got = 0;
    const double deadline = lmb_monotonic_s() + (double)timeout_ms / 1000.0;
    for (;;) {
        lmb_mutex_lock(d->mu);
        int rc = d->gone ? LMB_ERR_DISCONNECTED : (c->active ? LMB_OK : LMB_ERR_STATE);
        if (rc == LMB_OK && cap > 0) {
            if (d->head == c->pos)
                rc = pump(d); /* nothing waiting: look at the device (never blocks) */
            if (rc == LMB_OK && d->head != c->pos) {
                *got = take(d, c, logic, out, cap, first);
                lmb_mutex_unlock(d->mu);
                return LMB_OK;
            }
        }
        lmb_mutex_unlock(d->mu);
        if (rc != LMB_OK)
            return rc;
        if (cap == 0 || lmb_monotonic_s() >= deadline)
            return LMB_OK;
        lmb_sleep_ms(1); /* unlocked: other facets keep working */
    }
}

/* ---- facets ---- */

static int cs_info(void *self, lmb_stream_info *out)
{
    (void)self;
    if (!out)
        return LMB_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->flags = LMB_STREAM_GAP_DETECTION;
    out->sample_rate_hz = PPK2_SAMPLE_RATE;
    return LMB_OK;
}

static int li_info(void *self, lmb_stream_info *out)
{
    const int rc = cs_info(self, out);
    if (rc == LMB_OK)
        out->lines = 8;
    return rc;
}

static int cs_start(void *self) { p2dev *d = self; return stream_start(d, &d->cs); }
static int cs_stop(void *self) { p2dev *d = self; return stream_stop(d, &d->cs); }
static int li_start(void *self) { p2dev *d = self; return stream_start(d, &d->li); }
static int li_stop(void *self) { p2dev *d = self; return stream_stop(d, &d->li); }

static int cs_read(void *self, lmb_current_sample *out, size_t cap, int timeout_ms, size_t *got, uint64_t *first)
{
    p2dev *d = self;
    return stream_read(d, &d->cs, 0, out, cap, timeout_ms, got, first);
}

static int li_read(void *self, lmb_logic_sample *out, size_t cap, int timeout_ms, size_t *got, uint64_t *first)
{
    p2dev *d = self;
    return stream_read(d, &d->li, 1, out, cap, timeout_ms, got, first);
}

static int stats_of(p2dev *d, const cursor *c, lmb_stream_stats *out)
{
    if (!out)
        return LMB_ERR_INVALID_ARG;
    lmb_mutex_lock(d->mu);
    *out = c->st;
    lmb_mutex_unlock(d->mu);
    out->size = sizeof *out;
    return LMB_OK;
}

static int cs_stats(void *self, lmb_stream_stats *out) { p2dev *d = self; return stats_of(d, &d->cs, out); }
static int li_stats(void *self, lmb_stream_stats *out) { p2dev *d = self; return stats_of(d, &d->li, out); }

static int ps_info(void *self, lmb_power_supply_info *out)
{
    (void)self;
    if (!out)
        return LMB_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->modes = (1u << LMB_SUPPLY_EXTERNAL) | (1u << LMB_SUPPLY_INTERNAL);
    out->min_v = PPK2_VOLTAGE_MIN_MV / 1000.0;
    out->max_v = PPK2_VOLTAGE_MAX_MV / 1000.0;
    out->step_v = 0.001;
    return LMB_OK;
}

/* Common checks of the supply calls: not gone, no stream running (when idle is set). */
static int ps_enter(p2dev *d, int idle)
{
    if (d->gone)
        return LMB_ERR_DISCONNECTED;
    return (idle && d->running) ? LMB_ERR_STATE : LMB_OK;
}

static int ps_set_mode(void *self, lmb_supply_mode mode)
{
    p2dev *d = self;
    if (mode != LMB_SUPPLY_EXTERNAL && mode != LMB_SUPPLY_INTERNAL)
        return LMB_ERR_INVALID_ARG;
    lmb_mutex_lock(d->mu);
    int rc = ps_enter(d, 1);
    if (rc == LMB_OK)
        rc = note(d, b_select_supply(d, mode == LMB_SUPPLY_EXTERNAL ? PPK2_SUPPLY_AMMETER : PPK2_SUPPLY_SOURCE));
    lmb_mutex_unlock(d->mu);
    return rc;
}

static int ps_set_voltage(void *self, double volts, double *applied)
{
    p2dev *d = self;
    if (!isfinite(volts) || volts <= 0.0)
        return LMB_ERR_INVALID_ARG;
    const unsigned mv = volts > 100.0 ? 100000u : (unsigned)(volts * 1000.0 + 0.5);
    lmb_mutex_lock(d->mu);
    int rc = ps_enter(d, 1);
    unsigned got = 0;
    if (rc == LMB_OK)
        rc = note(d, b_set_voltage(d, mv, &got));
    lmb_mutex_unlock(d->mu);
    if (rc == LMB_OK && applied)
        *applied = (double)got / 1000.0;
    return rc;
}

static int ps_set_output(void *self, int on)
{
    p2dev *d = self;
    lmb_mutex_lock(d->mu);
    int rc = ps_enter(d, 0);
    if (rc == LMB_OK)
        rc = note(d, b_set_power(d, on));
    lmb_mutex_unlock(d->mu);
    return rc;
}

static int ps_get_state(void *self, lmb_power_supply_state *out)
{
    p2dev *d = self;
    if (!out)
        return LMB_ERR_INVALID_ARG;
    memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    lmb_mutex_lock(d->mu);
    const ppk2_ctl *c = ctl_of(d);
    switch (ppk2_ctl_supply(c)) {
    case PPK2_SUPPLY_AMMETER: out->mode = LMB_SUPPLY_EXTERNAL; break;
    case PPK2_SUPPLY_SOURCE: out->mode = LMB_SUPPLY_INTERNAL; break;
    default: out->mode = LMB_SUPPLY_NONE; break;
    }
    out->voltage_v = ppk2_ctl_source_volts(c);
    out->output_on = ppk2_ctl_power_on(c) == 1;
    const int rc = d->gone ? LMB_ERR_DISCONNECTED : LMB_OK;
    lmb_mutex_unlock(d->mu);
    return rc;
}

/* ---- the open device ---- */

static void dev_close(void *drv)
{
    p2dev *d = drv;
    if (!d)
        return;
    lmb_mutex_lock(d->mu);
    if (d->running && !d->gone)
        (void)b_stop(d);
    d->running = 0;
    lmb_mutex_unlock(d->mu);
    #ifdef LMB_PPK2_HAVE_SERIAL
    ppk2_dev_close(d->hw); /* stops the stream, gate off */
    #endif
    ppk2_ctl_free(d->sctl);
    ppk2_sim_free(d->ssim);
    ppk2_stream_free(d->sstream);
    lmb_mutex_free(d->mu);
    free(d->ring);
    free(d);
}

static int dev_facet(void *drv, lmb_facet_kind kind, const void **out)
{
    p2dev *d = drv;
    switch (kind) {
    case LMB_FACET_POWER_SUPPLY: *out = &d->ps; return LMB_OK;
    case LMB_FACET_CURRENT_STREAM: *out = &d->cstream; return LMB_OK;
    case LMB_FACET_LOGIC_INPUT: *out = &d->linput; return LMB_OK;
    case LMB_FACET_SIM_CONTROL:
        if (!d->sim)
            return LMB_ERR_NO_FACET;
        *out = &d->sctrl;
        return LMB_OK;
    default: return LMB_ERR_NO_FACET;
    }
}

static int dev_status(void *drv)
{
    p2dev *d = drv;
    lmb_mutex_lock(d->mu);
    const int rc = d->gone ? LMB_ERR_DISCONNECTED : LMB_OK;
    lmb_mutex_unlock(d->mu);
    return rc;
}

/* ---- simulator controls (the SimControl facet; the lmb_ppk2_sim_* functions below stay for compatibility) ---- */

static int sc_set_load(void *self, double amps)
{
    p2dev *d = self;
    if (!isfinite(amps) || amps < 0.0)
        return LMB_ERR_INVALID_ARG;
    lmb_mutex_lock(d->mu);
    d->load_a = amps;
    d->pulse_period_s = 0.0;
    lmb_mutex_unlock(d->mu);
    return LMB_OK;
}

static int sc_set_load_pulses(void *self, double base_a, double peak_a, double period_s, double duty)
{
    p2dev *d = self;
    if (!isfinite(base_a) || !isfinite(peak_a) || base_a < 0.0 || peak_a < 0.0 || !isfinite(period_s) ||
        period_s <= 0.0 || !isfinite(duty) || duty < 0.0 || duty > 1.0)
        return LMB_ERR_INVALID_ARG;
    lmb_mutex_lock(d->mu);
    d->load_a = base_a;
    d->pulse_peak_a = peak_a;
    d->pulse_period_s = period_s;
    d->pulse_duty = duty;
    lmb_mutex_unlock(d->mu);
    return LMB_OK;
}

static int sc_drop(void *self, uint64_t n)
{
    p2dev *d = self;
    lmb_mutex_lock(d->mu);
    ppk2_sim_drop(d->ssim, n);
    lmb_mutex_unlock(d->mu);
    return LMB_OK;
}

static int sc_disconnect(void *self)
{
    p2dev *d = self;
    lmb_mutex_lock(d->mu);
    d->gone = 1;
    lmb_mutex_unlock(d->mu);
    return LMB_OK;
}

static int sc_reconnect(void *self)
{
    p2dev *d = self;
    lmb_mutex_lock(d->mu);
    d->gone = 0;
    lmb_mutex_unlock(d->mu);
    return LMB_OK;
}

static p2dev *dev_new(void)
{
    p2dev *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->mu = lmb_mutex_new();
    d->ring = malloc(RING_SIZE * sizeof *d->ring);
    if (!d->mu || !d->ring) {
        lmb_mutex_free(d->mu);
        free(d->ring);
        free(d);
        return NULL;
    }
    d->ps = (lmb_power_supply){LMB_ABI_VERSION, sizeof d->ps, d, ps_info, ps_set_mode, ps_set_voltage,
                               ps_set_output, ps_get_state};
    d->cstream = (lmb_current_stream){LMB_ABI_VERSION, sizeof d->cstream, d, cs_info, cs_start, cs_stop,
                                      cs_read, cs_stats};
    d->linput = (lmb_logic_input){LMB_ABI_VERSION, sizeof d->linput, d, li_info, li_start, li_stop,
                                  li_read, li_stats};
    d->sctrl = (lmb_sim_control){LMB_ABI_VERSION, sizeof d->sctrl, d, sc_set_load, sc_drop, sc_disconnect,
                                 sc_reconnect, sc_set_load_pulses};
    return d;
}

static void fill_ops(p2dev *d, lmb_device_ops *ops)
{
    ops->drv = d;
    ops->close = dev_close;
    ops->facet = dev_facet;
    ops->status = dev_status;
}

static void set_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

/* ---- family: simulator ---- */

static int is_sim_id(const char *id)
{
    if (strcmp(id, SIM_ID) == 0)
        return 1;
    return strncmp(id, SIM_ID ":", sizeof SIM_ID) == 0 && id[sizeof SIM_ID] != '\0';
}

static void sim_info(lmb_device_info *info, const char *id)
{
    memset(info, 0, sizeof *info);
    set_str(info->id, sizeof info->id, id);
    set_str(info->family, sizeof info->family, FAMILY);
    if (strcmp(id, SIM_ID) == 0)
        set_str(info->name, sizeof info->name, "Simulated PPK2");
    else
        snprintf(info->name, sizeof info->name, "Simulated PPK2 (%.30s)", id + sizeof SIM_ID);
    set_str(info->serial, sizeof info->serial, "SIM");
    set_str(info->transport, sizeof info->transport, "sim");
    info->facets = FACETS | LMB_FACET_BIT(LMB_FACET_SIM_CONTROL);
    info->simulated = 1;
}

static int open_sim(const char *id, lmb_device_ops *ops, lmb_device_info *info)
{
    p2dev *d = dev_new();
    if (!d)
        return LMB_ERR_NO_MEM;
    d->sim = 1;
    const int rc = sim_init(d);
    if (rc != PPK2_OK) {
        dev_close(d);
        return map(rc);
    }
    fill_ops(d, ops);
    sim_info(info, id);
    return LMB_OK;
}

/* ---- family: hardware ---- */
#ifdef LMB_PPK2_HAVE_SERIAL

/* Id of the unit a port belongs to: its USB serial number, else the port path. */
static int port_id(const lmb_serial_port *p, char *id, size_t cap)
{
    const int n = snprintf(id, cap, "ppk2:%s", p->serial_number[0] ? p->serial_number : p->path);
    return n > 0 && (size_t)n < cap;
}

static lmb_serial_port *find_ports(size_t *n)
{
    *n = 0;
    size_t total = 0;
    if (ppk2_dev_find(NULL, 0, &total) != PPK2_OK || total == 0)
        return NULL;
    lmb_serial_port *ports = calloc(total, sizeof *ports);
    if (!ports)
        return NULL;
    size_t got = 0;
    if (ppk2_dev_find(ports, total, &got) != PPK2_OK) {
        free(ports);
        return NULL;
    }
    *n = got < total ? got : total;
    return ports;
}

static void hw_info(lmb_device_info *info, const lmb_serial_port *p, const char *id)
{
    memset(info, 0, sizeof *info);
    set_str(info->id, sizeof info->id, id);
    set_str(info->family, sizeof info->family, FAMILY);
    if (p->serial_number[0])
        snprintf(info->name, sizeof info->name, "PPK2 (%.40s)", p->serial_number);
    else
        set_str(info->name, sizeof info->name, "PPK2");
    set_str(info->serial, sizeof info->serial, p->serial_number);
    set_str(info->transport, sizeof info->transport, "usb-serial");
    info->facets = FACETS;
}

static int enumerate_hw(lmb_device_info *out, size_t cap, size_t *count)
{
    size_t n = 0;
    lmb_serial_port *ports = find_ports(&n);
    for (size_t i = 0; i < n; i++) {
        char id[64];
        if (!port_id(&ports[i], id, sizeof id))
            continue;
        int seen = 0; /* the unit's second port */
        for (size_t j = 0; j < i && !seen; j++) {
            char other[64];
            seen = port_id(&ports[j], other, sizeof other) && strcmp(id, other) == 0;
        }
        if (seen)
            continue;
        if (*count < cap)
            hw_info(&out[*count], &ports[i], id);
        (*count)++;
    }
    free(ports);
    return LMB_OK;
}

static int by_interface(const void *a, const void *b)
{
    int x = ((const lmb_serial_port *)a)->usb_interface, y = ((const lmb_serial_port *)b)->usb_interface;
    if (x < 0)
        x = 1 << 20; /* unknown last */
    if (y < 0)
        y = 1 << 20;
    return (x > y) - (x < y);
}

static int open_hw(const char *id, lmb_device_ops *ops, lmb_device_info *info)
{
    size_t n = 0;
    lmb_serial_port *ports = find_ports(&n);
    lmb_serial_port *mine = calloc(n ? n : 1, sizeof *mine);
    size_t m = 0;
    for (size_t i = 0; mine && i < n; i++) {
        char pid[64];
        if (port_id(&ports[i], pid, sizeof pid) && strcmp(pid, id) == 0)
            mine[m++] = ports[i];
    }
    free(ports);
    if (!mine || m == 0) {
        free(mine);
        return LMB_ERR_NOT_FOUND;
    }
    qsort(mine, m, sizeof *mine, by_interface);
    p2dev *d = dev_new();
    if (!d) {
        free(mine);
        return LMB_ERR_NO_MEM;
    }
    /* Which port is the measurement port is unconfirmed: try them in interface order and keep the
     * first that answers with metadata. A real error (busy, permission) beats a timeout. */
    int rc = PPK2_ERR_TIMEOUT;
    size_t used = 0;
    for (size_t i = 0; i < m; i++) {
        const int r = ppk2_dev_open(mine[i].path, 0, &d->hw);
        if (r == PPK2_OK) {
            rc = PPK2_OK;
            used = i;
            break;
        }
        if (rc == PPK2_ERR_TIMEOUT || r != PPK2_ERR_TIMEOUT)
            rc = r;
    }
    if (rc != PPK2_OK) {
        dev_close(d);
        free(mine);
        return map(rc);
    }
    fill_ops(d, ops);
    hw_info(info, &mine[used], id);
    free(mine);
    return LMB_OK;
}
#endif /* LMB_PPK2_HAVE_SERIAL */

/* ---- family ---- */

static int fam_enumerate(void *user, uint32_t flags, lmb_device_info *out, size_t cap, size_t *count)
{
    (void)user;
    *count = 0;
    if (!(flags & LMB_ENUM_NO_SIMULATED)) {
        /* "sim:ppk2", then "sim:ppk2:2", ... when LMB_PPK2_SIMULATORS asks for more (demos, tests of
         * an application with several devices). */
        long sims = 1;
        const char *env = getenv("LMB_PPK2_SIMULATORS");
        if (env && *env) {
            char *end = NULL;
            const long v = strtol(env, &end, 10);
            if (end != env && v >= 1 && v <= 8)
                sims = v;
        }
        for (long i = 1; i <= sims; i++) {
            char id[sizeof SIM_ID + 8];
            if (i == 1)
                snprintf(id, sizeof id, "%s", SIM_ID);
            else
                snprintf(id, sizeof id, "%s:%ld", SIM_ID, i);
            if (*count < cap)
                sim_info(&out[*count], id);
            (*count)++;
        }
    }
    #ifdef LMB_PPK2_HAVE_SERIAL
    if (!(flags & LMB_ENUM_ONLY_SIMULATED))
        return enumerate_hw(out, cap, count);
    #endif
    return LMB_OK;
}

static int fam_open(void *user, const char *id, lmb_device_ops *ops, lmb_device_info *info)
{
    (void)user;
    if (is_sim_id(id))
        return open_sim(id, ops, info);
    #ifdef LMB_PPK2_HAVE_SERIAL
    if (strncmp(id, "ppk2:", 5) == 0 && id[5])
        return open_hw(id, ops, info);
    #endif
    return LMB_ERR_NOT_FOUND;
}

int lmb_ppk2_register(lmb_context *ctx)
{
    lmb_family f;
    memset(&f, 0, sizeof f);
    f.abi_version = LMB_ABI_VERSION;
    f.size = sizeof f;
    f.name = FAMILY;
    f.enumerate = fam_enumerate;
    f.open = fam_open;
    return lmb_context_register_family(ctx, &f);
}

/* ---- simulator controls ---- */

static p2dev *sim_of(lmb_device *dev)
{
    p2dev *d = lmb_device_driver_data(dev, FAMILY);
    return (d && d->sim) ? d : NULL;
}

int lmb_ppk2_sim_set_load(lmb_device *dev, double amps)
{
    p2dev *d = sim_of(dev);
    return d ? sc_set_load(d, amps) : LMB_ERR_INVALID_ARG;
}

int lmb_ppk2_sim_drop(lmb_device *dev, uint64_t n)
{
    p2dev *d = sim_of(dev);
    return d ? sc_drop(d, n) : LMB_ERR_INVALID_ARG;
}

int lmb_ppk2_sim_disconnect(lmb_device *dev)
{
    p2dev *d = sim_of(dev);
    return d ? sc_disconnect(d) : LMB_ERR_INVALID_ARG;
}
