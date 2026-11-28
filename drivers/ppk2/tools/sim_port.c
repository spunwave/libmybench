/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "sim_port.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

struct sim_port {
    ppk2_sim *sim;
    int master, slave; /* the slave stays open here: the terminal stays raw and never hangs up */
    pthread_t thread;
    volatile int stop;
    uint8_t cmd[8];
    size_t cmd_len;
    unsigned long commands, refused;
    double carry; /* fraction of a sample word due but not sent yet */
};

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Split received bytes into commands (one opcode + its fixed-length arguments). */
static void take_input(sim_port *p, const uint8_t *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p->cmd_len == 0 && ppk2_command_length(data[i]) == 0) {
            p->refused++; /* not an opcode: dropped, like noise on the line */
            continue;
        }
        p->cmd[p->cmd_len++] = data[i];
        if (p->cmd_len == ppk2_command_length(p->cmd[0])) {
            p->commands++;
            if (ppk2_sim_write(p->sim, p->cmd, p->cmd_len) != PPK2_OK)
                p->refused++;
            p->cmd_len = 0;
        }
    }
}

/* Write all of buf unless asked to stop. */
static void put(sim_port *p, const uint8_t *buf, size_t n)
{
    while (n && !p->stop) {
        const ssize_t w = write(p->master, buf, n);
        if (w > 0) {
            buf += w;
            n -= (size_t)w;
            continue;
        }
        if (w < 0 && errno != EAGAIN && errno != EINTR)
            return;
        struct pollfd pf = {p->master, POLLOUT, 0};
        poll(&pf, 1, 20);
    }
}

static void *serve(void *arg)
{
    sim_port *p = arg;
    uint8_t in[256], out[16384];
    double last = now_s();
    while (!p->stop) {
        struct pollfd pf = {p->master, POLLIN, 0};
        poll(&pf, 1, 2);
        if (pf.revents & POLLIN) {
            const ssize_t n = read(p->master, in, sizeof in);
            if (n > 0)
                take_input(p, in, (size_t)n);
        }
        const double t = now_s();
        double due = (t - last) * PPK2_SAMPLE_RATE + p->carry;
        last = t;
        if (due > 20000.0)
            due = 20000.0; /* after a stall, don't burst more than 0.2 s */
        const uint64_t words = (uint64_t)due;
        p->carry = due - (double)words;
        uint64_t left = words;
        for (;;) {
            const uint64_t before = ppk2_sim_index(p->sim);
            const size_t n = ppk2_sim_read(p->sim, out, sizeof out, left);
            if (n == 0)
                break;
            put(p, out, n);
            const uint64_t sent = ppk2_sim_index(p->sim) - before;
            left = sent < left ? left - sent : 0;
        }
    }
    return NULL;
}

int sim_port_start(ppk2_sim *sim, char *path, size_t cap, sim_port **out)
{
    if (!sim || !path || !out) {
        errno = EINVAL;
        return -1;
    }
    sim_port *p = calloc(1, sizeof *p);
    if (!p)
        return -1;
    p->sim = sim;
    p->slave = -1;
    p->master = posix_openpt(O_RDWR | O_NOCTTY);
    if (p->master < 0 || grantpt(p->master) != 0 || unlockpt(p->master) != 0)
        goto fail;
    const char *name = ptsname(p->master);
    if (!name || strlen(name) >= cap)
        goto fail;
    strcpy(path, name);
    p->slave = open(name, O_RDWR | O_NOCTTY);
    if (p->slave < 0)
        goto fail;
    struct termios t;
    if (tcgetattr(p->slave, &t) != 0)
        goto fail;
    cfmakeraw(&t);
    if (tcsetattr(p->slave, TCSANOW, &t) != 0)
        goto fail;
    fcntl(p->master, F_SETFL, fcntl(p->master, F_GETFL) | O_NONBLOCK);
    if (pthread_create(&p->thread, NULL, serve, p) != 0)
        goto fail;
    *out = p;
    return 0;
fail:;
    const int e = errno;
    if (p->slave >= 0)
        close(p->slave);
    if (p->master >= 0)
        close(p->master);
    free(p);
    errno = e;
    return -1;
}

void sim_port_stop(sim_port *p)
{
    if (!p)
        return;
    p->stop = 1;
    pthread_join(p->thread, NULL);
    close(p->slave);
    close(p->master);
    free(p);
}

void sim_port_counts(const sim_port *p, unsigned long *commands, unsigned long *refused)
{
    if (commands)
        *commands = p->commands;
    if (refused)
        *refused = p->refused;
}
