/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* PPK2 device layer end to end (POSIX): ppk2_dev on the serial transport, talking to a
 * simulated PPK2 behind a pseudo-terminal, in real time. */

#if !defined(__APPLE__)
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "libmybench/ppk2_dev.h"

#include "sim_port.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define CHECK_RC(expr, want)                                                           \
    do {                                                                               \
        int rc_ = (expr);                                                              \
        g_checks++;                                                                    \
        if (rc_ != (want)) {                                                           \
            g_failures++;                                                              \
            fprintf(stderr, "%s:%d: %s = %d (%s), expected %d\n", __FILE__, __LINE__,  \
                    #expr, rc_, ppk2_dev_strerror(rc_), (want));                       \
        }                                                                              \
    } while (0)

/* Reads n samples (or until 3 s pass without any). Returns how many arrived. */
static size_t collect(ppk2_dev *d, ppk2_sample *out, size_t n)
{
    size_t total = 0;
    int idle = 0;
    while (total < n && idle < 30) {
        size_t got = 0;
        const int rc = ppk2_dev_read(d, out + total, n - total, 100, &got);
        if (rc != PPK2_OK) {
            fprintf(stderr, "read: %s\n", ppk2_dev_strerror(rc));
            break;
        }
        idle = got ? 0 : idle + 1;
        total += got;
    }
    return total;
}

static void test_measure(void)
{
    /* A unit with its own calibration: the device layer must read and use it. */
    ppk2_cal cal;
    ppk2_cal_default(&cal);
    cal.coef[PPK2_COEF_R][2] = 10.5;
    cal.coef[PPK2_COEF_O][2] = 12.0;
    cal.missing = 0; /* every coefficient is in its metadata reply */
    ppk2_sim *sim = ppk2_sim_new(&cal);
    /* Left streaming by a previous program: open must stop it and drop what it sent. */
    const uint8_t start = 0x06;
    ppk2_sim_write(sim, &start, 1);

    char path[256];
    sim_port *port = NULL;
    CHECK(sim_port_start(sim, path, sizeof path, &port) == 0);
    if (!port)
        return;
    usleep(100 * 1000); /* let some stale samples queue up */

    ppk2_dev *d = NULL;
    CHECK_RC(ppk2_dev_open(path, 4000, &d), PPK2_OK);
    if (!d) {
        sim_port_stop(port);
        ppk2_sim_free(sim);
        return;
    }
    CHECK(ppk2_dev_calibration(d)->coef[PPK2_COEF_R][2] == 10.5);
    CHECK(ppk2_dev_calibration(d)->coef[PPK2_COEF_O][2] == 12.0);
    CHECK(ppk2_cal_fallback_count(ppk2_dev_calibration(d)) == 0);
    CHECK(ppk2_ctl_sampling(ppk2_dev_ctl(d)) == 0);

    /* The power order is enforced: no gate before a supply and a voltage. */
    CHECK_RC(ppk2_dev_set_power(d, 1), PPK2_ERR_STATE);
    CHECK_RC(ppk2_dev_select_supply(d, PPK2_SUPPLY_SOURCE), PPK2_OK);
    unsigned applied = 0;
    CHECK_RC(ppk2_dev_set_voltage(d, 4500, &applied), PPK2_OK);
    CHECK(applied == 4000); /* the cap */
    CHECK_RC(ppk2_dev_set_voltage(d, 3300, &applied), PPK2_OK);
    CHECK_RC(ppk2_dev_set_power(d, 1), PPK2_OK);
    CHECK_RC(ppk2_dev_start(d), PPK2_OK);

    enum { N = 30000 }; /* 0.3 s */
    ppk2_sample *s = malloc(N * sizeof *s);
    const size_t got = collect(d, s, N);
    CHECK(got == N);
    double sum = 0.0;
    size_t finite = 0;
    for (size_t i = 0; i < got; i++)
        if (isfinite(s[i].current_a)) {
            sum += s[i].current_a;
            finite++;
        }
    CHECK(finite == got);
    /* The simulator's default load: 1 mA; ADC rounding keeps it within a fraction of a percent. */
    if (finite)
        CHECK(fabs(sum / (double)finite - 1e-3) < 1e-5);
    ppk2_stream_stats st;
    ppk2_dev_stats(d, &st);
    CHECK(st.gaps == 0);
    CHECK(st.words >= N);

    CHECK_RC(ppk2_dev_stop(d), PPK2_OK);
    CHECK(ppk2_ctl_sampling(ppk2_dev_ctl(d)) == 0);
    size_t after = 0;
    CHECK_RC(ppk2_dev_read(d, s, N, 150, &after), PPK2_OK);
    CHECK(after == 0); /* drained, nothing more comes */

    /* A second run restarts cleanly (sequence and carry reset). */
    CHECK_RC(ppk2_dev_start(d), PPK2_OK);
    CHECK(collect(d, s, 5000) == 5000);
    ppk2_dev_stats(d, &st);
    CHECK(st.gaps == 0);

    ppk2_dev_close(d);
    usleep(50 * 1000);
    int gate = -1, sampling = -1;
    ppk2_supply supply;
    unsigned mv = 0;
    ppk2_sim_state(sim, &gate, &sampling, &supply, &mv);
    CHECK(gate == 0);     /* close disconnects the DUT */
    CHECK(sampling == 0); /* and stops the stream */
    CHECK(supply == PPK2_SUPPLY_SOURCE && mv == 3300);
    unsigned long commands = 0, refused = 0;
    sim_port_counts(port, &commands, &refused);
    CHECK(commands > 0 && refused == 0);
    sim_port_stop(port);
    ppk2_sim_free(sim);
    free(s);
}

static void test_no_answer(void)
{
    /* A terminal that never answers (the console port, or not a PPK2): open times out. */
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    CHECK(master >= 0 && grantpt(master) == 0 && unlockpt(master) == 0);
    const char *name = ptsname(master);
    ppk2_dev *d = NULL;
    CHECK_RC(ppk2_dev_open(name, 0, &d), PPK2_ERR_TIMEOUT);
    CHECK(d == NULL);
    close(master);

    CHECK_RC(ppk2_dev_open("/dev/does-not-exist-ppk2", 0, &d), PPK2_ERR_NOT_FOUND);
    CHECK_RC(ppk2_dev_open("/dev/null", 0, &d), PPK2_ERR_INVALID_ARG); /* not a terminal */
}

static void test_busy(void)
{
    ppk2_sim *sim = ppk2_sim_new(NULL);
    char path[256];
    sim_port *port = NULL;
    CHECK(sim_port_start(sim, path, sizeof path, &port) == 0);
    ppk2_dev *a = NULL, *b = NULL;
    CHECK_RC(ppk2_dev_open(path, 0, &a), PPK2_OK);
    CHECK_RC(ppk2_dev_open(path, 0, &b), PPK2_ERR_BUSY); /* one program at a time */
    ppk2_dev_close(a);
    CHECK_RC(ppk2_dev_open(path, 0, &b), PPK2_OK);
    ppk2_dev_close(b);
    sim_port_stop(port);
    ppk2_sim_free(sim);
}

static void test_find(void)
{
    /* No PPK2 on a build machine: the call works and finds none (or real ones, which is fine). */
    size_t n = 99;
    lmb_serial_port ports[4];
    CHECK_RC(ppk2_dev_find(ports, 4, &n), PPK2_OK);
    CHECK(n < 99);
    for (size_t i = 0; i < n && i < 4; i++)
        CHECK(ports[i].usb_vid == PPK2_USB_VID && ports[i].usb_pid == PPK2_USB_PID);
    size_t all = 0;
    CHECK(lmb_serial_list(NULL, 0, &all) == LMB_SERIAL_OK);
}

int main(void)
{
    test_measure();
    test_no_answer();
    test_busy();
    test_find();
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
