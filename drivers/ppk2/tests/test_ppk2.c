/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* PPK2 protocol core tests. Golden vectors come from the interoperability report. */

#include "libmybench/ppk2.h"
#include "text.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        g_checks++;                                                                    \
        if (!(cond)) {                                                                 \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                          \
    do {                                                                               \
        double a_ = (a), b_ = (b);                                                     \
        g_checks++;                                                                    \
        if (!(fabs(a_ - b_) <= (tol))) {                                               \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: %s = %.12g, expected %.12g (tol %g)\n", __FILE__,  \
                    __LINE__, #a, a_, b_, (double)(tol));                              \
        }                                                                              \
    } while (0)

static int frames_equal(const ppk2_tx *tx, const char *const *expected, size_t n)
{
    if (tx->count != n)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char hex[16] = "";
        for (unsigned b = 0; b < tx->frames[i].len; b++)
            snprintf(hex + strlen(hex), sizeof hex - strlen(hex), b ? " %02X" : "%02X", tx->frames[i].bytes[b]);
        if (strcmp(hex, expected[i]) != 0) {
            fprintf(stderr, "frame %zu: %s, expected %s\n", i, hex, expected[i]);
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------------------------ */

static void test_word_golden(void)
{
    /* Report: bytes 23 41 54 A5 give ADC 291, range 1, aux 0, sequence 21, logic A5. */
    ppk2_word w = ppk2_word_decode(0xA5544123u);
    CHECK(w.adc == 291);
    CHECK(w.range == 1);
    CHECK(w.aux == 0);
    CHECK(w.seq == 21);
    CHECK(w.logic == 0xA5);
    CHECK(ppk2_word_encode(w) == 0xA5544123u);

    ppk2_word all = {0x3FFF, 7, 1, 63, 0xFF};
    CHECK(ppk2_word_encode(all) == 0xFFFFFFFFu);
}

static void test_stream_splits(void)
{
    /* Three words with consecutive sequence numbers, fed split at every possible pair of points. */
    uint8_t bytes[12];
    for (unsigned k = 0; k < 3; k++) {
        ppk2_word w = {(uint16_t)(291 + k), 1, 0, (uint8_t)(21 + k), (uint8_t)(0xA5 + k)};
        uint32_t v = ppk2_word_encode(w);
        for (unsigned b = 0; b < 4; b++)
            bytes[4 * k + b] = (uint8_t)(v >> (8 * b));
    }
    CHECK(bytes[0] == 0x23 && bytes[1] == 0x41 && bytes[2] == 0x54 && bytes[3] == 0xA5);

    int all_ok = 1;
    for (size_t a = 0; a <= sizeof bytes; a++) {
        for (size_t b = a; b <= sizeof bytes; b++) {
            ppk2_stream *s = ppk2_stream_new();
            ppk2_sample out[3];
            size_t n = 0, used;
            n += ppk2_stream_decode(s, bytes, a, &used, out + n, 3 - n);
            all_ok &= used == a;
            n += ppk2_stream_decode(s, bytes + a, b - a, &used, out + n, 3 - n);
            all_ok &= used == b - a;
            n += ppk2_stream_decode(s, bytes + b, sizeof bytes - b, &used, out + n, 3 - n);
            all_ok &= used == sizeof bytes - b;
            all_ok &= n == 3;
            for (unsigned k = 0; k < n; k++)
                all_ok &= out[k].adc == 291 + k && out[k].logic == 0xA5 + k && !(out[k].flags & PPK2_SAMPLE_GAP);
            ppk2_stream_stats st;
            ppk2_stream_get_stats(s, &st);
            all_ok &= st.words == 3 && st.gaps == 0;
            ppk2_stream_free(s);
        }
    }
    CHECK(all_ok);
}

static void test_stream_output_full(void)
{
    uint8_t bytes[4 * 5];
    for (unsigned k = 0; k < 5; k++) {
        ppk2_word w = {(uint16_t)k, 0, 0, (uint8_t)k, 0};
        uint32_t v = ppk2_word_encode(w);
        for (unsigned b = 0; b < 4; b++)
            bytes[4 * k + b] = (uint8_t)(v >> (8 * b));
    }
    ppk2_stream *s = ppk2_stream_new();
    ppk2_sample out[2];
    size_t used, total = 0, got = 0;
    for (int round = 0; round < 4 && total < sizeof bytes; round++) {
        size_t n = ppk2_stream_decode(s, bytes + total, sizeof bytes - total, &used, out, 2);
        for (size_t k = 0; k < n; k++)
            CHECK(out[k].adc == got + k);
        got += n;
        total += used;
    }
    CHECK(got == 5);
    CHECK(total == sizeof bytes);
    ppk2_stream_free(s);
}

static void feed_words(ppk2_stream *s, const uint8_t *seqs, size_t n, ppk2_sample *out)
{
    for (size_t k = 0; k < n; k++) {
        ppk2_word w = {100, 0, 0, seqs[k], 0};
        uint32_t v = ppk2_word_encode(w);
        uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        size_t used;
        CHECK(ppk2_stream_decode(s, b, 4, &used, &out[k], 1) == 1);
    }
}

static void test_stream_sequence(void)
{
    ppk2_stream *s = ppk2_stream_new();
    ppk2_sample out[6];
    const uint8_t seqs[] = {62, 63, 0, 1, 5, 6}; /* wrap 63 -> 0 is fine, 1 -> 5 loses 3 */
    feed_words(s, seqs, 6, out);
    CHECK(!(out[2].flags & PPK2_SAMPLE_GAP));
    CHECK(out[4].flags & PPK2_SAMPLE_GAP);
    CHECK(out[4].missing == 3);
    CHECK(!(out[5].flags & PPK2_SAMPLE_GAP));
    ppk2_stream_stats st;
    ppk2_stream_get_stats(s, &st);
    CHECK(st.gaps == 1 && st.missing == 3 && st.words == 6);

    /* After a reset the first word starts a new sequence: no gap. */
    ppk2_stream_reset(s);
    const uint8_t again[] = {40};
    feed_words(s, again, 1, out);
    CHECK(!(out[0].flags & PPK2_SAMPLE_GAP));
    ppk2_stream_free(s);

    /* A loss of exactly 64 is invisible to the sequence check; the rate check sees it. */
    CHECK(ppk2_possible_undetected_loss(100000 - 512, 0, 1.0) == 512);
    CHECK(ppk2_possible_undetected_loss(100000, 0, 1.0) == 0);
    CHECK(ppk2_possible_undetected_loss(99000, 1000, 1.0) == 0);
    CHECK(ppk2_possible_undetected_loss(10, 0, -1.0) == 0);
}

static void test_stream_bad_range(void)
{
    ppk2_stream *s = ppk2_stream_new();
    ppk2_cal cal;
    ppk2_cal_default(&cal);
    ppk2_stream_set_calibration(s, &cal, 3.3);
    ppk2_word w = {500, 6, 1, 9, 0x3C};
    uint32_t v = ppk2_word_encode(w);
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    ppk2_sample out;
    size_t used;
    CHECK(ppk2_stream_decode(s, b, 4, &used, &out, 1) == 1);
    CHECK(isnan(out.current_a));
    CHECK(out.flags & PPK2_SAMPLE_BAD_RANGE);
    CHECK(out.flags & PPK2_SAMPLE_AUX);
    CHECK(out.logic == 0x3C);
    ppk2_stream_stats st;
    ppk2_stream_get_stats(s, &st);
    CHECK(st.bad_range == 1);
    ppk2_stream_free(s);

    /* No calibration: current is NAN, everything else decodes. */
    s = ppk2_stream_new();
    w.range = 2;
    v = ppk2_word_encode(w);
    uint8_t c[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    CHECK(ppk2_stream_decode(s, c, 4, &used, &out, 1) == 1);
    CHECK(isnan(out.current_a) && out.range == 2 && !(out.flags & PPK2_SAMPLE_BAD_RANGE));
    ppk2_stream_free(s);
}

/* ------------------------------------------------------------------------------------------ */

static const char k_meta[] =
    "Calibrated: true\n"
    "HW: 9173\n"
    "mode: 2\n"
    "VDD: 3300\n"
    "IA: 56\n"
    "R0: 1003.5\nR1: 101.2\nR2: 10.07\nR3: 0.9575\nR4: 0.0443\n"
    "GS0: 0\nGS1: 0.00012\nGS2: 0.0000318\nGS3: 0\nGS4: 0\n"
    "GI0: 1.0021\nGI1: 0.998\nGI2: 1.001\nGI3: 1.0003\nGI4: 0.9989\n"
    "O0: 112.6\nO1: 75.2\nO2: 64.1\nO3: 60.9\nO4: 58.8\n"
    "S0: 0.0000000231\nS1: 1.5e-7\nS2: 0\nS3: 0\nS4: 0\n"
    "I0: -0.0000000012\nI1: 0\nI2: 0\nI3: 0\nI4: 0\n"
    "UG0: 1\nUG1: 1\nUG2: 1\nUG3: 1\nUG4: 1\n"
    "SomethingNew: 7\n"
    "END\n";

static void check_meta(const ppk2_metadata *m)
{
    CHECK(m->calibrated == 1);
    CHECK(strcmp(m->hw, "9173") == 0);
    CHECK(strcmp(m->mode, "2") == 0);
    CHECK_NEAR(m->vdd_mv, 3300, 0);
    CHECK_NEAR(m->ia, 56, 0);
    CHECK_NEAR(m->coef[PPK2_COEF_R][3], 0.9575, 1e-15);
    CHECK_NEAR(m->coef[PPK2_COEF_GS][2], 0.0000318, 1e-18);
    CHECK_NEAR(m->coef[PPK2_COEF_S][1], 1.5e-7, 1e-20);
    CHECK_NEAR(m->coef[PPK2_COEF_I][0], -0.0000000012, 1e-22);
    CHECK(m->present == (UINT64_C(1) << 35) - 1);
    CHECK(m->unknown_keys == 1);
    CHECK(m->bad_lines == 0);
}

static void test_meta_parse_and_splits(void)
{
    const size_t n = sizeof k_meta - 1;
    const uint8_t tail[] = {0x23, 0x41, 0x54, 0xA5};
    uint8_t buf[sizeof k_meta + sizeof tail];
    memcpy(buf, k_meta, n);
    memcpy(buf + n, tail, sizeof tail);

    /* In one read: consumption stops right after END, the sample bytes stay for the decoder. */
    ppk2_meta_parser *p = ppk2_meta_parser_new();
    CHECK(ppk2_meta_parser_feed(p, buf, n + sizeof tail) == n);
    CHECK(ppk2_meta_parser_done(p));
    check_meta(ppk2_meta_parser_result(p));
    CHECK(ppk2_meta_parser_feed(p, buf, 4) == 0); /* done: takes nothing more */

    /* Split at every byte boundary. */
    int all_ok = 1;
    for (size_t cut = 0; cut <= n; cut++) {
        ppk2_meta_parser_reset(p);
        size_t a = ppk2_meta_parser_feed(p, buf, cut);
        size_t b = ppk2_meta_parser_feed(p, buf + cut, n + sizeof tail - cut);
        all_ok &= a == cut && a + b == n && ppk2_meta_parser_done(p);
        const ppk2_metadata *m = ppk2_meta_parser_result(p);
        all_ok &= m->present == (UINT64_C(1) << 35) - 1 && m->coef[PPK2_COEF_O][4] == 58.8 && m->bad_lines == 0;
    }
    CHECK(all_ok);

    /* One byte per read. */
    ppk2_meta_parser_reset(p);
    size_t used = 0;
    for (size_t i = 0; i < n; i++)
        used += ppk2_meta_parser_feed(p, buf + i, 1);
    CHECK(used == n && ppk2_meta_parser_done(p));
    check_meta(ppk2_meta_parser_result(p));
    ppk2_meta_parser_free(p);
}

static void test_meta_robustness(void)
{
    ppk2_meta_parser *p = ppk2_meta_parser_new();
    char text[600];
    memset(text, 'x', 400);
    strcpy(text + 400, "\r\nR2: abc\nno colon here\n  R1 :  99.5  \r\nGS9: 1\nCalibrated: maybe\nEND\r\n");
    size_t len = strlen(text);
    CHECK(ppk2_meta_parser_feed(p, (const uint8_t *)text, len) == len);
    CHECK(ppk2_meta_parser_done(p));
    const ppk2_metadata *m = ppk2_meta_parser_result(p);
    CHECK(m->bad_lines == 4); /* over-long line, R2: abc, no colon, Calibrated: maybe */
    CHECK(m->unknown_keys == 1); /* GS9 */
    CHECK(m->present == PPK2_COEF_BIT(PPK2_COEF_R, 1));
    CHECK_NEAR(m->coef[PPK2_COEF_R][1], 99.5, 0);
    CHECK(m->calibrated == -1);
    CHECK(isnan(m->vdd_mv));

    /* A NUL byte makes the line garbage. */
    ppk2_meta_parser_reset(p);
    const uint8_t nul[] = {'R', '0', ':', ' ', '1', 0, '\n', 'E', 'N', 'D', '\n'};
    CHECK(ppk2_meta_parser_feed(p, nul, sizeof nul) == sizeof nul);
    CHECK(ppk2_meta_parser_result(p)->present == 0 && ppk2_meta_parser_result(p)->bad_lines == 1);
    ppk2_meta_parser_free(p);
}

static void test_locale_independence(void)
{
    /* A French LC_NUMERIC makes strtod() expect "1,5"; our parser must not care. */
    const char *loc = setlocale(LC_NUMERIC, "fr_FR.UTF-8");
    if (!loc)
        loc = setlocale(LC_NUMERIC, "fr_FR.utf8");
    double v = 0;
    CHECK(lmb_parse_double("1031.64", &v) == 0);
    CHECK_NEAR(v, 1031.64, 1e-12);
    char buf[32];
    CHECK(lmb_format_double(buf, sizeof buf, 0.043) > 0);
    CHECK(strcmp(buf, "0.043") == 0);
    if (!loc)
        fprintf(stderr, "note: no French locale installed, locale test ran in C locale\n");
    setlocale(LC_NUMERIC, "C");

    CHECK(lmb_parse_double("  -12.5e-3 ", &v) == 0 && v == -0.0125);
    CHECK(lmb_parse_double("1e400", &v) != 0);
    CHECK(lmb_parse_double("", &v) != 0);
    CHECK(lmb_parse_double(".", &v) != 0);
    CHECK(lmb_parse_double("1.2.3", &v) != 0);
    CHECK(lmb_parse_double("5e", &v) != 0);
    CHECK(lmb_parse_double("0.000000000000000000000000001", &v) == 0 && fabs(v - 1e-27) < 1e-40);
    CHECK(lmb_parse_double("123456789012345678901234", &v) == 0 && fabs(v / 1.23456789012345678901234e23 - 1) < 1e-15);
}

/* ------------------------------------------------------------------------------------------ */

static void test_calibration(void)
{
    ppk2_cal cal;
    ppk2_cal_default(&cal);
    CHECK(ppk2_cal_fallback_count(&cal) == 35);
    /* Fallback set, range 1, ADC 291: x = (4*291)*1.8/(163840*101.65), I = UG*(GS*x^2 + GI*x)
       with GS = GI = UG = 1 (the report's fallbacks). */
    const double x0 = 1164.0 * 1.8 / (163840.0 * 101.65);
    CHECK_NEAR(ppk2_cal_current(&cal, 291, 1, 3.3), x0 * x0 + x0, 1e-18);
    CHECK(isnan(ppk2_cal_current(&cal, 291, 5, 3.3)));
    CHECK(isnan(ppk2_cal_current(&cal, 291, 1, NAN)));

    ppk2_meta_parser *p = ppk2_meta_parser_new();
    ppk2_meta_parser_feed(p, (const uint8_t *)k_meta, sizeof k_meta - 1);
    ppk2_cal_from_metadata(&cal, ppk2_meta_parser_result(p));
    /* Zero GS0, GS3, GS4 keep their fallback of 1 and are reported; zero S/I/O are real zeros. */
    CHECK(cal.missing == 0);
    CHECK(cal.zero == (PPK2_COEF_BIT(PPK2_COEF_GS, 0) | PPK2_COEF_BIT(PPK2_COEF_GS, 3) | PPK2_COEF_BIT(PPK2_COEF_GS, 4)));
    CHECK(ppk2_cal_fallback_count(&cal) == 3);
    CHECK_NEAR(cal.coef[PPK2_COEF_GS][0], 1.0, 0);

    /* Full formula by hand, range 1. */
    const double a = 5000, v = 3.3;
    const double x = (4 * a - 75.2) * 1.8 / (163840 * 101.2);
    const double expect = 1.0 * (0.00012 * x * x + 0.998 * x + 1.5e-7 * v + 0);
    CHECK_NEAR(ppk2_cal_current(&cal, 5000, 1, v), expect, 1e-18);

    /* Missing coefficients. */
    ppk2_metadata m;
    memset(&m, 0, sizeof m);
    m.present = PPK2_COEF_BIT(PPK2_COEF_R, 0);
    m.coef[PPK2_COEF_R][0] = 1000.0;
    ppk2_cal_from_metadata(&cal, &m);
    CHECK(ppk2_cal_fallback_count(&cal) == 34);
    CHECK_NEAR(cal.coef[PPK2_COEF_R][0], 1000.0, 0);
    CHECK_NEAR(cal.coef[PPK2_COEF_R][4], 0.043, 0);
    ppk2_meta_parser_free(p);
}

/* ------------------------------------------------------------------------------------------ */

static void test_ctl_report_sequence(void)
{
    /* Report example: 3.3 V from the internal source, then stop and disconnect. */
    ppk2_ctl *c = ppk2_ctl_new(0);
    ppk2_tx tx;
    unsigned applied;

    CHECK(ppk2_ctl_request_metadata(c, &tx) == PPK2_ERR_STATE); /* stream state unknown */
    CHECK(ppk2_ctl_stop(c, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"07"}, 1));
    CHECK(ppk2_ctl_request_metadata(c, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"19"}, 1));

    CHECK(ppk2_ctl_set_power(c, 1, &tx) == PPK2_ERR_STATE); /* no supply yet */
    CHECK(tx.count == 0);
    CHECK(ppk2_ctl_select_supply(c, PPK2_SUPPLY_SOURCE, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"0C 00", "11 02"}, 2));
    CHECK(ppk2_ctl_set_power(c, 1, &tx) == PPK2_ERR_STATE); /* source without a voltage */
    CHECK(ppk2_ctl_set_voltage(c, 3300, &applied, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"0D 0C E4"}, 1));
    CHECK(applied == 3300);
    CHECK_NEAR(ppk2_ctl_source_volts(c), 3.3, 1e-12);
    CHECK(ppk2_ctl_set_power(c, 1, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"0C 01"}, 1));
    CHECK(ppk2_ctl_start(c, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"06"}, 1));
    CHECK(ppk2_ctl_start(c, &tx) == PPK2_ERR_STATE);
    CHECK(ppk2_ctl_request_metadata(c, &tx) == PPK2_ERR_STATE);
    CHECK(ppk2_ctl_stop(c, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"07"}, 1));
    CHECK(ppk2_ctl_power_on(c) == 1); /* stop leaves the gate alone */
    CHECK(ppk2_ctl_shutdown(c, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"0C 00"}, 1));
    CHECK(ppk2_ctl_power_on(c) == 0);
    ppk2_ctl_free(c);
}

static void test_ctl_safety(void)
{
    ppk2_ctl *c = ppk2_ctl_new(3600);
    ppk2_tx tx;
    unsigned applied;

    /* Voltage clamps: below 800 mV and above the cap. */
    CHECK(ppk2_ctl_set_voltage(c, 100, &applied, &tx) == PPK2_OK && applied == 800);
    CHECK(frames_equal(&tx, (const char *const[]){"0D 03 20"}, 1));
    CHECK(ppk2_ctl_set_voltage(c, 5000, &applied, &tx) == PPK2_OK && applied == 3600);

    /* Ammeter mode can power on without a voltage set... */
    ppk2_ctl *d = ppk2_ctl_new(0);
    CHECK(ppk2_ctl_select_supply(d, PPK2_SUPPLY_AMMETER, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"0C 00", "11 01"}, 2));
    CHECK(ppk2_ctl_set_power(d, 1, &tx) == PPK2_OK);
    CHECK(isnan(ppk2_ctl_source_volts(d)));
    /* ...and changing supply with the DUT connected turns the gate off first. */
    CHECK(ppk2_ctl_select_supply(d, PPK2_SUPPLY_SOURCE, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"0C 00", "11 02"}, 2));
    CHECK(ppk2_ctl_power_on(d) == 0);
    /* Same supply again: nothing to send. */
    CHECK(ppk2_ctl_select_supply(d, PPK2_SUPPLY_SOURCE, &tx) == PPK2_OK && tx.count == 0);
    CHECK(ppk2_ctl_select_supply(d, PPK2_SUPPLY_NONE, &tx) == PPK2_ERR_INVALID_ARG);

    /* After a failed write everything is unknown: shutdown sends stop and gate off, and power on
       needs a fresh supply selection. */
    ppk2_ctl_invalidate(d);
    CHECK(ppk2_ctl_set_power(d, 1, &tx) == PPK2_ERR_STATE);
    CHECK(ppk2_ctl_shutdown(d, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"07", "0C 00"}, 2));
    ppk2_ctl_invalidate(d);
    CHECK(ppk2_ctl_select_supply(d, PPK2_SUPPLY_SOURCE, &tx) == PPK2_OK);
    CHECK(frames_equal(&tx, (const char *const[]){"0C 00", "11 02"}, 2));
    ppk2_ctl_free(d);
    ppk2_ctl_free(c);
}

/* ------------------------------------------------------------------------------------------ */

static void test_records(void)
{
    /* Report: 1.0 uA with only D0 high encodes as 00 00 80 3F 55 56. */
    uint8_t rec[PPK2_RECORD_SIZE];
    uint16_t states = ppk2_logic_states_from_mask(0x01);
    ppk2_record_encode(1.0f, states, rec);
    const uint8_t expect[PPK2_RECORD_SIZE] = {0x00, 0x00, 0x80, 0x3F, 0x55, 0x56};
    CHECK(memcmp(rec, expect, sizeof rec) == 0);

    float ua;
    uint16_t st;
    ppk2_record_decode(expect, &ua, &st);
    CHECK(ua == 1.0f);
    CHECK(st == 0x5556);
    CHECK(ppk2_logic_state(st, 0) == PPK2_LOGIC_HIGH);
    CHECK(ppk2_logic_state(st, 7) == PPK2_LOGIC_LOW);
    CHECK(ppk2_logic_state(st, 8) == PPK2_LOGIC_MISSING);
    CHECK(ppk2_logic_state(0xC000, 7) == PPK2_LOGIC_BOTH);
}

/* ------------------------------------------------------------------------------------------ */

typedef struct {
    double amps;
} load;

static double load_current(void *user, uint64_t i)
{
    (void)i;
    return ((load *)user)->amps;
}

static uint8_t load_logic(void *user, uint64_t i)
{
    (void)user;
    return (uint8_t)(i * 7u);
}

static void write_tx(ppk2_sim *sim, const ppk2_tx *tx)
{
    for (size_t i = 0; i < tx->count; i++)
        CHECK(ppk2_sim_write(sim, tx->frames[i].bytes, tx->frames[i].len) == PPK2_OK);
}

static void test_sim_end_to_end(void)
{
    /* A unit with its own coefficients; host and simulator talk only through bytes. */
    ppk2_meta_parser *p = ppk2_meta_parser_new();
    ppk2_meta_parser_feed(p, (const uint8_t *)k_meta, sizeof k_meta - 1);
    ppk2_cal unit;
    ppk2_cal_from_metadata(&unit, ppk2_meta_parser_result(p));

    ppk2_sim *sim = ppk2_sim_new(&unit);
    load ld = {0};
    ppk2_sim_set_load(sim, load_current, load_logic, &ld);
    ppk2_ctl *ctl = ppk2_ctl_new(0);
    ppk2_tx tx;

    ppk2_ctl_stop(ctl, &tx);
    write_tx(sim, &tx);
    ppk2_ctl_request_metadata(ctl, &tx);
    write_tx(sim, &tx);
    ppk2_ctl_select_supply(ctl, PPK2_SUPPLY_SOURCE, &tx);
    write_tx(sim, &tx);
    ppk2_ctl_set_voltage(ctl, 3000, NULL, &tx);
    write_tx(sim, &tx);
    ppk2_ctl_set_power(ctl, 1, &tx);
    write_tx(sim, &tx);
    ppk2_ctl_start(ctl, &tx);
    write_tx(sim, &tx);

    /* Read metadata then samples in odd-sized chunks, as a serial port would deliver them. */
    ppk2_meta_parser_reset(p);
    ppk2_stream *s = ppk2_stream_new();
    ppk2_cal host_cal;
    int have_cal = 0;
    const double levels[] = {1e-6, 3e-6, 4.5e-5, 6e-4, 7.5e-3, 0.09};
    double worst_rel = 0;

    for (size_t li = 0; li < sizeof levels / sizeof levels[0]; li++) {
        ld.amps = levels[li];
        unsigned range;
        const double expect = ppk2_sim_quantize(sim, ld.amps, &range);
        uint64_t due = 997;
        size_t seen = 0;
        while (due > 0 || seen == 0) {
            uint8_t buf[333];
            size_t n = ppk2_sim_read(sim, buf, sizeof buf, due);
            if (n == 0)
                break;
            size_t off = 0;
            if (!ppk2_meta_parser_done(p))
                off = ppk2_meta_parser_feed(p, buf, n);
            if (ppk2_meta_parser_done(p) && !have_cal) {
                ppk2_cal_from_metadata(&host_cal, ppk2_meta_parser_result(p));
                ppk2_stream_set_calibration(s, &host_cal, ppk2_ctl_source_volts(ctl));
                have_cal = 1;
            }
            while (off < n) {
                ppk2_sample out[64];
                size_t used;
                size_t got = ppk2_stream_decode(s, buf + off, n - off, &used, out, 64);
                off += used;
                for (size_t k = 0; k < got; k++) {
                    CHECK(out[k].range == range);
                    CHECK_NEAR(out[k].current_a, expect, fabs(expect) * 1e-9);
                    double rel = fabs(out[k].current_a - ld.amps) / ld.amps;
                    if (rel > worst_rel)
                        worst_rel = rel;
                }
                seen += got;
                due -= got;
            }
        }
        CHECK(seen == 997);
    }
    CHECK(have_cal);
    /* Range switching keeps the quantization error within a few percent from 1 uA to 90 mA. */
    CHECK(worst_rel < 0.03);

    /* Inject a loss of 70 samples: detected as 6 (70 mod 64). A loss of 128 is not detected. */
    ppk2_sim_drop(sim, 70);
    uint8_t buf[64];
    size_t n = ppk2_sim_read(sim, buf, sizeof buf, 2);
    ppk2_sample out[2];
    size_t used;
    CHECK(ppk2_stream_decode(s, buf, n, &used, out, 2) == 2);
    CHECK((out[0].flags & PPK2_SAMPLE_GAP) && out[0].missing == 6);
    ppk2_sim_drop(sim, 128);
    n = ppk2_sim_read(sim, buf, sizeof buf, 1);
    CHECK(ppk2_stream_decode(s, buf, n, &used, out, 1) == 1);
    CHECK(!(out[0].flags & PPK2_SAMPLE_GAP));

    /* Gate off: no current, and stop ends the stream. */
    ppk2_ctl_shutdown(ctl, &tx);
    write_tx(sim, &tx);
    CHECK(ppk2_sim_read(sim, buf, sizeof buf, 10) == 0);

    /* Unknown and malformed commands are refused. */
    const uint8_t bad1[] = {0x0D, 0x0C};
    const uint8_t bad2[] = {0x42};
    CHECK(ppk2_sim_write(sim, bad1, sizeof bad1) == PPK2_ERR_FORMAT);
    CHECK(ppk2_sim_write(sim, bad2, sizeof bad2) == PPK2_ERR_FORMAT);

    ppk2_stream_free(s);
    ppk2_ctl_free(ctl);
    ppk2_sim_free(sim);
    ppk2_meta_parser_free(p);
}

static void test_sim_missing_coefficients(void)
{
    /* A unit without metadata coefficients: the host falls back and says so. */
    ppk2_cal unit;
    ppk2_cal_default(&unit);
    ppk2_sim *sim = ppk2_sim_new(&unit);
    const uint8_t req = 0x19;
    CHECK(ppk2_sim_write(sim, &req, 1) == PPK2_OK);
    uint8_t buf[256];
    size_t n = ppk2_sim_read(sim, buf, sizeof buf, 0);
    ppk2_meta_parser *p = ppk2_meta_parser_new();
    CHECK(ppk2_meta_parser_feed(p, buf, n) == n && ppk2_meta_parser_done(p));
    CHECK(ppk2_meta_parser_result(p)->calibrated == 0);
    ppk2_cal host;
    ppk2_cal_from_metadata(&host, ppk2_meta_parser_result(p));
    CHECK(ppk2_cal_fallback_count(&host) == 35);
    ppk2_meta_parser_free(p);
    ppk2_sim_free(sim);
}

static void test_random_input(void)
{
    /* Garbage must never crash or overrun: a cheap stand-in for the fuzz targets. */
    uint32_t x = 12345;
    uint8_t buf[4096];
    ppk2_meta_parser *p = ppk2_meta_parser_new();
    ppk2_stream *s = ppk2_stream_new();
    ppk2_sample out[100];
    for (int round = 0; round < 200; round++) {
        size_t len = 1 + (x % sizeof buf);
        for (size_t i = 0; i < len; i++) {
            x = x * 1103515245u + 12345u;
            buf[i] = (uint8_t)(x >> 16);
            if ((x >> 8) % 7 == 0)
                buf[i] = '\n';
        }
        ppk2_meta_parser_reset(p);
        size_t used = ppk2_meta_parser_feed(p, buf, len);
        CHECK(used <= len);
        size_t off = 0;
        while (off < len) {
            size_t u;
            ppk2_stream_decode(s, buf + off, len - off, &u, out, 100);
            if (u == 0)
                break;
            off += u;
        }
    }
    ppk2_meta_parser_free(p);
    ppk2_stream_free(s);
}

int main(void)
{
    test_word_golden();
    test_stream_splits();
    test_stream_output_full();
    test_stream_sequence();
    test_stream_bad_range();
    test_meta_parse_and_splits();
    test_meta_robustness();
    test_locale_independence();
    test_calibration();
    test_ctl_report_sequence();
    test_ctl_safety();
    test_records();
    test_sim_end_to_end();
    test_sim_missing_coefficients();
    test_random_input();

    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
