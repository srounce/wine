/*
 * Copyright (C) 2022 Mohamad Al-Jaf
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

#include <stdarg.h>
#include <stdlib.h>

#include "windef.h"
#include "winbase.h"
#include "winioctl.h"
#include "winternl.h"
#include "winusb.h"

#include "wine/debug.h"
#include "wine/winusb.h"

WINE_DEFAULT_DEBUG_CHANNEL(winusb);

struct pipe_policy
{
    ULONG transfer_timeout;
    UCHAR raw_io;
    UCHAR auto_clear_stall;
    UCHAR short_packet_terminate;
    UCHAR ignore_short_packets;
    UCHAR allow_partial_reads;
    UCHAR auto_flush;
};

struct winusb_interface
{
    HANDLE handle; /* owned by the application */

    /* Cached device descriptor followed by the raw active configuration
     * descriptor, retrieved from winusb.sys. */
    UCHAR *descriptors;
    ULONG descriptors_len;

    const USB_CONFIGURATION_DESCRIPTOR *config;
    const USB_INTERFACE_DESCRIPTOR *interface;

    struct pipe_policy policies[256];
};

static const USB_COMMON_DESCRIPTOR *next_descriptor(const USB_CONFIGURATION_DESCRIPTOR *config,
        const USB_COMMON_DESCRIPTOR *prev)
{
    const UCHAR *end = (const UCHAR *)config + config->wTotalLength;
    const UCHAR *next;

    if (!prev)
        next = (const UCHAR *)config;
    else
        next = (const UCHAR *)prev + prev->bLength;

    if (next + sizeof(USB_COMMON_DESCRIPTOR) > end)
        return NULL;
    if (!((const USB_COMMON_DESCRIPTOR *)next)->bLength)
        return NULL;
    if (next + ((const USB_COMMON_DESCRIPTOR *)next)->bLength > end)
        return NULL;
    return (const USB_COMMON_DESCRIPTOR *)next;
}

/* Find an interface descriptor, by position in the configuration if
 * "by_position" (used for handle creation, where the WinUSB interface index
 * follows configuration order), otherwise by interface number. */
static const USB_INTERFACE_DESCRIPTOR *find_interface(const USB_CONFIGURATION_DESCRIPTOR *config,
        BOOL by_position, UCHAR value, UCHAR alt_setting)
{
    const USB_COMMON_DESCRIPTOR *desc = NULL;
    UCHAR position = 0;

    while ((desc = next_descriptor(config, desc)))
    {
        const USB_INTERFACE_DESCRIPTOR *iface = (const USB_INTERFACE_DESCRIPTOR *)desc;

        if (desc->bDescriptorType != USB_INTERFACE_DESCRIPTOR_TYPE
                || desc->bLength < sizeof(USB_INTERFACE_DESCRIPTOR))
            continue;

        if (by_position)
        {
            if (iface->bAlternateSetting)
                continue;
            if (position++ == value)
                return iface;
        }
        else
        {
            if (iface->bInterfaceNumber == value && iface->bAlternateSetting == alt_setting)
                return iface;
        }
    }
    return NULL;
}

static const USB_ENDPOINT_DESCRIPTOR *find_endpoint(const USB_CONFIGURATION_DESCRIPTOR *config,
        const USB_INTERFACE_DESCRIPTOR *iface, UCHAR index)
{
    const USB_COMMON_DESCRIPTOR *desc = (const USB_COMMON_DESCRIPTOR *)iface;
    UCHAR position = 0;

    while ((desc = next_descriptor(config, desc)))
    {
        if (desc->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE)
            break;
        if (desc->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE
                && desc->bLength >= 7 /* audio endpoint descriptors are longer */
                && position++ == index)
            return (const USB_ENDPOINT_DESCRIPTOR *)desc;
    }
    return NULL;
}

static BOOL sync_ioctl(HANDLE handle, DWORD code, void *in_buffer, DWORD in_len,
        void *out_buffer, DWORD out_len, ULONG *ret_len, ULONG timeout, UCHAR abort_pipe)
{
    OVERLAPPED ovl = {0};
    DWORD transferred;
    BOOL ret;

    if (!(ovl.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL)))
        return FALSE;

    ret = DeviceIoControl(handle, code, in_buffer, in_len, out_buffer, out_len, NULL, &ovl);
    if (!ret && GetLastError() == ERROR_IO_PENDING)
    {
        if (timeout && WaitForSingleObject(ovl.hEvent, timeout) == WAIT_TIMEOUT)
        {
            struct wine_winusb_pipe_params params = {.pipe = abort_pipe};

            CancelIoEx(handle, &ovl);
            DeviceIoControl(handle, IOCTL_WINE_WINUSB_ABORT_PIPE,
                    &params, sizeof(params), NULL, 0, &transferred, NULL);
            GetOverlappedResult(handle, &ovl, &transferred, TRUE);
            CloseHandle(ovl.hEvent);
            SetLastError(ERROR_SEM_TIMEOUT);
            return FALSE;
        }
        ret = GetOverlappedResult(handle, &ovl, &transferred, TRUE);
    }
    else if (ret)
    {
        GetOverlappedResult(handle, &ovl, &transferred, TRUE);
    }

    if (ret && ret_len)
        *ret_len = transferred;
    CloseHandle(ovl.hEvent);
    return ret;
}

static BOOL create_interface(HANDLE handle, const UCHAR *descriptors, ULONG len,
        UCHAR position, WINUSB_INTERFACE_HANDLE *ret)
{
    struct winusb_interface *object;
    const USB_CONFIGURATION_DESCRIPTOR *config;
    const USB_INTERFACE_DESCRIPTOR *iface;

    if (len < sizeof(USB_DEVICE_DESCRIPTOR) + sizeof(USB_CONFIGURATION_DESCRIPTOR))
    {
        SetLastError(ERROR_BAD_DEVICE);
        return FALSE;
    }

    config = (const USB_CONFIGURATION_DESCRIPTOR *)(descriptors + sizeof(USB_DEVICE_DESCRIPTOR));
    if (sizeof(USB_DEVICE_DESCRIPTOR) + config->wTotalLength > len)
    {
        SetLastError(ERROR_BAD_DEVICE);
        return FALSE;
    }

    if (!(iface = find_interface(config, TRUE, position, 0)))
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }

    if (!(object = calloc(1, sizeof(*object))) || !(object->descriptors = malloc(len)))
    {
        free(object);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    memcpy(object->descriptors, descriptors, len);
    object->descriptors_len = len;
    object->handle = handle;
    object->config = (const USB_CONFIGURATION_DESCRIPTOR *)(object->descriptors
            + sizeof(USB_DEVICE_DESCRIPTOR));
    object->interface = (const USB_INTERFACE_DESCRIPTOR *)(object->descriptors
            + ((const UCHAR *)iface - descriptors));

    *ret = object;
    return TRUE;
}

/***********************************************************************
 *           WinUsb_Initialize (winusb.@)
 */
BOOL WINAPI WinUsb_Initialize(HANDLE handle, PWINUSB_INTERFACE_HANDLE ret)
{
    UCHAR stack_buffer[1024], *buffer = stack_buffer;
    ULONG total, transferred;
    BOOL success;

    TRACE("handle %p, ret %p.\n", handle, ret);

    if (handle == INVALID_HANDLE_VALUE || !ret)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!sync_ioctl(handle, IOCTL_WINE_WINUSB_GET_DESCRIPTORS, NULL, 0,
            buffer, sizeof(stack_buffer), &transferred, 0, 0))
        return FALSE;

    total = *(ULONG *)buffer;
    if (sizeof(ULONG) + total > transferred)
    {
        if (!(buffer = malloc(sizeof(ULONG) + total)))
        {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return FALSE;
        }
        if (!sync_ioctl(handle, IOCTL_WINE_WINUSB_GET_DESCRIPTORS, NULL, 0,
                buffer, sizeof(ULONG) + total, &transferred, 0, 0))
        {
            free(buffer);
            return FALSE;
        }
    }

    success = create_interface(handle, buffer + sizeof(ULONG),
            transferred - sizeof(ULONG), 0, ret);
    if (buffer != stack_buffer)
        free(buffer);
    return success;
}

/***********************************************************************
 *           WinUsb_Free (winusb.@)
 */
BOOL WINAPI WinUsb_Free(WINUSB_INTERFACE_HANDLE handle)
{
    struct winusb_interface *object = handle;

    TRACE("handle %p.\n", handle);

    if (!object)
        return TRUE;
    free(object->descriptors);
    free(object);
    return TRUE;
}

/***********************************************************************
 *           WinUsb_GetAssociatedInterface (winusb.@)
 */
BOOL WINAPI WinUsb_GetAssociatedInterface(WINUSB_INTERFACE_HANDLE handle,
        UCHAR index, PWINUSB_INTERFACE_HANDLE ret)
{
    struct winusb_interface *object = handle;
    const USB_COMMON_DESCRIPTOR *desc;
    UCHAR position = 0;

    TRACE("handle %p, index %u, ret %p.\n", handle, index, ret);

    if (!object || !ret)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* Find the position of our own interface, then create a handle for the
     * one index + 1 places later. */
    desc = NULL;
    while ((desc = next_descriptor(object->config, desc)))
    {
        const USB_INTERFACE_DESCRIPTOR *iface = (const USB_INTERFACE_DESCRIPTOR *)desc;

        if (desc->bDescriptorType != USB_INTERFACE_DESCRIPTOR_TYPE || iface->bAlternateSetting)
            continue;
        if (iface->bInterfaceNumber == object->interface->bInterfaceNumber)
            break;
        ++position;
    }

    return create_interface(object->handle, object->descriptors, object->descriptors_len,
            position + index + 1, ret);
}

/***********************************************************************
 *           WinUsb_GetDescriptor (winusb.@)
 */
BOOL WINAPI WinUsb_GetDescriptor(WINUSB_INTERFACE_HANDLE handle, UCHAR type, UCHAR index,
        USHORT language, UCHAR *buffer, ULONG size, ULONG *transferred)
{
    struct winusb_interface *object = handle;
    struct wine_winusb_control_transfer_params params;

    TRACE("handle %p, type %#x, index %u, language %u, buffer %p, size %lu, transferred %p.\n",
            handle, type, index, language, buffer, size, transferred);

    if (!object)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    params.setup[0] = 0x80;
    params.setup[1] = 0x06; /* GET_DESCRIPTOR */
    params.setup[2] = index;
    params.setup[3] = type;
    params.setup[4] = language;
    params.setup[5] = language >> 8;
    params.setup[6] = size;
    params.setup[7] = size >> 8;

    return sync_ioctl(object->handle, IOCTL_WINE_WINUSB_CONTROL_TRANSFER,
            &params, sizeof(params), buffer, size, transferred, 0, 0);
}

/***********************************************************************
 *           WinUsb_QueryInterfaceSettings (winusb.@)
 */
BOOL WINAPI WinUsb_QueryInterfaceSettings(WINUSB_INTERFACE_HANDLE handle,
        UCHAR alt_setting, PUSB_INTERFACE_DESCRIPTOR ret)
{
    struct winusb_interface *object = handle;
    const USB_INTERFACE_DESCRIPTOR *iface;

    TRACE("handle %p, alt_setting %u, ret %p.\n", handle, alt_setting, ret);

    if (!object || !ret)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!(iface = find_interface(object->config, FALSE,
            object->interface->bInterfaceNumber, alt_setting)))
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }

    memcpy(ret, iface, sizeof(*ret));
    return TRUE;
}

/***********************************************************************
 *           WinUsb_QueryDeviceInformation (winusb.@)
 */
BOOL WINAPI WinUsb_QueryDeviceInformation(WINUSB_INTERFACE_HANDLE handle,
        ULONG type, ULONG *size, void *buffer)
{
    struct winusb_interface *object = handle;

    TRACE("handle %p, type %#lx, size %p, buffer %p.\n", handle, type, size, buffer);

    if (!object || !size || !buffer)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (type == DEVICE_SPEED)
    {
        if (*size < sizeof(UCHAR))
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return FALSE;
        }
        /* FIXME: The actual speed is not exposed by the driver yet. */
        *(UCHAR *)buffer = HighSpeed;
        *size = sizeof(UCHAR);
        return TRUE;
    }

    FIXME("Unhandled information type %#lx.\n", type);
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/***********************************************************************
 *           WinUsb_SetCurrentAlternateSetting (winusb.@)
 */
BOOL WINAPI WinUsb_SetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE handle, UCHAR alt_setting)
{
    struct winusb_interface *object = handle;
    struct wine_winusb_set_alt_setting_params params;

    TRACE("handle %p, alt_setting %u.\n", handle, alt_setting);

    if (!object)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    params.interface_number = object->interface->bInterfaceNumber;
    params.alt_setting = alt_setting;
    return sync_ioctl(object->handle, IOCTL_WINE_WINUSB_SET_ALT_SETTING,
            &params, sizeof(params), NULL, 0, NULL, 0, 0);
}

/***********************************************************************
 *           WinUsb_GetCurrentAlternateSetting (winusb.@)
 */
BOOL WINAPI WinUsb_GetCurrentAlternateSetting(WINUSB_INTERFACE_HANDLE handle, UCHAR *alt_setting)
{
    struct winusb_interface *object = handle;
    struct wine_winusb_control_transfer_params params;

    TRACE("handle %p, alt_setting %p.\n", handle, alt_setting);

    if (!object || !alt_setting)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    params.setup[0] = 0x81; /* IN | standard | interface */
    params.setup[1] = 0x0a; /* GET_INTERFACE */
    params.setup[2] = 0;
    params.setup[3] = 0;
    params.setup[4] = object->interface->bInterfaceNumber;
    params.setup[5] = 0;
    params.setup[6] = 1;
    params.setup[7] = 0;

    return sync_ioctl(object->handle, IOCTL_WINE_WINUSB_CONTROL_TRANSFER,
            &params, sizeof(params), alt_setting, 1, NULL, 0, 0);
}

/***********************************************************************
 *           WinUsb_QueryPipe (winusb.@)
 */
BOOL WINAPI WinUsb_QueryPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR alt_setting,
        UCHAR index, PWINUSB_PIPE_INFORMATION ret)
{
    struct winusb_interface *object = handle;
    const USB_INTERFACE_DESCRIPTOR *iface;
    const USB_ENDPOINT_DESCRIPTOR *endpoint;

    TRACE("handle %p, alt_setting %u, index %u, ret %p.\n", handle, alt_setting, index, ret);

    if (!object || !ret)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!(iface = find_interface(object->config, FALSE,
            object->interface->bInterfaceNumber, alt_setting))
            || !(endpoint = find_endpoint(object->config, iface, index)))
    {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }

    ret->PipeType = endpoint->bmAttributes & USB_ENDPOINT_TYPE_MASK;
    ret->PipeId = endpoint->bEndpointAddress;
    ret->MaximumPacketSize = endpoint->wMaxPacketSize;
    ret->Interval = endpoint->bInterval;
    return TRUE;
}

/***********************************************************************
 *           WinUsb_QueryPipeEx (winusb.@)
 */
BOOL WINAPI WinUsb_QueryPipeEx(WINUSB_INTERFACE_HANDLE handle, UCHAR alt_setting,
        UCHAR index, PWINUSB_PIPE_INFORMATION_EX ret)
{
    WINUSB_PIPE_INFORMATION info;

    TRACE("handle %p, alt_setting %u, index %u, ret %p.\n", handle, alt_setting, index, ret);

    if (!ret)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (!WinUsb_QueryPipe(handle, alt_setting, index, &info))
        return FALSE;

    ret->PipeType = info.PipeType;
    ret->PipeId = info.PipeId;
    ret->MaximumPacketSize = info.MaximumPacketSize;
    ret->Interval = info.Interval;
    ret->MaximumBytesPerInterval = info.MaximumPacketSize;
    return TRUE;
}

/***********************************************************************
 *           WinUsb_SetPipePolicy (winusb.@)
 */
BOOL WINAPI WinUsb_SetPipePolicy(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe,
        ULONG type, ULONG size, void *value)
{
    struct winusb_interface *object = handle;
    struct pipe_policy *policy;

    TRACE("handle %p, pipe %#x, type %#lx, size %lu, value %p.\n", handle, pipe, type, size, value);

    if (!object || !value || !size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    policy = &object->policies[pipe];

    switch (type)
    {
        case PIPE_TRANSFER_TIMEOUT:
            if (size < sizeof(ULONG))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            policy->transfer_timeout = *(ULONG *)value;
            return TRUE;

        case AUTO_CLEAR_STALL:
            policy->auto_clear_stall = !!*(UCHAR *)value;
            return TRUE;

        case RAW_IO:
            policy->raw_io = !!*(UCHAR *)value;
            return TRUE;

        case SHORT_PACKET_TERMINATE:
            policy->short_packet_terminate = !!*(UCHAR *)value;
            return TRUE;

        case IGNORE_SHORT_PACKETS:
            policy->ignore_short_packets = !!*(UCHAR *)value;
            return TRUE;

        case ALLOW_PARTIAL_READS:
            policy->allow_partial_reads = !!*(UCHAR *)value;
            return TRUE;

        case AUTO_FLUSH:
            policy->auto_flush = !!*(UCHAR *)value;
            return TRUE;

        default:
            FIXME("Unhandled policy type %#lx.\n", type);
            SetLastError(ERROR_NOT_SUPPORTED);
            return FALSE;
    }
}

/***********************************************************************
 *           WinUsb_GetPipePolicy (winusb.@)
 */
BOOL WINAPI WinUsb_GetPipePolicy(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe,
        ULONG type, ULONG *size, void *value)
{
    struct winusb_interface *object = handle;
    struct pipe_policy *policy;

    TRACE("handle %p, pipe %#x, type %#lx, size %p, value %p.\n", handle, pipe, type, size, value);

    if (!object || !size || !value)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    policy = &object->policies[pipe];

    switch (type)
    {
        case PIPE_TRANSFER_TIMEOUT:
        case MAXIMUM_TRANSFER_SIZE:
            if (*size < sizeof(ULONG))
            {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return FALSE;
            }
            if (type == PIPE_TRANSFER_TIMEOUT)
                *(ULONG *)value = policy->transfer_timeout;
            else
                *(ULONG *)value = 0x100000;
            *size = sizeof(ULONG);
            return TRUE;

        case AUTO_CLEAR_STALL:
        case RAW_IO:
        case SHORT_PACKET_TERMINATE:
        case IGNORE_SHORT_PACKETS:
        case ALLOW_PARTIAL_READS:
        case AUTO_FLUSH:
            if (*size < sizeof(UCHAR))
            {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                return FALSE;
            }
            switch (type)
            {
                case AUTO_CLEAR_STALL: *(UCHAR *)value = policy->auto_clear_stall; break;
                case RAW_IO: *(UCHAR *)value = policy->raw_io; break;
                case SHORT_PACKET_TERMINATE: *(UCHAR *)value = policy->short_packet_terminate; break;
                case IGNORE_SHORT_PACKETS: *(UCHAR *)value = policy->ignore_short_packets; break;
                case ALLOW_PARTIAL_READS: *(UCHAR *)value = policy->allow_partial_reads; break;
                case AUTO_FLUSH: *(UCHAR *)value = policy->auto_flush; break;
            }
            *size = sizeof(UCHAR);
            return TRUE;

        default:
            FIXME("Unhandled policy type %#lx.\n", type);
            SetLastError(ERROR_NOT_SUPPORTED);
            return FALSE;
    }
}

/***********************************************************************
 *           WinUsb_SetPowerPolicy (winusb.@)
 */
BOOL WINAPI WinUsb_SetPowerPolicy(WINUSB_INTERFACE_HANDLE handle, ULONG type,
        ULONG size, void *value)
{
    TRACE("handle %p, type %#lx, size %lu, value %p.\n", handle, type, size, value);
    return TRUE;
}

/***********************************************************************
 *           WinUsb_GetPowerPolicy (winusb.@)
 */
BOOL WINAPI WinUsb_GetPowerPolicy(WINUSB_INTERFACE_HANDLE handle, ULONG type,
        ULONG *size, void *value)
{
    TRACE("handle %p, type %#lx, size %p, value %p.\n", handle, type, size, value);

    if (!size || !value || !*size)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    memset(value, 0, *size);
    return TRUE;
}

static BOOL transfer_pipe(struct winusb_interface *object, UCHAR pipe, void *in_buffer,
        ULONG in_len, void *out_buffer, ULONG out_len, ULONG *transferred, OVERLAPPED *overlapped)
{
    if (overlapped)
    {
        BOOL ret = DeviceIoControl(object->handle, IOCTL_WINE_WINUSB_TRANSFER(pipe),
                in_buffer, in_len, out_buffer, out_len, NULL, overlapped);
        /* Some callers pass a length pointer even for asynchronous
         * transfers; it must not be filled until completion. */
        return ret;
    }

    return sync_ioctl(object->handle, IOCTL_WINE_WINUSB_TRANSFER(pipe), in_buffer,
            in_len, out_buffer, out_len, transferred,
            object->policies[pipe].transfer_timeout, pipe);
}

/***********************************************************************
 *           WinUsb_ReadPipe (winusb.@)
 */
BOOL WINAPI WinUsb_ReadPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe, UCHAR *buffer,
        ULONG size, ULONG *transferred, OVERLAPPED *overlapped)
{
    struct winusb_interface *object = handle;

    TRACE("handle %p, pipe %#x, buffer %p, size %lu, transferred %p, overlapped %p.\n",
            handle, pipe, buffer, size, transferred, overlapped);

    if (!object || !(pipe & 0x80))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return transfer_pipe(object, pipe, NULL, 0, buffer, size, transferred, overlapped);
}

/***********************************************************************
 *           WinUsb_WritePipe (winusb.@)
 */
BOOL WINAPI WinUsb_WritePipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe, UCHAR *buffer,
        ULONG size, ULONG *transferred, OVERLAPPED *overlapped)
{
    struct winusb_interface *object = handle;

    TRACE("handle %p, pipe %#x, buffer %p, size %lu, transferred %p, overlapped %p.\n",
            handle, pipe, buffer, size, transferred, overlapped);

    if (!object || (pipe & 0x80))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return transfer_pipe(object, pipe, buffer, size, NULL, 0, transferred, overlapped);
}

/***********************************************************************
 *           WinUsb_ControlTransfer (winusb.@)
 */
BOOL WINAPI WinUsb_ControlTransfer(WINUSB_INTERFACE_HANDLE handle, WINUSB_SETUP_PACKET setup,
        UCHAR *buffer, ULONG size, ULONG *transferred, OVERLAPPED *overlapped)
{
    struct winusb_interface *object = handle;
    struct wine_winusb_control_transfer_params *params;
    UCHAR stack_buffer[sizeof(*params) + 64];
    ULONG in_len = sizeof(*params);
    BOOL in, ret;

    TRACE("handle %p, setup {%#x, %#x, %#x, %#x, %#x}, buffer %p, size %lu, transferred %p, overlapped %p.\n",
            handle, setup.RequestType, setup.Request, setup.Value, setup.Index, setup.Length,
            buffer, size, transferred, overlapped);

    if (!object)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    in = setup.RequestType & 0x80;
    if (!in)
        in_len += size;

    if (in_len <= sizeof(stack_buffer))
        params = (struct wine_winusb_control_transfer_params *)stack_buffer;
    else if (!(params = malloc(in_len)))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    params->setup[0] = setup.RequestType;
    params->setup[1] = setup.Request;
    params->setup[2] = setup.Value;
    params->setup[3] = setup.Value >> 8;
    params->setup[4] = setup.Index;
    params->setup[5] = setup.Index >> 8;
    params->setup[6] = setup.Length;
    params->setup[7] = setup.Length >> 8;
    if (!in)
        memcpy(params + 1, buffer, size);

    /* Buffered I/O captures the input buffer when the request is issued, so
     * this is safe for asynchronous requests too. */
    if (overlapped)
        ret = DeviceIoControl(object->handle, IOCTL_WINE_WINUSB_CONTROL_TRANSFER,
                params, in_len, in ? buffer : NULL, in ? size : 0, NULL, overlapped);
    else
        ret = sync_ioctl(object->handle, IOCTL_WINE_WINUSB_CONTROL_TRANSFER,
                params, in_len, in ? buffer : NULL, in ? size : 0, transferred, 0, 0);

    if ((UCHAR *)params != stack_buffer)
        free(params);
    return ret;
}

/***********************************************************************
 *           WinUsb_ResetPipe (winusb.@)
 */
BOOL WINAPI WinUsb_ResetPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe)
{
    struct winusb_interface *object = handle;
    struct wine_winusb_pipe_params params = {.pipe = pipe};

    TRACE("handle %p, pipe %#x.\n", handle, pipe);

    if (!object)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return sync_ioctl(object->handle, IOCTL_WINE_WINUSB_RESET_PIPE,
            &params, sizeof(params), NULL, 0, NULL, 0, 0);
}

/***********************************************************************
 *           WinUsb_AbortPipe (winusb.@)
 */
BOOL WINAPI WinUsb_AbortPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe)
{
    struct winusb_interface *object = handle;
    struct wine_winusb_pipe_params params = {.pipe = pipe};

    TRACE("handle %p, pipe %#x.\n", handle, pipe);

    if (!object)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return sync_ioctl(object->handle, IOCTL_WINE_WINUSB_ABORT_PIPE,
            &params, sizeof(params), NULL, 0, NULL, 0, 0);
}

/***********************************************************************
 *           WinUsb_FlushPipe (winusb.@)
 */
BOOL WINAPI WinUsb_FlushPipe(WINUSB_INTERFACE_HANDLE handle, UCHAR pipe)
{
    TRACE("handle %p, pipe %#x.\n", handle, pipe);
    return TRUE;
}

/***********************************************************************
 *           WinUsb_GetOverlappedResult (winusb.@)
 */
BOOL WINAPI WinUsb_GetOverlappedResult(WINUSB_INTERFACE_HANDLE handle,
        OVERLAPPED *overlapped, DWORD *transferred, BOOL wait)
{
    struct winusb_interface *object = handle;

    TRACE("handle %p, overlapped %p, transferred %p, wait %d.\n",
            handle, overlapped, transferred, wait);

    if (!object)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    return GetOverlappedResult(object->handle, overlapped, transferred, wait);
}

/***********************************************************************
 *           WinUsb_ParseDescriptors (winusb.@)
 */
PUSB_COMMON_DESCRIPTOR WINAPI WinUsb_ParseDescriptors(void *buffer, ULONG size,
        void *start, LONG type)
{
    UCHAR *offset = start, *end = (UCHAR *)buffer + size;

    TRACE("buffer %p, size %lu, start %p, type %#lx.\n", buffer, size, start, type);

    while (offset + sizeof(USB_COMMON_DESCRIPTOR) <= end)
    {
        USB_COMMON_DESCRIPTOR *desc = (USB_COMMON_DESCRIPTOR *)offset;

        if (!desc->bLength || offset + desc->bLength > end)
            break;
        if (desc->bDescriptorType == type)
            return desc;
        offset += desc->bLength;
    }

    SetLastError(ERROR_NO_MORE_ITEMS);
    return NULL;
}

/***********************************************************************
 *           WinUsb_ParseConfigurationDescriptor (winusb.@)
 */
PUSB_INTERFACE_DESCRIPTOR WINAPI WinUsb_ParseConfigurationDescriptor(
        PUSB_CONFIGURATION_DESCRIPTOR config, void *start, LONG interface_number,
        LONG alt_setting, LONG interface_class, LONG interface_subclass, LONG interface_protocol)
{
    UCHAR *offset = start, *end = (UCHAR *)config + config->wTotalLength;

    TRACE("config %p, start %p, interface %ld, alt_setting %ld, class %ld, subclass %ld, protocol %ld.\n",
            config, start, interface_number, alt_setting, interface_class, interface_subclass,
            interface_protocol);

    while (offset + sizeof(USB_COMMON_DESCRIPTOR) <= end)
    {
        USB_INTERFACE_DESCRIPTOR *desc = (USB_INTERFACE_DESCRIPTOR *)offset;

        if (!desc->bLength || offset + desc->bLength > end)
            break;
        if (desc->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE
                && desc->bLength >= sizeof(USB_INTERFACE_DESCRIPTOR)
                && (interface_number == -1 || desc->bInterfaceNumber == interface_number)
                && (alt_setting == -1 || desc->bAlternateSetting == alt_setting)
                && (interface_class == -1 || desc->bInterfaceClass == interface_class)
                && (interface_subclass == -1 || desc->bInterfaceSubClass == interface_subclass)
                && (interface_protocol == -1 || desc->bInterfaceProtocol == interface_protocol))
            return desc;
        offset += desc->bLength;
    }

    SetLastError(ERROR_NO_MORE_ITEMS);
    return NULL;
}
