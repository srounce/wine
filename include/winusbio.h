/*
 * Copyright (C) 2026 Samuel Rounce
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

#ifndef _WINUSBIO_H_
#define _WINUSBIO_H_

#include <ddk/usb200.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef USBD_PIPE_TYPE_DEFINED
#define USBD_PIPE_TYPE_DEFINED
typedef enum _USBD_PIPE_TYPE {
    UsbdPipeTypeControl,
    UsbdPipeTypeIsochronous,
    UsbdPipeTypeBulk,
    UsbdPipeTypeInterrupt
} USBD_PIPE_TYPE;
#endif

#ifndef USBD_STATUS_DEFINED
#define USBD_STATUS_DEFINED
typedef LONG USBD_STATUS;
#endif

#ifndef USBD_ISO_PACKET_DESCRIPTOR_DEFINED
#define USBD_ISO_PACKET_DESCRIPTOR_DEFINED
typedef struct _USBD_ISO_PACKET_DESCRIPTOR {
    ULONG Offset;
    ULONG Length;
    USBD_STATUS Status;
} USBD_ISO_PACKET_DESCRIPTOR;
typedef struct _USBD_ISO_PACKET_DESCRIPTOR *PUSBD_ISO_PACKET_DESCRIPTOR;
#endif

#define SHORT_PACKET_TERMINATE  0x01
#define AUTO_CLEAR_STALL        0x02
#define PIPE_TRANSFER_TIMEOUT   0x03
#define IGNORE_SHORT_PACKETS    0x04
#define ALLOW_PARTIAL_READS     0x05
#define AUTO_FLUSH              0x06
#define RAW_IO                  0x07
#define MAXIMUM_TRANSFER_SIZE   0x08
#define RESET_PIPE_ON_RESUME    0x09

#define AUTO_SUSPEND            0x81
#define SUSPEND_DELAY           0x83

#define DEVICE_SPEED            0x01

#define LowSpeed                0x01
#define FullSpeed               0x02
#define HighSpeed               0x03

#pragma pack(push,1)

typedef struct _WINUSB_PIPE_INFORMATION
{
    USBD_PIPE_TYPE PipeType;
    UCHAR PipeId;
    USHORT MaximumPacketSize;
    UCHAR Interval;
} WINUSB_PIPE_INFORMATION, *PWINUSB_PIPE_INFORMATION;

typedef struct _WINUSB_PIPE_INFORMATION_EX
{
    USBD_PIPE_TYPE PipeType;
    UCHAR PipeId;
    USHORT MaximumPacketSize;
    UCHAR Interval;
    ULONG MaximumBytesPerInterval;
} WINUSB_PIPE_INFORMATION_EX, *PWINUSB_PIPE_INFORMATION_EX;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* _WINUSBIO_H_ */
