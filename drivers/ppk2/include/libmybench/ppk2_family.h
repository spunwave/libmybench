/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */
/*
 * The PPK2 as a device family of the libmybench device model (device.h): PowerSupply,
 * CurrentStream and LogicInput facets on top of ppk2_dev, plus a simulated PPK2 behind the same
 * registry. An application normally doesn't call this header: lmb_register_builtin_families does,
 * and everything after that goes through the generic API.
 *
 * Devices:
 *   "ppk2:<serial>"   a unit on USB (id "ppk2:<port path>" when the port has no USB serial number).
 *                     Its two serial ports are one device; open tries the measurement candidate
 *                     first (lowest USB interface number, unconfirmed: CLEANROOM.md question 1)
 *                     and falls back to the other.
 *   "sim:ppk2"        a simulated unit, always listed (unless LMB_ENUM_NO_SIMULATED). "sim:ppk2:<n>"
 *                     opens further independent simulators. It runs on the library's own PPK2
 *                     simulator through the real command builder and sample decoder, in process,
 *                     paced in real time at 100 kS/s; it needs no serial port, so it works on every platform.
 *                     The environment variable LMB_PPK2_SIMULATORS=<n> (1 to 8) lists n of them
 *                     ("sim:ppk2", "sim:ppk2:2", ...), for demos and for testing an application
 *                     with several devices.
 *
 * Facet behaviour specific to this family:
 *   - Starting a stream selects the external-supply (ammeter) mode and 3.3 V when nothing was
 *     chosen, so a plain "start" measures; the current formula needs a voltage in both modes.
 *   - The supply mode and voltage cannot change while a stream runs (LMB_ERR_STATE): the
 *     current calculation takes the voltage at start. The output switch can.
 *   - Current flows only while the DUT gate is on (PowerSupply output "DUT power"): with it off the
 *     stream reads zero, in ammeter mode as well as in source mode.
 *   - Closing stops the stream and disconnects the DUT.
 *   - The simulator also has the SimControl facet (load, pulsed load, lost samples, unplug, replug).
 */
#ifndef LIBMYBENCH_PPK2_FAMILY_H
#define LIBMYBENCH_PPK2_FAMILY_H

#include "libmybench/device.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register the family "ppk2" (hardware when the serial transport is built, and the simulator). */
LMB_API int lmb_ppk2_register(lmb_context *ctx);

/* Simulator controls, for tests and demos. LMB_ERR_INVALID_ARG unless dev is a simulated PPK2.
 * New code should use the generic SimControl facet (lmb_device_sim_control in facets.h), which a
 * simulated device of any family can offer and which needs no family header: these three remain for
 * compatibility and do the same. */

/** Constant current drawn by the simulated DUT (default 1 mA). Takes effect at once. */
LMB_API int lmb_ppk2_sim_set_load(lmb_device *dev, double amps);
/** Lose n samples on the simulated wire: the stream reports a gap. */
LMB_API int lmb_ppk2_sim_drop(lmb_device *dev, uint64_t n);
/** Pretend the unit was unplugged: every later call returns LMB_ERR_DISCONNECTED. */
LMB_API int lmb_ppk2_sim_disconnect(lmb_device *dev);

#ifdef __cplusplus
}
#endif

#endif
