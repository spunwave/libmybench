/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * Driver interface: what a device family implements to plug into a context. Applications
 * don't include this header.
 *
 * A family is a name plus three functions. The context asks it to list its devices, and to open
 * one by id; an open device is a small ops table plus the facet vtables the driver owns. The
 * core owns the lmb_device handle, the busy check and the facet validation, so a driver only
 * contains its own logic. Families register explicitly (no static constructors, which don't
 * survive static libraries and WebAssembly); a registration function per family is the whole
 * coupling, and a family can later live in a plugin loaded at run time without changing this.
 *
 * Rules for drivers:
 *   - No global mutable state: everything hangs off the `user` pointer and the open device.
 *   - enumerate and open may be called from any thread, concurrently with calls on open devices.
 *   - A facet's functions are called by one thread at a time per facet, and different facets of
 *     one device may be called concurrently: the driver locks what they share (lmb_mutex below).
 *   - Never block longer than the timeout you were given, and never forever without one.
 */
#ifndef LIBMYBENCH_DRIVER_H
#define LIBMYBENCH_DRIVER_H

#include "libmybench/device.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The open half of a device, filled by lmb_family.open. */
typedef struct lmb_device_ops {
    uint32_t size;  /**< sizeof(lmb_device_ops) */
    void *drv;      /**< the driver's state, passed to every function below */
    /** Stop everything, safe state, release the hardware, free drv. Required. */
    void (*close)(void *drv);
    /** The vtable of a facet (lifetime: until close), or LMB_ERR_NO_FACET. Required. The vtable's
     *  abi_version must be LMB_ABI_VERSION and its size at least the header (core checks). */
    int (*facet)(void *drv, lmb_facet_kind kind, const void **out);
    /** LMB_OK or LMB_ERR_DISCONNECTED. Optional (NULL: always LMB_OK). */
    int (*status)(void *drv);
} lmb_device_ops;

typedef struct lmb_family {
    uint32_t abi_version; /**< LMB_ABI_VERSION the driver was built with */
    uint32_t size;        /**< sizeof(lmb_family) */
    const char *name;     /**< "ppk2"; at most 31 bytes, unique in a context */
    void *user;           /**< passed to the functions below */
    /** List devices like lmb_enumerate does (up to cap into out, *count = total). Required. */
    int (*enumerate)(void *user, uint32_t flags, lmb_device_info *out, size_t cap, size_t *count);
    /**
     * Open the device with this id. LMB_ERR_NOT_FOUND when the id is not this family's, or the
     * device is gone: the core then asks the next family. On success fill *ops and *info.
     */
    int (*open)(void *user, const char *id, lmb_device_ops *ops, lmb_device_info *info);
    /** Called when the context is freed, after its devices were closed. Optional. */
    void (*destroy)(void *user);
} lmb_family;

/**
 * Add a family (the struct is copied, `name` too). LMB_ERR_ABI when abi_version differs,
 * LMB_ERR_EXISTS for a name already registered. Call before the context is shared between
 * threads. On failure, destroy is NOT called: the caller still owns `user`.
 */
LMB_API int lmb_context_register_family(lmb_context *ctx, const lmb_family *family);

/** Driver state of an open device if it belongs to this family, else NULL. For a family's own
 *  control functions (simulator controls, vendor extras). */
LMB_API void *lmb_device_driver_data(lmb_device *dev, const char *family_name);

/* Small portable helpers for drivers, so they need no platform code of their own. */

typedef struct lmb_mutex lmb_mutex;
LMB_API lmb_mutex *lmb_mutex_new(void);
LMB_API void lmb_mutex_free(lmb_mutex *m);
LMB_API void lmb_mutex_lock(lmb_mutex *m);
LMB_API void lmb_mutex_unlock(lmb_mutex *m);

/** Seconds on a monotonic clock (arbitrary origin). */
LMB_API double lmb_monotonic_s(void);
LMB_API void lmb_sleep_ms(unsigned ms);

#ifdef __cplusplus
}
#endif

#endif
