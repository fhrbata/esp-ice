/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file serial.h
 * @brief Small portable serial-port API.
 *
 * Wraps the bare minimum that the ESPLoader protocol needs: open a
 * device by path, set the baud rate, read with a millisecond timeout,
 * write blocking, toggle DTR/RTS for the reset sequence, and drop
 * the input buffer.  Everything is 8N1 / no flow control -- that is
 * all ESP chips speak.
 *
 * @c struct @c serial is opaque: each platform defines the full
 * layout in its own @c platform/{posix,win}/serial.c, so this header
 * contains zero @c #ifdef gates.  @ref serial_open allocates the
 * struct; @ref serial_close frees it.
 *
 * Error convention:
 *   - Setup functions return 0 on success or @c -errno on failure.
 *   - @ref serial_read / @ref serial_write return the number of bytes
 *     transferred (>= 0) or -1 with @c errno set on failure.
 *     @ref serial_read returns 0 on timeout.
 */
#ifndef SERIAL_H
#define SERIAL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/** Opaque, defined per-platform in platform/{posix,win}/serial.c. */
struct serial;

/**
 * @brief Open @p path as an 8N1 raw serial device.
 *
 * Allocates a new @c struct @c serial and returns it via @p out.
 * On failure @p *out is set to @c NULL and the return value is a
 * negative errno.
 *
 * @return 0 on success, -errno on failure.
 */
int serial_open(struct serial **out, const char *path);

/** Close the port and free @p s.  Safe to call on @c NULL. */
void serial_close(struct serial *s);

/**
 * @brief Change the baud rate.
 * @return 0 on success, -errno on failure.
 */
int serial_set_baud(struct serial *s, unsigned baud);

/**
 * @brief Read up to @p n bytes, waiting up to @p timeout_ms.
 * @return bytes read (> 0), 0 on timeout, -1 on error (errno set).
 */
ssize_t serial_read(struct serial *s, void *buf, size_t n, unsigned timeout_ms);

/**
 * @brief Write @p n bytes, retrying on partial writes and @c EINTR.
 * @return bytes written on success (always @p n), -1 on error.
 */
ssize_t serial_write(struct serial *s, const void *buf, size_t n);

/**
 * @brief Assert or deassert the DTR modem line.
 * @return 0 on success, -errno on failure.
 */
int serial_set_dtr(struct serial *s, int on);

/**
 * @brief Assert or deassert the RTS modem line.
 * @return 0 on success, -errno on failure.
 */
int serial_set_rts(struct serial *s, int on);

/**
 * @brief Set DTR and RTS atomically in a single ioctl / driver call.
 *
 * Changing the two lines in two separate calls leaves an intermediate
 * state visible on the wire between them.  On some USB-UART bridges
 * (CP2102/CH340/FT232) each TIOCMBIS / TIOCMBIC turns into a separate
 * USB control transfer, so the glitch window is large enough for the
 * ESP chip to sample BOOT incorrectly at reset release.  Use this
 * function in timing-sensitive sequences (bootloader entry, hard
 * reset) where both lines must change together.
 *
 * @return 0 on success, -errno on failure.
 */
int serial_set_dtr_rts(struct serial *s, int dtr, int rts);

/**
 * @brief Discard any bytes currently buffered on the input side.
 * @return 0 on success, -errno on failure.
 */
int serial_flush_input(struct serial *s);

/**
 * @brief Enumerate available serial ports on the host.
 *
 * Allocates and fills an array of NUL-terminated device-path strings
 * (e.g. "/dev/ttyUSB0", "COM3") and stores it in @p *out.  The caller
 * must release the list with @ref serial_free_port_list.
 *
 * On POSIX the function globs @c /dev/ttyUSB*, @c /dev/ttyACM*, and
 * @c /dev/cu.* (macOS).  On Windows it reads the registry key
 * @c HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM.
 *
 * @param[out] out  Receives a newly-allocated array of heap strings.
 *                  Set to @c NULL if the count is 0 or on failure.
 * @return          Number of entries (>= 0), or -errno on failure.
 */
int serial_list_ports(char ***out);

/**
 * @brief Free a port list returned by @ref serial_list_ports.
 *
 * The array is NULL-terminated, so no separate count is needed.
 *
 * @param ports  Array pointer returned by serial_list_ports.
 */
void serial_free_port_list(char **ports);

/**
 * @brief Read the USB VID and PID for a serial device.
 *
 * On Linux this is read from sysfs.  On other platforms the function
 * always returns -1.
 *
 * @param device  Device path, e.g. "/dev/ttyACM0".
 * @param vid     Receives the USB Vendor ID.
 * @param pid     Receives the USB Product ID.
 * @return 0 on success, -1 if the information is unavailable.
 */
int serial_get_usb_id(const char *device, unsigned int *vid, unsigned int *pid);

#endif /* SERIAL_H */
