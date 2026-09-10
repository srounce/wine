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

#ifndef __DDK_USBIODEF_H__
#define __DDK_USBIODEF_H__

#include <guiddef.h>

#define USB_SUBMIT_URB                            0
#define USB_RESET_PORT                            1
#define USB_GET_ROOT_HUB_NAME                     3
#define USB_GET_PORT_STATUS                       4
#define USB_ENABLE_PORT                           5
#define USB_GET_HUB_COUNT                         6
#define USB_CYCLE_PORT                            7
#define USB_GET_HUB_NAME                          8
#define USB_IDLE_NOTIFICATION                     9

#define USB_GET_NODE_INFORMATION                  258
#define USB_GET_NODE_CONNECTION_INFORMATION       259
#define USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION   260
#define USB_GET_NODE_CONNECTION_NAME              261
#define USB_DIAG_IGNORE_HUBS_ON                   262
#define USB_DIAG_IGNORE_HUBS_OFF                  263
#define USB_GET_NODE_CONNECTION_DRIVERKEY_NAME    264
#define USB_GET_HUB_CAPABILITIES                  271
#define USB_GET_NODE_CONNECTION_ATTRIBUTES        272
#define USB_HUB_CYCLE_PORT                        273
#define USB_GET_NODE_CONNECTION_INFORMATION_EX    274
#define USB_RESET_HUB                             275
#define USB_GET_HUB_CAPABILITIES_EX               276
#define USB_GET_PORT_CONNECTOR_PROPERTIES         277
#define USB_GET_NODE_CONNECTION_INFORMATION_EX_V2 279

#define HCD_GET_STATS_1                           255
#define HCD_DIAGNOSTIC_MODE_ON                    256
#define HCD_DIAGNOSTIC_MODE_OFF                   257
#define HCD_GET_ROOT_HUB_NAME                     258
#define HCD_GET_DRIVERKEY_NAME                    265
#define HCD_GET_STATS_2                           266
#define HCD_DISABLE_PORT                          268
#define HCD_ENABLE_PORT                           269
#define HCD_USER_REQUEST                          270

#define FILE_DEVICE_USB FILE_DEVICE_UNKNOWN

DEFINE_GUID(GUID_DEVINTERFACE_USB_HUB, 0xf18a0e88, 0xc30c, 0x11d0, 0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8);
DEFINE_GUID(GUID_DEVINTERFACE_USB_DEVICE, 0xa5dcbf10, 0x6530, 0x11d2, 0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed);
DEFINE_GUID(GUID_DEVINTERFACE_USB_HOST_CONTROLLER, 0x3abf6f2d, 0x71c4, 0x462a, 0x8a, 0x92, 0x1e, 0x68, 0x61, 0xe6, 0xaf, 0x27);

#define GUID_CLASS_USBHUB GUID_DEVINTERFACE_USB_HUB
#define GUID_CLASS_USB_DEVICE GUID_DEVINTERFACE_USB_DEVICE
#define GUID_CLASS_USB_HOST_CONTROLLER GUID_DEVINTERFACE_USB_HOST_CONTROLLER

#endif /* __DDK_USBIODEF_H__ */
