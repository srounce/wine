/*
 * The Wine project - Wine-private interface between winusb.dll and winusb.sys
 *
 * Copyright 2026 Samuel Rounce
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINE_WINE_WINUSB_H
#define __WINE_WINE_WINUSB_H

/* The native winusb.dll <-> winusb.sys protocol is undocumented; Wine
 * controls both ends, so it defines its own. Nothing outside dlls/winusb and
 * dlls/winusb.sys may depend on it. */

/* Output: ULONG total size of the descriptor set, followed by as much of the
 * cached device descriptor plus raw configuration descriptors as fits. */
#define IOCTL_WINE_WINUSB_GET_DESCRIPTORS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Input: struct wine_winusb_control_transfer_params, followed by the data
 * stage for host-to-device transfers. Output: the data stage for
 * device-to-host transfers. */
#define IOCTL_WINE_WINUSB_CONTROL_TRANSFER CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Input: struct wine_winusb_pipe_params. */
#define IOCTL_WINE_WINUSB_RESET_PIPE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Input: struct wine_winusb_pipe_params. */
#define IOCTL_WINE_WINUSB_ABORT_PIPE       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* Input: struct wine_winusb_set_alt_setting_params. */
#define IOCTL_WINE_WINUSB_SET_ALT_SETTING  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Transfer on the pipe encoded in the IOCTL code, so that the data buffer
 * needs no header and asynchronous requests pass application buffers
 * through unchanged. Direction follows the endpoint address: device-to-host
 * pipes read into the output buffer, host-to-device pipes write from the
 * input buffer. */
#define WINE_WINUSB_TRANSFER_FUNCTION_BASE 0xa00
#define IOCTL_WINE_WINUSB_TRANSFER(pipe) \
    CTL_CODE(FILE_DEVICE_UNKNOWN, WINE_WINUSB_TRANSFER_FUNCTION_BASE | (pipe), METHOD_BUFFERED, FILE_ANY_ACCESS)

struct wine_winusb_control_transfer_params
{
    UCHAR setup[8];
};

struct wine_winusb_pipe_params
{
    UCHAR pipe;
};

struct wine_winusb_set_alt_setting_params
{
    UCHAR interface_number;
    UCHAR alt_setting;
};

#endif /* __WINE_WINE_WINUSB_H */
