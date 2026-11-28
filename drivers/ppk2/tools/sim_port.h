/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* A simulated PPK2 behind a pseudo-terminal (POSIX): programs open the printed path like the
 * real measurement port. A thread splits the bytes it receives into commands, feeds them to a
 * ppk2_sim and sends its output paced in real time (100 kS/s). Used by ppk2-tool sim-port and
 * the device tests; not part of the library. */
#ifndef LIBMYBENCH_PPK2_SIM_PORT_H
#define LIBMYBENCH_PPK2_SIM_PORT_H

#include "libmybench/ppk2.h"

#include <stddef.h>

typedef struct sim_port sim_port;

/* Starts serving sim (owned by the caller, used by the thread until sim_port_stop). Writes the
 * port's path to path. 0 on success, -1 with errno otherwise. */
int sim_port_start(ppk2_sim *sim, char *path, size_t cap, sim_port **out);
/* Stops the thread and closes the pseudo-terminal: a program that has it open sees a hang-up. */
void sim_port_stop(sim_port *p);
/* Commands received and refused by the simulator so far (refused: unknown or malformed). */
void sim_port_counts(const sim_port *p, unsigned long *commands, unsigned long *refused);

#endif
