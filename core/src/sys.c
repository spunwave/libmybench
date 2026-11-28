/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/* Portable mutex and monotonic clock for the core and the drivers (driver.h). */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "libmybench/driver.h"

#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>

struct lmb_mutex {
    CRITICAL_SECTION cs;
};

lmb_mutex *lmb_mutex_new(void)
{
    lmb_mutex *m = malloc(sizeof *m);
    if (m)
        InitializeCriticalSection(&m->cs);
    return m;
}

void lmb_mutex_free(lmb_mutex *m)
{
    if (m) {
        DeleteCriticalSection(&m->cs);
        free(m);
    }
}

void lmb_mutex_lock(lmb_mutex *m) { EnterCriticalSection(&m->cs); }
void lmb_mutex_unlock(lmb_mutex *m) { LeaveCriticalSection(&m->cs); }

double lmb_monotonic_s(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

void lmb_sleep_ms(unsigned ms) { Sleep(ms); }

#else
#include <pthread.h>
#include <time.h>

struct lmb_mutex {
    pthread_mutex_t mu;
};

lmb_mutex *lmb_mutex_new(void)
{
    lmb_mutex *m = malloc(sizeof *m);
    if (m && pthread_mutex_init(&m->mu, NULL) != 0) {
        free(m);
        return NULL;
    }
    return m;
}

void lmb_mutex_free(lmb_mutex *m)
{
    if (m) {
        pthread_mutex_destroy(&m->mu);
        free(m);
    }
}

void lmb_mutex_lock(lmb_mutex *m) { pthread_mutex_lock(&m->mu); }
void lmb_mutex_unlock(lmb_mutex *m) { pthread_mutex_unlock(&m->mu); }

double lmb_monotonic_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void lmb_sleep_ms(unsigned ms)
{
    struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
    while (nanosleep(&ts, &ts) != 0) {
    }
}
#endif
