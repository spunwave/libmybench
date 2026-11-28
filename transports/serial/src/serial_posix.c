/* SPDX-License-Identifier: MPL-2.0
 * SPDX-FileCopyrightText: 2026 Martin Dolez
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy
 * of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/* Serial ports on POSIX: termios for I/O, sysfs (Linux) or IOKit (macOS) to list them. */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#endif

#include "libmybench/serial.h"

#include "serial_common.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__)
#include <dirent.h>
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>
#endif

struct lmb_serial {
    int fd;
};

static int from_errno(int e)
{
    switch (e) {
    case ENOENT:
    case ENODEV:
        return LMB_SERIAL_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:
        return LMB_SERIAL_ERR_ACCESS;
    case EBUSY:
    case EWOULDBLOCK:
        return LMB_SERIAL_ERR_BUSY;
    case EIO:
    case ENXIO:
        return LMB_SERIAL_ERR_DISCONNECTED;
    case ENOMEM:
        return LMB_SERIAL_ERR_NO_MEM;
    default:
        return LMB_SERIAL_ERR_IO;
    }
}

static int speed_for(unsigned baud, speed_t *out)
{
    static const struct {
        unsigned baud;
        speed_t speed;
    } k_speeds[] = {
        {1200, B1200},     {2400, B2400},     {4800, B4800},   {9600, B9600},
        {19200, B19200},   {38400, B38400},   {57600, B57600}, {115200, B115200},
        {230400, B230400},
#ifdef B460800
        {460800, B460800},
#endif
#ifdef B921600
        {921600, B921600},
#endif
    };
    for (size_t i = 0; i < sizeof k_speeds / sizeof k_speeds[0]; i++)
        if (k_speeds[i].baud == baud) {
            *out = k_speeds[i].speed;
            return 0;
        }
    return -1;
}

int lmb_serial_open(const char *path, unsigned baud, int dtr, lmb_serial **out)
{
    speed_t speed;
    if (!path || !*path || !out || speed_for(baud, &speed) != 0)
        return LMB_SERIAL_ERR_INVALID_ARG;
    *out = NULL;
    const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return from_errno(errno);
    if (!isatty(fd)) {
        close(fd);
        return LMB_SERIAL_ERR_INVALID_ARG;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int e = errno;
        close(fd);
        return e == EWOULDBLOCK ? LMB_SERIAL_ERR_BUSY : from_errno(e);
    }
    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        const int e = errno;
        close(fd);
        return from_errno(e);
    }
    cfmakeraw(&t);
    t.c_cflag |= CLOCAL | CREAD;
    t.c_cflag &= (tcflag_t)~(CSTOPB | PARENB | CSIZE);
    t.c_cflag |= CS8;
#ifdef CRTSCTS
    t.c_cflag &= (tcflag_t)~CRTSCTS;
#endif
    t.c_iflag &= (tcflag_t)~(IXON | IXOFF | IXANY);
    /* VMIN 1: a non-blocking read with nothing there fails with EAGAIN. With VMIN 0 it would
     * return 0, which can't be told apart from a hang-up (end of file). */
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    cfsetispeed(&t, speed);
    cfsetospeed(&t, speed);
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        const int e = errno;
        close(fd);
        return from_errno(e);
    }
    /* Modem lines. A pseudo-terminal has none (ENOTTY / EINVAL): fine, it needs no DTR. */
    int lines = TIOCM_RTS;
    if (ioctl(fd, TIOCMBIS, &lines) != 0 && errno != ENOTTY && errno != EINVAL) {
        const int e = errno;
        close(fd);
        return from_errno(e);
    }
    lines = TIOCM_DTR;
    if (ioctl(fd, dtr ? TIOCMBIS : TIOCMBIC, &lines) != 0 && errno != ENOTTY && errno != EINVAL) {
        const int e = errno;
        close(fd);
        return from_errno(e);
    }
    lmb_serial *s = malloc(sizeof *s);
    if (!s) {
        close(fd);
        return LMB_SERIAL_ERR_NO_MEM;
    }
    s->fd = fd;
    *out = s;
    return LMB_SERIAL_OK;
}

void lmb_serial_close(lmb_serial *s)
{
    if (!s)
        return;
    close(s->fd); /* also releases the flock */
    free(s);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Waits for events on the port. 1 ready, 0 timeout, < 0 error. */
static int wait_fd(int fd, short events, int timeout_ms)
{
    const long long deadline = timeout_ms < 0 ? 0 : now_ms() + timeout_ms;
    for (;;) {
        int left = -1;
        if (timeout_ms >= 0) {
            const long long d = deadline - now_ms();
            left = d < 0 ? 0 : d > INT_MAX ? INT_MAX : (int)d;
        }
        struct pollfd p = {fd, events, 0};
        const int n = poll(&p, 1, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            return from_errno(errno);
        if (n == 0)
            return 0;
        if (p.revents & POLLNVAL)
            return LMB_SERIAL_ERR_INVALID_ARG;
        /* POLLHUP / POLLERR: the following read or write reports what happened. */
        return 1;
    }
}

int lmb_serial_write(lmb_serial *s, const void *data, size_t len)
{
    if (!s || (len && !data))
        return LMB_SERIAL_ERR_INVALID_ARG;
    const unsigned char *p = data;
    while (len) {
        const ssize_t n = write(s->fd, p, len);
        if (n > 0) {
            p += n;
            len -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            return from_errno(errno);
        const int w = wait_fd(s->fd, POLLOUT, 2000);
        if (w < 0)
            return w;
        if (w == 0)
            return LMB_SERIAL_ERR_IO; /* the device takes no data */
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
    for (;;) {
        const ssize_t n = read(s->fd, buf, cap);
        if (n > 0) {
            *got = (size_t)n;
            return LMB_SERIAL_OK;
        }
        if (n == 0)
            return LMB_SERIAL_ERR_DISCONNECTED; /* hang-up (a closed pseudo-terminal, a gone modem) */
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return from_errno(errno);
        if (timeout_ms == 0)
            return LMB_SERIAL_OK;
        const int w = wait_fd(s->fd, POLLIN, timeout_ms);
        if (w < 0)
            return w;
        if (w == 0)
            return LMB_SERIAL_OK;
        timeout_ms = 0; /* data (or an error) is there now */
    }
}

int lmb_serial_flush_input(lmb_serial *s)
{
    if (!s)
        return LMB_SERIAL_ERR_INVALID_ARG;
    return tcflush(s->fd, TCIFLUSH) == 0 ? LMB_SERIAL_OK : from_errno(errno);
}

/* ------------------------------------------------------------------------------------------ */
/* Listing                                                                                     */

#if defined(__linux__)

/* First line of a sysfs attribute file, trimmed. 0 on success. */
static int read_attr(const char *dir, const char *name, char *out, size_t cap)
{
    char path[PATH_MAX];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, name) >= sizeof path)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    const int ok = fgets(out, (int)cap, f) != NULL;
    fclose(f);
    if (!ok)
        return -1;
    out[strcspn(out, "\r\n")] = '\0';
    return 0;
}

static void parent_dir(char *path)
{
    char *slash = strrchr(path, '/');
    if (slash && slash != path)
        *slash = '\0';
}

/* USB identity of /sys/class/tty/<name>: walk up from its device to the interface (has
 * bInterfaceNumber) and the device above it (has idVendor). 0 if it is a USB port. */
static int usb_identity(const char *name, lmb_serial_port *port)
{
    char link[PATH_MAX], dir[PATH_MAX];
    if ((size_t)snprintf(link, sizeof link, "/sys/class/tty/%s/device", name) >= sizeof link)
        return -1;
    if (!realpath(link, dir))
        return -1; /* virtual terminal: no device */
    char value[128];
    for (int up = 0; up < 3; up++) {
        if (read_attr(dir, "bInterfaceNumber", value, sizeof value) == 0) {
            port->usb_interface = (int)strtol(value, NULL, 16);
            parent_dir(dir);
            char vid[16], pid[16];
            if (read_attr(dir, "idVendor", vid, sizeof vid) != 0 || read_attr(dir, "idProduct", pid, sizeof pid) != 0)
                return -1;
            port->usb_vid = (uint16_t)strtoul(vid, NULL, 16);
            port->usb_pid = (uint16_t)strtoul(pid, NULL, 16);
            if (read_attr(dir, "serial", value, sizeof value) == 0)
                lmb_serial_copy(port->serial_number, sizeof port->serial_number, value);
            if (read_attr(dir, "product", value, sizeof value) == 0)
                lmb_serial_copy(port->description, sizeof port->description, value);
            return 0;
        }
        parent_dir(dir);
    }
    return -1;
}

int lmb_serial_list(lmb_serial_port *ports, size_t cap, size_t *count)
{
    if (!count || (cap && !ports))
        return LMB_SERIAL_ERR_INVALID_ARG;
    *count = 0;
    DIR *d = opendir("/sys/class/tty");
    if (!d)
        return LMB_SERIAL_OK; /* no sysfs (a container): nothing to list */
    lmb_serial_list_builder b;
    lmb_serial_list_begin(&b, ports, cap);
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        lmb_serial_port port;
        memset(&port, 0, sizeof port);
        port.usb_interface = -1;
        if (usb_identity(e->d_name, &port) != 0)
            continue;
        if ((size_t)snprintf(port.path, sizeof port.path, "/dev/%s", e->d_name) >= sizeof port.path)
            continue;
        lmb_serial_list_add(&b, &port);
    }
    closedir(d);
    *count = lmb_serial_list_end(&b);
    return LMB_SERIAL_OK;
}

#elif defined(__APPLE__)

static int cf_int(io_registry_entry_t entry, CFStringRef key, int *out)
{
    CFTypeRef v = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault,
                                                  kIORegistryIterateRecursively | kIORegistryIterateParents);
    if (!v)
        return -1;
    int ok = CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, out);
    CFRelease(v);
    return ok ? 0 : -1;
}

static void cf_str(io_registry_entry_t entry, CFStringRef key, int parents, char *out, size_t cap)
{
    const IOOptionBits opts = parents ? (kIORegistryIterateRecursively | kIORegistryIterateParents) : 0;
    CFTypeRef v = IORegistryEntrySearchCFProperty(entry, kIOServicePlane, key, kCFAllocatorDefault, opts);
    if (!v)
        return;
    if (CFGetTypeID(v) == CFStringGetTypeID())
        CFStringGetCString((CFStringRef)v, out, (CFIndex)cap, kCFStringEncodingUTF8);
    CFRelease(v);
}

int lmb_serial_list(lmb_serial_port *ports, size_t cap, size_t *count)
{
    if (!count || (cap && !ports))
        return LMB_SERIAL_ERR_INVALID_ARG;
    *count = 0;
    CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
    if (!match)
        return LMB_SERIAL_ERR_NO_MEM;
    io_iterator_t it;
    /* kIOMainPortDefault is 0, like the older kIOMasterPortDefault. */
    if (IOServiceGetMatchingServices(0, match, &it) != KERN_SUCCESS)
        return LMB_SERIAL_ERR_IO;
    lmb_serial_list_builder b;
    lmb_serial_list_begin(&b, ports, cap);
    io_object_t service;
    while ((service = IOIteratorNext(it)) != 0) {
        lmb_serial_port port;
        memset(&port, 0, sizeof port);
        port.usb_interface = -1;
        cf_str(service, CFSTR(kIOCalloutDeviceKey), 0, port.path, sizeof port.path);
        int vid, pid, intf;
        if (port.path[0] && cf_int(service, CFSTR("idVendor"), &vid) == 0 &&
            cf_int(service, CFSTR("idProduct"), &pid) == 0) {
            port.usb_vid = (uint16_t)vid;
            port.usb_pid = (uint16_t)pid;
            if (cf_int(service, CFSTR("bInterfaceNumber"), &intf) == 0)
                port.usb_interface = intf;
            cf_str(service, CFSTR("USB Serial Number"), 1, port.serial_number, sizeof port.serial_number);
            cf_str(service, CFSTR("USB Product Name"), 1, port.description, sizeof port.description);
            lmb_serial_list_add(&b, &port);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(it);
    *count = lmb_serial_list_end(&b);
    return LMB_SERIAL_OK;
}

#else

int lmb_serial_list(lmb_serial_port *ports, size_t cap, size_t *count)
{
    (void)ports;
    (void)cap;
    if (!count)
        return LMB_SERIAL_ERR_INVALID_ARG;
    *count = 0;
    return LMB_SERIAL_ERR_UNSUPPORTED;
}

#endif
