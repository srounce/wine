/*
 * Copyright (C) the Wine project
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

#ifndef __DDK_USB200_H__
#define __DDK_USB200_H__

#include <ddk/usb100.h>

typedef enum _USB_DEVICE_TYPE
{
    Usb11Device = 0,
    Usb20Device,
} USB_DEVICE_TYPE;

typedef enum _USB_DEVICE_SPEED
{
    UsbLowSpeed = 0,
    UsbFullSpeed,
    UsbHighSpeed,
    UsbSuperSpeed,
} USB_DEVICE_SPEED;

#define USB_INTERFACE_ASSOCIATION_DESCRIPTOR_TYPE 0x0b

#pragma pack(push,1)

typedef struct _USB_INTERFACE_ASSOCIATION_DESCRIPTOR
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bFirstInterface;
    UCHAR bInterfaceCount;
    UCHAR bFunctionClass;
    UCHAR bFunctionSubClass;
    UCHAR bFunctionProtocol;
    UCHAR iFunction;
} USB_INTERFACE_ASSOCIATION_DESCRIPTOR, *PUSB_INTERFACE_ASSOCIATION_DESCRIPTOR;

typedef struct _USB_DEVICE_QUALIFIER_DESCRIPTOR
{
    UCHAR bLength;
    UCHAR bDescriptorType;
    USHORT bcdUSB;
    UCHAR bDeviceClass;
    UCHAR bDeviceSubClass;
    UCHAR bDeviceProtocol;
    UCHAR bMaxPacketSize0;
    UCHAR bNumConfigurations;
    UCHAR bReserved;
} USB_DEVICE_QUALIFIER_DESCRIPTOR, *PUSB_DEVICE_QUALIFIER_DESCRIPTOR;

#pragma pack(pop)

#endif
