/*
 * Copyright (C) 2013 Damjan Jovanovic
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

#ifndef __DDK_USBIOCTL_H__
#define __DDK_USBIOCTL_H__

#include "ddk/usbiodef.h"
#include "ddk/usb200.h"

#define IOCTL_INTERNAL_USB_SUBMIT_URB  \
  CTL_CODE(FILE_DEVICE_USB, USB_SUBMIT_URB, METHOD_NEITHER, FILE_ANY_ACCESS)

#define IOCTL_USB_GET_NODE_INFORMATION \
  CTL_CODE(FILE_DEVICE_USB, USB_GET_NODE_INFORMATION, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_GET_NODE_CONNECTION_INFORMATION \
  CTL_CODE(FILE_DEVICE_USB, USB_GET_NODE_CONNECTION_INFORMATION, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION \
  CTL_CODE(FILE_DEVICE_USB, USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_GET_NODE_CONNECTION_NAME \
  CTL_CODE(FILE_DEVICE_USB, USB_GET_NODE_CONNECTION_NAME, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_GET_NODE_CONNECTION_DRIVERKEY_NAME \
  CTL_CODE(FILE_DEVICE_USB, USB_GET_NODE_CONNECTION_DRIVERKEY_NAME, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX \
  CTL_CODE(FILE_DEVICE_USB, USB_GET_NODE_CONNECTION_INFORMATION_EX, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2 \
  CTL_CODE(FILE_DEVICE_USB, USB_GET_NODE_CONNECTION_INFORMATION_EX_V2, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USB_GET_ROOT_HUB_NAME \
  CTL_CODE(FILE_DEVICE_USB, HCD_GET_ROOT_HUB_NAME, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef enum _USB_HUB_NODE
{
    UsbHub,
    UsbMIParent,
} USB_HUB_NODE;

typedef enum _USB_CONNECTION_STATUS
{
    NoDeviceConnected,
    DeviceConnected,
    DeviceFailedEnumeration,
    DeviceGeneralFailure,
    DeviceCausedOvercurrent,
    DeviceNotEnoughPower,
    DeviceNotEnoughBandwidth,
    DeviceHubNestedTooDeeply,
    DeviceInLegacyHub,
    DeviceEnumerating,
    DeviceReset,
} USB_CONNECTION_STATUS;

#pragma pack(push,1)

typedef struct _USB_HUB_INFORMATION
{
    USB_HUB_DESCRIPTOR HubDescriptor;
    BOOLEAN HubIsBusPowered;
} USB_HUB_INFORMATION, *PUSB_HUB_INFORMATION;

typedef struct _USB_MI_PARENT_INFORMATION
{
    ULONG NumberOfInterfaces;
} USB_MI_PARENT_INFORMATION, *PUSB_MI_PARENT_INFORMATION;

typedef struct _USB_NODE_INFORMATION
{
    USB_HUB_NODE NodeType;
    union
    {
        USB_HUB_INFORMATION HubInformation;
        USB_MI_PARENT_INFORMATION MiParentInformation;
    } u;
} USB_NODE_INFORMATION, *PUSB_NODE_INFORMATION;

typedef struct _USB_PIPE_INFO
{
    USB_ENDPOINT_DESCRIPTOR EndpointDescriptor;
    ULONG ScheduleOffset;
} USB_PIPE_INFO, *PUSB_PIPE_INFO;

typedef struct _USB_NODE_CONNECTION_INFORMATION
{
    ULONG ConnectionIndex;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    UCHAR CurrentConfigurationValue;
    BOOLEAN LowSpeed;
    BOOLEAN DeviceIsHub;
    USHORT DeviceAddress;
    ULONG NumberOfOpenPipes;
    USB_CONNECTION_STATUS ConnectionStatus;
    USB_PIPE_INFO PipeList[1];
} USB_NODE_CONNECTION_INFORMATION, *PUSB_NODE_CONNECTION_INFORMATION;

typedef struct _USB_NODE_CONNECTION_INFORMATION_EX
{
    ULONG ConnectionIndex;
    USB_DEVICE_DESCRIPTOR DeviceDescriptor;
    UCHAR CurrentConfigurationValue;
    UCHAR Speed;
    BOOLEAN DeviceIsHub;
    USHORT DeviceAddress;
    ULONG NumberOfOpenPipes;
    USB_CONNECTION_STATUS ConnectionStatus;
    USB_PIPE_INFO PipeList[1];
} USB_NODE_CONNECTION_INFORMATION_EX, *PUSB_NODE_CONNECTION_INFORMATION_EX;

typedef union _USB_PROTOCOLS
{
    ULONG ul;
    struct
    {
        ULONG Usb110 : 1;
        ULONG Usb200 : 1;
        ULONG Usb300 : 1;
        ULONG ReservedMBZ : 29;
    } DUMMYSTRUCTNAME;
} USB_PROTOCOLS, *PUSB_PROTOCOLS;

typedef union _USB_NODE_CONNECTION_INFORMATION_EX_V2_FLAGS
{
    ULONG ul;
    struct
    {
        ULONG DeviceIsOperatingAtSuperSpeedOrHigher : 1;
        ULONG DeviceIsSuperSpeedCapableOrHigher : 1;
        ULONG DeviceIsOperatingAtSuperSpeedPlusOrHigher : 1;
        ULONG DeviceIsSuperSpeedPlusCapableOrHigher : 1;
        ULONG ReservedMBZ : 28;
    } DUMMYSTRUCTNAME;
} USB_NODE_CONNECTION_INFORMATION_EX_V2_FLAGS, *PUSB_NODE_CONNECTION_INFORMATION_EX_V2_FLAGS;

typedef struct _USB_NODE_CONNECTION_INFORMATION_EX_V2
{
    ULONG ConnectionIndex;
    ULONG Length;
    USB_PROTOCOLS SupportedUsbProtocols;
    USB_NODE_CONNECTION_INFORMATION_EX_V2_FLAGS Flags;
} USB_NODE_CONNECTION_INFORMATION_EX_V2, *PUSB_NODE_CONNECTION_INFORMATION_EX_V2;

typedef struct _USB_NODE_CONNECTION_NAME
{
    ULONG ConnectionIndex;
    ULONG ActualLength;
    WCHAR NodeName[1];
} USB_NODE_CONNECTION_NAME, *PUSB_NODE_CONNECTION_NAME;

typedef struct _USB_NODE_CONNECTION_DRIVERKEY_NAME
{
    ULONG ConnectionIndex;
    ULONG ActualLength;
    WCHAR DriverKeyName[1];
} USB_NODE_CONNECTION_DRIVERKEY_NAME, *PUSB_NODE_CONNECTION_DRIVERKEY_NAME;

typedef struct _USB_DESCRIPTOR_REQUEST
{
    ULONG ConnectionIndex;
    struct
    {
        UCHAR bmRequest;
        UCHAR bRequest;
        USHORT wValue;
        USHORT wIndex;
        USHORT wLength;
    } SetupPacket;
    UCHAR Data[1];
} USB_DESCRIPTOR_REQUEST, *PUSB_DESCRIPTOR_REQUEST;

typedef struct _USB_ROOT_HUB_NAME
{
    ULONG ActualLength;
    WCHAR RootHubName[1];
} USB_ROOT_HUB_NAME, *PUSB_ROOT_HUB_NAME;

#pragma pack(pop)

#endif /* __DDK_USBIOCTL_H__ */
