/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Serial ports on Windows: SetupAPI (COM port interface class) to list them, the Win32 comm
 * API for I/O. USB CDC ACM ports use the inbox usbser.sys: no driver to install. */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cfgmgr32.h>
#include <setupapi.h>

#include "libmybench/serial.h"

#include "serial_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* GUID_DEVINTERFACE_COMPORT (ntddser.h), spelled out to avoid initguid.h ordering. */
static const GUID k_comport_guid = {0x86E0D1E0, 0x8089, 0x11D0, {0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73}};

struct lmb_serial {
    HANDLE h;
    DWORD read_timeout; /* what COMMTIMEOUTS is set to now, MAXDWORD - 1 = "wait forever" */
};

static int from_win32(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return LMB_SERIAL_ERR_NOT_FOUND;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        /* A COM port opened by another program gives ACCESS_DENIED. */
        return LMB_SERIAL_ERR_BUSY;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return LMB_SERIAL_ERR_NO_MEM;
    case ERROR_DEVICE_NOT_CONNECTED:
    case ERROR_BAD_COMMAND:
    case ERROR_GEN_FAILURE:
    case ERROR_OPERATION_ABORTED:
    case ERROR_DEVICE_REMOVED:
        return LMB_SERIAL_ERR_DISCONNECTED;
    default:
        return LMB_SERIAL_ERR_IO;
    }
}

static int set_read_timeout(lmb_serial *s, int timeout_ms)
{
    const DWORD want = timeout_ms < 0 ? MAXDWORD - 1 : (DWORD)timeout_ms;
    if (want == s->read_timeout)
        return LMB_SERIAL_OK;
    COMMTIMEOUTS t;
    memset(&t, 0, sizeof t);
    if (timeout_ms == 0) {
        t.ReadIntervalTimeout = MAXDWORD; /* return at once with what is there */
    } else if (timeout_ms < 0) {
        /* As below with the longest total timeout allowed (about 49 days): "until a byte". */
        t.ReadIntervalTimeout = MAXDWORD;
        t.ReadTotalTimeoutMultiplier = MAXDWORD;
        t.ReadTotalTimeoutConstant = MAXDWORD - 1;
    } else {
        /* MAXDWORD / MAXDWORD / n: return as soon as a byte arrives, or after n ms. */
        t.ReadIntervalTimeout = MAXDWORD;
        t.ReadTotalTimeoutMultiplier = MAXDWORD;
        t.ReadTotalTimeoutConstant = (DWORD)timeout_ms;
    }
    t.WriteTotalTimeoutConstant = 2000;
    if (!SetCommTimeouts(s->h, &t))
        return from_win32(GetLastError());
    s->read_timeout = want;
    return LMB_SERIAL_OK;
}

int lmb_serial_open(const char *path, unsigned baud, int dtr, lmb_serial **out)
{
    if (!path || !*path || !out || baud == 0)
        return LMB_SERIAL_ERR_INVALID_ARG;
    *out = NULL;
    /* COM10 and above only open through the \\.\ namespace; it works for COM1-9 too. */
    char dev[300];
    if (strncmp(path, "\\\\.\\", 4) == 0)
        lmb_serial_copy(dev, sizeof dev, path);
    else if ((size_t)snprintf(dev, sizeof dev, "\\\\.\\%s", path) >= sizeof dev)
        return LMB_SERIAL_ERR_INVALID_ARG;
    HANDLE h = CreateFileA(dev, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return from_win32(GetLastError());
    DCB dcb;
    memset(&dcb, 0, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (!GetCommState(h, &dcb)) {
        const DWORD e = GetLastError();
        CloseHandle(h);
        return e == ERROR_INVALID_FUNCTION ? LMB_SERIAL_ERR_INVALID_ARG : from_win32(e);
    }
    dcb.BaudRate = baud;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = dtr ? DTR_CONTROL_ENABLE : DTR_CONTROL_DISABLE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fAbortOnError = FALSE;
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    if (!SetCommState(h, &dcb)) {
        const DWORD e = GetLastError();
        CloseHandle(h);
        return from_win32(e);
    }
    SetupComm(h, 1 << 20, 1 << 12); /* a hint; usbser.sys may keep its own sizes */
    lmb_serial *s = malloc(sizeof *s);
    if (!s) {
        CloseHandle(h);
        return LMB_SERIAL_ERR_NO_MEM;
    }
    s->h = h;
    s->read_timeout = MAXDWORD; /* unset */
    const int rc = set_read_timeout(s, 0);
    if (rc != LMB_SERIAL_OK) {
        lmb_serial_close(s);
        return rc;
    }
    *out = s;
    return LMB_SERIAL_OK;
}

void lmb_serial_close(lmb_serial *s)
{
    if (!s)
        return;
    CloseHandle(s->h);
    free(s);
}

int lmb_serial_write(lmb_serial *s, const void *data, size_t len)
{
    if (!s || (len && !data))
        return LMB_SERIAL_ERR_INVALID_ARG;
    const unsigned char *p = data;
    while (len) {
        const DWORD chunk = len > 0x10000 ? 0x10000 : (DWORD)len;
        DWORD done = 0;
        if (!WriteFile(s->h, p, chunk, &done, NULL))
            return from_win32(GetLastError());
        if (done == 0)
            return LMB_SERIAL_ERR_IO; /* write timeout: the device takes no data */
        p += done;
        len -= done;
    }
    return LMB_SERIAL_OK;
}

int lmb_serial_read(lmb_serial *s, void *buf, size_t cap, int timeout_ms, size_t *got)
{
    if (!s || !got || (cap && !buf))
        return LMB_SERIAL_ERR_INVALID_ARG;
    *got = 0;
    if (cap == 0)
        return LMB_SERIAL_OK;
    const int rc = set_read_timeout(s, timeout_ms);
    if (rc != LMB_SERIAL_OK)
        return rc;
    DWORD n = 0;
    if (!ReadFile(s->h, buf, cap > 0x100000 ? 0x100000 : (DWORD)cap, &n, NULL))
        return from_win32(GetLastError());
    *got = n;
    return LMB_SERIAL_OK;
}

int lmb_serial_flush_input(lmb_serial *s)
{
    if (!s)
        return LMB_SERIAL_ERR_INVALID_ARG;
    return PurgeComm(s->h, PURGE_RXCLEAR | PURGE_RXABORT) ? LMB_SERIAL_OK : from_win32(GetLastError());
}

/* ------------------------------------------------------------------------------------------ */
/* Listing                                                                                     */

/* "...VID_1915&PID_C00A&MI_01..." -> numbers. Returns 0 when VID and PID were found. */
static int parse_hwid(const char *id, uint16_t *vid, uint16_t *pid, int *mi)
{
    const char *v = strstr(id, "VID_");
    const char *p = strstr(id, "PID_");
    const char *m = strstr(id, "MI_");
    if (!v || !p)
        return -1;
    *vid = (uint16_t)strtoul(v + 4, NULL, 16);
    *pid = (uint16_t)strtoul(p + 4, NULL, 16);
    *mi = m ? (int)strtol(m + 3, NULL, 16) : -1;
    return 0;
}

int lmb_serial_list(lmb_serial_port *ports, size_t cap, size_t *count)
{
    if (!count || (cap && !ports))
        return LMB_SERIAL_ERR_INVALID_ARG;
    *count = 0;
    HDEVINFO set = SetupDiGetClassDevsA(&k_comport_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
        return from_win32(GetLastError());
    lmb_serial_list_builder b;
    lmb_serial_list_begin(&b, ports, cap);
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof info;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++) {
        lmb_serial_port port;
        memset(&port, 0, sizeof port);
        port.usb_interface = -1;

        HKEY key = SetupDiOpenDevRegKey(set, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;
        char name[64] = "";
        DWORD type = 0, size = sizeof name - 1;
        const LONG r = RegQueryValueExA(key, "PortName", NULL, &type, (LPBYTE)name, &size);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS || type != REG_SZ || !name[0])
            continue;
        name[sizeof name - 1] = '\0';
        lmb_serial_copy(port.path, sizeof port.path, name);

        char hwid[512] = "";
        if (SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_HARDWAREID, NULL, (PBYTE)hwid, sizeof hwid - 1, NULL))
            parse_hwid(hwid, &port.usb_vid, &port.usb_pid, &port.usb_interface);
        char friendly[256] = "";
        if (SetupDiGetDeviceRegistryPropertyA(set, &info, SPDRP_FRIENDLYNAME, NULL, (PBYTE)friendly,
                                              sizeof friendly - 1, NULL))
            lmb_serial_copy(port.description, sizeof port.description, friendly);

        /* The serial number is the last part of the USB device's instance ID: the port's own
         * for a single-function device, its parent's (the composite device) otherwise. */
        if (port.usb_vid) {
            DEVINST inst = info.DevInst;
            if (port.usb_interface >= 0) {
                DEVINST parent;
                if (CM_Get_Parent(&parent, inst, 0) == CR_SUCCESS)
                    inst = parent;
            }
            char instance[MAX_DEVICE_ID_LEN] = "";
            if (CM_Get_Device_IDA(inst, instance, sizeof instance, 0) == CR_SUCCESS) {
                const char *last = strrchr(instance, '\\');
                /* Windows makes up an ID with '&' when the device has no serial number. */
                if (last && !strchr(last + 1, '&'))
                    lmb_serial_copy(port.serial_number, sizeof port.serial_number, last + 1);
            }
        }
        lmb_serial_list_add(&b, &port);
    }
    SetupDiDestroyDeviceInfoList(set);
    *count = lmb_serial_list_end(&b);
    return LMB_SERIAL_OK;
}
