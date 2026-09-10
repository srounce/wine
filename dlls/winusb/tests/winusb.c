/*
 * WinUSB API tests
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

#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "winioctl.h"
#include "ddk/usbioctl.h"
#include "winusb.h"

#include "wine/test.h"

/* A two-interface configuration: interface 0 with one interrupt endpoint and
 * a class-specific descriptor, interface 1 with two bulk endpoints. */
static const UCHAR test_config[] =
{
    /* configuration */
    9, USB_CONFIGURATION_DESCRIPTOR_TYPE, 53, 0, 2, 1, 0, 0x80, 50,
    /* interface 0, alt 0 */
    9, USB_INTERFACE_DESCRIPTOR_TYPE, 0, 0, 1, 3, 1, 2, 0,
    /* class-specific */
    5, 0x24, 1, 2, 3,
    /* endpoint 0x81 */
    7, USB_ENDPOINT_DESCRIPTOR_TYPE, 0x81, 3, 0x40, 0, 10,
    /* interface 1, alt 0 */
    9, USB_INTERFACE_DESCRIPTOR_TYPE, 1, 0, 2, 0xff, 0xff, 0xff, 0,
    /* endpoint 0x82 */
    7, USB_ENDPOINT_DESCRIPTOR_TYPE, 0x82, 2, 0, 2, 0,
    /* endpoint 0x02 */
    7, USB_ENDPOINT_DESCRIPTOR_TYPE, 0x02, 2, 0, 2, 0,
};

static void test_parse_descriptors(void)
{
    UCHAR buffer[sizeof(test_config)];
    PUSB_COMMON_DESCRIPTOR desc;

    memcpy(buffer, test_config, sizeof(buffer));

    desc = WinUsb_ParseDescriptors(buffer, sizeof(buffer), buffer, USB_INTERFACE_DESCRIPTOR_TYPE);
    ok(desc == (void *)(buffer + 9), "got descriptor %p, expected %p\n", desc, buffer + 9);

    desc = WinUsb_ParseDescriptors(buffer, sizeof(buffer), buffer, USB_ENDPOINT_DESCRIPTOR_TYPE);
    ok(desc == (void *)(buffer + 23), "got descriptor %p, expected %p\n", desc, buffer + 23);

    /* start position past the first match finds the next one */
    desc = WinUsb_ParseDescriptors(buffer, sizeof(buffer), buffer + 18, USB_INTERFACE_DESCRIPTOR_TYPE);
    ok(desc == (void *)(buffer + 30), "got descriptor %p, expected %p\n", desc, buffer + 30);

    SetLastError(0xdeadbeef);
    desc = WinUsb_ParseDescriptors(buffer, sizeof(buffer), buffer, 0x42);
    ok(!desc, "got descriptor %p\n", desc);
    ok(GetLastError() == ERROR_NO_MORE_ITEMS, "got error %lu\n", GetLastError());

    /* class-specific descriptors are matched too */
    desc = WinUsb_ParseDescriptors(buffer, sizeof(buffer), buffer, 0x24);
    ok(desc == (void *)(buffer + 18), "got descriptor %p, expected %p\n", desc, buffer + 18);
}

static void test_parse_configuration_descriptor(void)
{
    UCHAR buffer[sizeof(test_config)];
    PUSB_CONFIGURATION_DESCRIPTOR config = (void *)buffer;
    PUSB_INTERFACE_DESCRIPTOR iface;

    memcpy(buffer, test_config, sizeof(buffer));

    iface = WinUsb_ParseConfigurationDescriptor(config, buffer, -1, -1, -1, -1, -1);
    ok(iface == (void *)(buffer + 9), "got descriptor %p, expected %p\n", iface, buffer + 9);

    iface = WinUsb_ParseConfigurationDescriptor(config, buffer, 1, -1, -1, -1, -1);
    ok(iface == (void *)(buffer + 30), "got descriptor %p, expected %p\n", iface, buffer + 30);

    iface = WinUsb_ParseConfigurationDescriptor(config, buffer, -1, -1, 0xff, -1, -1);
    ok(iface == (void *)(buffer + 30), "got descriptor %p, expected %p\n", iface, buffer + 30);

    iface = WinUsb_ParseConfigurationDescriptor(config, buffer, -1, -1, 3, -1, 2);
    ok(iface == (void *)(buffer + 9), "got descriptor %p, expected %p\n", iface, buffer + 9);

    SetLastError(0xdeadbeef);
    iface = WinUsb_ParseConfigurationDescriptor(config, buffer, 2, -1, -1, -1, -1);
    ok(!iface, "got descriptor %p\n", iface);
    ok(GetLastError() == ERROR_NO_MORE_ITEMS, "got error %lu\n", GetLastError());

    /* start position is respected */
    iface = WinUsb_ParseConfigurationDescriptor(config, buffer + 18, -1, -1, -1, -1, -1);
    ok(iface == (void *)(buffer + 30), "got descriptor %p, expected %p\n", iface, buffer + 30);
}

static void test_invalid_handles(void)
{
    WINUSB_INTERFACE_HANDLE iface = NULL;
    UCHAR value;
    ULONG size;
    BOOL ret;

    SetLastError(0xdeadbeef);
    ret = WinUsb_Initialize(INVALID_HANDLE_VALUE, &iface);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    ret = WinUsb_Free(NULL);
    ok(ret, "expected success\n");

    SetLastError(0xdeadbeef);
    ret = WinUsb_GetAssociatedInterface(NULL, 0, &iface);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());

    SetLastError(0xdeadbeef);
    size = sizeof(value);
    ret = WinUsb_QueryDeviceInformation(NULL, DEVICE_SPEED, &size, &value);
    ok(!ret, "expected failure\n");
    ok(GetLastError() == ERROR_INVALID_PARAMETER, "got error %lu\n", GetLastError());
}

START_TEST(winusb)
{
    test_parse_descriptors();
    test_parse_configuration_descriptor();
    test_invalid_handles();
}
