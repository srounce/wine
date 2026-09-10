/*
 * WinUSB function driver
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
#include <stdlib.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "ddk/usb.h"
#include "ddk/usbioctl.h"
#include "wine/debug.h"
#include "wine/winusb.h"

WINE_DEFAULT_DEBUG_CHANNEL(winusb);

#define MAX_PIPES 32

struct usb_pipe
{
    UCHAR address;
    USBD_PIPE_TYPE type;
    USBD_PIPE_HANDLE handle;
};

struct device
{
    DEVICE_OBJECT *device_obj, *bus_pdo, *upper;
    UNICODE_STRING link_name;
    BOOL removed;

    /* Interface driven by this device, for interface PDOs of composite
     * devices; 0xff if the device ID carries no MI_ suffix. */
    UCHAR interface_number;

    /* Device descriptor followed by the raw active configuration
     * descriptor. */
    void *descriptors;
    ULONG descriptors_len;

    struct usb_pipe pipes[MAX_PIPES];
    ULONG pipe_count;
};

static NTSTATUS send_urb(struct device *device, URB *urb)
{
    IO_STATUS_BLOCK io;
    NTSTATUS status;
    KEVENT event;
    IRP *irp;

    KeInitializeEvent(&event, NotificationEvent, FALSE);
    if (!(irp = IoBuildDeviceIoControlRequest(IOCTL_INTERNAL_USB_SUBMIT_URB, device->upper,
            NULL, 0, NULL, 0, TRUE, &event, &io)))
        return STATUS_NO_MEMORY;
    IoGetNextIrpStackLocation(irp)->Parameters.Others.Argument1 = urb;

    status = IoCallDriver(device->upper, irp);
    if (status == STATUS_PENDING)
    {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        status = io.Status;
    }
    if (!status && !USBD_SUCCESS(urb->UrbHeader.Status))
        status = STATUS_UNSUCCESSFUL;
    return status;
}

static NTSTATUS get_descriptor(struct device *device, UCHAR type, UCHAR index,
        void *buffer, ULONG size, ULONG *actual)
{
    URB urb = {0};
    NTSTATUS status;

    urb.UrbHeader.Length = sizeof(urb.UrbControlDescriptorRequest);
    urb.UrbHeader.Function = URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE;
    urb.UrbControlDescriptorRequest.TransferBuffer = buffer;
    urb.UrbControlDescriptorRequest.TransferBufferLength = size;
    urb.UrbControlDescriptorRequest.DescriptorType = type;
    urb.UrbControlDescriptorRequest.Index = index;

    if (!(status = send_urb(device, &urb)) && actual)
        *actual = urb.UrbControlDescriptorRequest.TransferBufferLength;
    return status;
}

static const USB_INTERFACE_DESCRIPTOR *find_interface_descriptor(const UCHAR *config,
        ULONG len, UCHAR interface_number)
{
    ULONG offset = 0;

    while (offset + sizeof(USB_COMMON_DESCRIPTOR) <= len)
    {
        const USB_COMMON_DESCRIPTOR *desc = (const USB_COMMON_DESCRIPTOR *)(config + offset);

        if (!desc->bLength)
            break;
        if (desc->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE
                && offset + sizeof(USB_INTERFACE_DESCRIPTOR) <= len)
        {
            const USB_INTERFACE_DESCRIPTOR *iface = (const USB_INTERFACE_DESCRIPTOR *)desc;
            if (!iface->bAlternateSetting
                    && (interface_number == 0xff || iface->bInterfaceNumber == interface_number))
                return iface;
        }
        offset += desc->bLength;
    }
    return NULL;
}

static NTSTATUS select_configuration(struct device *device)
{
    USB_CONFIGURATION_DESCRIPTOR *config;
    const USB_INTERFACE_DESCRIPTOR *iface;
    const UCHAR *config_end, *offset;
    USBD_INTERFACE_INFORMATION *info;
    ULONG i, urb_len;
    NTSTATUS status;
    URB *urb;

    config = (USB_CONFIGURATION_DESCRIPTOR *)((UCHAR *)device->descriptors
            + sizeof(USB_DEVICE_DESCRIPTOR));

    if (!(iface = find_interface_descriptor((const UCHAR *)config,
            device->descriptors_len - sizeof(USB_DEVICE_DESCRIPTOR), device->interface_number)))
    {
        ERR("Failed to find interface %u.\n", device->interface_number);
        return STATUS_INVALID_PARAMETER;
    }

    urb_len = offsetof(struct _URB_SELECT_CONFIGURATION, Interface)
            + offsetof(USBD_INTERFACE_INFORMATION, Pipes[iface->bNumEndpoints]);
    if (!(urb = ExAllocatePool(NonPagedPool, urb_len)))
        return STATUS_NO_MEMORY;
    memset(urb, 0, urb_len);

    urb->UrbHeader.Length = urb_len;
    urb->UrbHeader.Function = URB_FUNCTION_SELECT_CONFIGURATION;
    urb->UrbSelectConfiguration.ConfigurationDescriptor = config;

    info = &urb->UrbSelectConfiguration.Interface;
    info->Length = offsetof(USBD_INTERFACE_INFORMATION, Pipes[iface->bNumEndpoints]);
    info->InterfaceNumber = iface->bInterfaceNumber;
    info->AlternateSetting = iface->bAlternateSetting;
    info->NumberOfPipes = 0;

    config_end = (const UCHAR *)config + (device->descriptors_len - sizeof(USB_DEVICE_DESCRIPTOR));
    offset = (const UCHAR *)iface + iface->bLength;
    while (offset + sizeof(USB_COMMON_DESCRIPTOR) <= config_end
            && info->NumberOfPipes < iface->bNumEndpoints)
    {
        const USB_COMMON_DESCRIPTOR *desc = (const USB_COMMON_DESCRIPTOR *)offset;

        if (!desc->bLength || desc->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE)
            break;
        if (desc->bDescriptorType == USB_ENDPOINT_DESCRIPTOR_TYPE
                && offset + sizeof(USB_ENDPOINT_DESCRIPTOR) <= config_end)
        {
            const USB_ENDPOINT_DESCRIPTOR *endpoint = (const USB_ENDPOINT_DESCRIPTOR *)desc;
            USBD_PIPE_INFORMATION *pipe = &info->Pipes[info->NumberOfPipes++];

            pipe->MaximumPacketSize = endpoint->wMaxPacketSize;
            pipe->EndpointAddress = endpoint->bEndpointAddress;
            pipe->Interval = endpoint->bInterval;
            pipe->PipeType = endpoint->bmAttributes & USB_ENDPOINT_TYPE_MASK;
        }
        offset += desc->bLength;
    }

    if (!(status = send_urb(device, urb)))
    {
        device->pipe_count = min(info->NumberOfPipes, MAX_PIPES);
        for (i = 0; i < device->pipe_count; ++i)
        {
            device->pipes[i].address = info->Pipes[i].EndpointAddress;
            device->pipes[i].type = info->Pipes[i].PipeType;
            device->pipes[i].handle = info->Pipes[i].PipeHandle;
        }
    }

    ExFreePool(urb);
    return status;
}

static USBD_PIPE_HANDLE get_pipe_handle(struct device *device, UCHAR address)
{
    ULONG i;

    for (i = 0; i < device->pipe_count; ++i)
    {
        if (device->pipes[i].address == address)
            return device->pipes[i].handle;
    }
    return NULL;
}

static NTSTATUS get_interface_number(struct device *device)
{
    WCHAR *id, *p;
    NTSTATUS status;
    ULONG size;
    WCHAR buffer[MAX_PATH];

    device->interface_number = 0xff;

    if ((status = IoGetDeviceProperty(device->bus_pdo, DevicePropertyHardwareID,
            sizeof(buffer), buffer, &size)))
    {
        WARN("Failed to get hardware ID, status %#lx.\n", status);
        return status;
    }

    for (id = buffer; *id; id += wcslen(id) + 1)
    {
        if ((p = wcsstr(id, L"&MI_")))
        {
            device->interface_number = wcstoul(p + 4, NULL, 16);
            break;
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS cache_descriptors(struct device *device)
{
    USB_CONFIGURATION_DESCRIPTOR config;
    UCHAR *buffer;
    ULONG actual;
    NTSTATUS status;

    if ((status = get_descriptor(device, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0,
            &config, sizeof(config), &actual)))
        return status;
    if (actual < sizeof(config))
        return STATUS_DEVICE_DATA_ERROR;

    if (!(buffer = ExAllocatePool(NonPagedPool, sizeof(USB_DEVICE_DESCRIPTOR) + config.wTotalLength)))
        return STATUS_NO_MEMORY;

    if ((status = get_descriptor(device, USB_DEVICE_DESCRIPTOR_TYPE, 0,
            buffer, sizeof(USB_DEVICE_DESCRIPTOR), NULL)))
    {
        ExFreePool(buffer);
        return status;
    }
    if ((status = get_descriptor(device, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0,
            buffer + sizeof(USB_DEVICE_DESCRIPTOR), config.wTotalLength, &actual)))
    {
        ExFreePool(buffer);
        return status;
    }

    device->descriptors = buffer;
    device->descriptors_len = sizeof(USB_DEVICE_DESCRIPTOR) + actual;
    return STATUS_SUCCESS;
}

static const WCHAR default_interface_guid[] = L"{f4b7b3ad-2364-4d92-a2f5-5cbb0c1e91b2}";

static NTSTATUS register_device_interface(struct device *device)
{
    WCHAR buffer[64 + sizeof(KEY_VALUE_PARTIAL_INFORMATION) / sizeof(WCHAR)];
    KEY_VALUE_PARTIAL_INFORMATION *info = (KEY_VALUE_PARTIAL_INFORMATION *)buffer;
    UNICODE_STRING name, guid_str;
    const WCHAR *guids = default_interface_guid;
    NTSTATUS status;
    HANDLE key;
    ULONG size;
    GUID guid;

    if (!(status = IoOpenDeviceRegistryKey(device->bus_pdo, PLUGPLAY_REGKEY_DEVICE,
            KEY_READ | KEY_WRITE, &key)))
    {
        RtlInitUnicodeString(&name, L"DeviceInterfaceGUIDs");
        if (!(status = ZwQueryValueKey(key, &name, KeyValuePartialInformation,
                info, sizeof(buffer) - sizeof(WCHAR), &size))
                && info->DataLength >= sizeof(WCHAR))
        {
            memset((UCHAR *)info->Data + info->DataLength, 0, sizeof(WCHAR));
            guids = (const WCHAR *)info->Data;
        }
        else
        {
            /* Seed the value libusb and other clients look for. */
            WCHAR value[ARRAY_SIZE(default_interface_guid) + 1];

            memcpy(value, default_interface_guid, sizeof(default_interface_guid));
            value[ARRAY_SIZE(default_interface_guid)] = 0;
            RtlWriteRegistryValue(RTL_REGISTRY_HANDLE, (WCHAR *)key,
                    L"DeviceInterfaceGUIDs", REG_MULTI_SZ, value, sizeof(value));
        }
        ZwClose(key);
    }
    else
    {
        WARN("Failed to open device registry key, status %#lx.\n", status);
    }

    RtlInitUnicodeString(&guid_str, guids);
    if ((status = RtlGUIDFromString(&guid_str, &guid)))
    {
        ERR("Invalid interface GUID %s.\n", debugstr_w(guids));
        return status;
    }

    if ((status = IoRegisterDeviceInterface(device->bus_pdo, &guid, NULL, &device->link_name)))
    {
        ERR("Failed to register device interface, status %#lx.\n", status);
        return status;
    }
    IoSetDeviceInterfaceState(&device->link_name, TRUE);
    return STATUS_SUCCESS;
}

static NTSTATUS start_device(struct device *device)
{
    NTSTATUS status;

    get_interface_number(device);

    if ((status = cache_descriptors(device)))
    {
        ERR("Failed to cache descriptors, status %#lx.\n", status);
        return status;
    }

    if ((status = select_configuration(device)))
    {
        ERR("Failed to select configuration, status %#lx.\n", status);
        return status;
    }

    return register_device_interface(device);
}

struct transfer_ctx
{
    IRP *ioctl_irp;
    URB *urb;
};

static NTSTATUS WINAPI transfer_complete(DEVICE_OBJECT *device_obj, IRP *irp, void *context)
{
    struct transfer_ctx *ctx = context;
    IRP *ioctl_irp = ctx->ioctl_irp;
    URB *urb = ctx->urb;
    NTSTATUS status = irp->IoStatus.Status;

    if (!status && !USBD_SUCCESS(urb->UrbHeader.Status))
        status = STATUS_UNSUCCESSFUL;

    if (!status)
    {
        switch (urb->UrbHeader.Function)
        {
            case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
                ioctl_irp->IoStatus.Information = urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
                break;
            case URB_FUNCTION_CONTROL_TRANSFER:
                ioctl_irp->IoStatus.Information = urb->UrbControlTransfer.TransferBufferLength;
                break;
        }
    }

    ExFreePool(urb);
    ExFreePool(ctx);
    IoFreeIrp(irp);

    ioctl_irp->IoStatus.Status = status;
    IoCompleteRequest(ioctl_irp, IO_NO_INCREMENT);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* Submit a URB down the stack asynchronously, completing the ioctl IRP when
 * it finishes. Takes ownership of the URB allocation. */
static NTSTATUS submit_urb_async(struct device *device, IRP *ioctl_irp, URB *urb)
{
    struct transfer_ctx *ctx;
    IO_STACK_LOCATION *stack;
    IRP *irp;

    if (!(ctx = ExAllocatePool(NonPagedPool, sizeof(*ctx))))
    {
        ExFreePool(urb);
        return STATUS_NO_MEMORY;
    }

    if (!(irp = IoAllocateIrp(device->upper->StackSize, FALSE)))
    {
        ExFreePool(ctx);
        ExFreePool(urb);
        return STATUS_NO_MEMORY;
    }

    ctx->ioctl_irp = ioctl_irp;
    ctx->urb = urb;

    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    stack->Parameters.Others.Argument1 = urb;
    IoSetCompletionRoutine(irp, transfer_complete, ctx, TRUE, TRUE, TRUE);

    IoMarkIrpPending(ioctl_irp);
    IoCallDriver(device->upper, irp);
    return STATUS_PENDING;
}

static NTSTATUS WINAPI driver_ioctl(DEVICE_OBJECT *device_obj, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inlen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outlen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    struct device *device = device_obj->DeviceExtension;
    UCHAR *buffer = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    TRACE("device_obj %p, irp %p, code %#lx.\n", device_obj, irp, code);

    if (device->removed)
    {
        irp->IoStatus.Status = STATUS_DELETE_PENDING;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_DELETE_PENDING;
    }

    switch (code)
    {
        case IOCTL_WINE_WINUSB_GET_DESCRIPTORS:
        {
            ULONG copied;

            if (outlen < sizeof(ULONG))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            *(ULONG *)buffer = device->descriptors_len;
            copied = min(outlen - sizeof(ULONG), device->descriptors_len);
            memcpy(buffer + sizeof(ULONG), device->descriptors, copied);
            irp->IoStatus.Information = sizeof(ULONG) + copied;
            status = STATUS_SUCCESS;
            break;
        }

        case IOCTL_WINE_WINUSB_CONTROL_TRANSFER:
        {
            const struct wine_winusb_control_transfer_params *params = (void *)buffer;
            BOOL in;
            ULONG data_len;
            URB *urb;

            if (inlen < sizeof(*params))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            in = params->setup[0] & 0x80;
            data_len = in ? outlen : inlen - sizeof(*params);

            if (!(urb = ExAllocatePool(NonPagedPool, sizeof(*urb))))
            {
                status = STATUS_NO_MEMORY;
                break;
            }
            memset(urb, 0, sizeof(*urb));
            urb->UrbHeader.Length = sizeof(urb->UrbControlTransfer);
            urb->UrbHeader.Function = URB_FUNCTION_CONTROL_TRANSFER;
            memcpy(urb->UrbControlTransfer.SetupPacket, params->setup, sizeof(params->setup));
            urb->UrbControlTransfer.TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN : 0;
            urb->UrbControlTransfer.TransferBufferLength = data_len;
            /* For device-to-host transfers the data lands at the start of the
             * buffered output; for host-to-device it follows the params. */
            urb->UrbControlTransfer.TransferBuffer = in ? buffer : buffer + sizeof(*params);

            status = submit_urb_async(device, irp, urb);
            break;
        }

        case IOCTL_WINE_WINUSB_RESET_PIPE:
        case IOCTL_WINE_WINUSB_ABORT_PIPE:
        {
            const struct wine_winusb_pipe_params *params = (void *)buffer;
            USBD_PIPE_HANDLE pipe;
            URB urb = {0};

            if (inlen < sizeof(*params))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            if (!(pipe = get_pipe_handle(device, params->pipe)))
            {
                status = STATUS_INVALID_PARAMETER;
                break;
            }

            urb.UrbHeader.Length = sizeof(urb.UrbPipeRequest);
            urb.UrbHeader.Function = code == IOCTL_WINE_WINUSB_RESET_PIPE
                    ? URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL : URB_FUNCTION_ABORT_PIPE;
            urb.UrbPipeRequest.PipeHandle = pipe;

            status = send_urb(device, &urb);
            break;
        }

        case IOCTL_WINE_WINUSB_SET_ALT_SETTING:
        {
            const struct wine_winusb_set_alt_setting_params *params = (void *)buffer;
            URB urb = {0};

            if (inlen < sizeof(*params))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }

            urb.UrbHeader.Length = sizeof(urb.UrbSelectInterface);
            urb.UrbHeader.Function = URB_FUNCTION_SELECT_INTERFACE;
            urb.UrbSelectInterface.Interface.Length = offsetof(USBD_INTERFACE_INFORMATION, Pipes[0]);
            urb.UrbSelectInterface.Interface.InterfaceNumber = params->interface_number;
            urb.UrbSelectInterface.Interface.AlternateSetting = params->alt_setting;

            /* FIXME: Pipe handles for the new alternate setting are not
             * refreshed. */
            status = send_urb(device, &urb);
            break;
        }

        default:
        {
            ULONG function = (code >> 2) & 0xfff;

            if ((function & ~0xffu) == WINE_WINUSB_TRANSFER_FUNCTION_BASE)
            {
                UCHAR address = function & 0xff;
                BOOL in = address & 0x80;
                USBD_PIPE_HANDLE pipe;
                URB *urb;

                if (!(pipe = get_pipe_handle(device, address)))
                {
                    status = STATUS_INVALID_PARAMETER;
                    break;
                }

                if (!(urb = ExAllocatePool(NonPagedPool, sizeof(*urb))))
                {
                    status = STATUS_NO_MEMORY;
                    break;
                }
                memset(urb, 0, sizeof(*urb));
                urb->UrbHeader.Length = sizeof(urb->UrbBulkOrInterruptTransfer);
                urb->UrbHeader.Function = URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER;
                urb->UrbBulkOrInterruptTransfer.PipeHandle = pipe;
                urb->UrbBulkOrInterruptTransfer.TransferFlags = in ? USBD_TRANSFER_DIRECTION_IN : 0;
                urb->UrbBulkOrInterruptTransfer.TransferBuffer = buffer;
                urb->UrbBulkOrInterruptTransfer.TransferBufferLength = in ? outlen : inlen;

                status = submit_urb_async(device, irp, urb);
                break;
            }

            FIXME("Unhandled ioctl %#lx (device %#lx, access %#lx, function %#lx, method %#lx).\n",
                    code, code >> 16, (code >> 14) & 3, (code >> 2) & 0xfff, code & 3);
        }
    }

    if (status != STATUS_PENDING)
    {
        irp->IoStatus.Status = status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
    return status;
}

static NTSTATUS WINAPI driver_internal_ioctl(DEVICE_OBJECT *device_obj, IRP *irp)
{
    struct device *device = device_obj->DeviceExtension;

    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(device->upper, irp);
}

static NTSTATUS WINAPI driver_create(DEVICE_OBJECT *device_obj, IRP *irp)
{
    TRACE("device_obj %p, irp %p.\n", device_obj, irp);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI driver_close(DEVICE_OBJECT *device_obj, IRP *irp)
{
    TRACE("device_obj %p, irp %p.\n", device_obj, irp);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI driver_pnp(DEVICE_OBJECT *device_obj, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct device *device = device_obj->DeviceExtension;
    NTSTATUS status;

    TRACE("device_obj %p, irp %p, minor function %#x.\n", device_obj, irp, stack->MinorFunction);

    switch (stack->MinorFunction)
    {
        case IRP_MN_START_DEVICE:
            /* The wineusb PDO starts synchronously. */
            if (!(status = start_device(device)))
                irp->IoStatus.Status = STATUS_SUCCESS;
            else
                irp->IoStatus.Status = status;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            device->removed = TRUE;
            irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_REMOVE_DEVICE:
        {
            DEVICE_OBJECT *upper = device->upper;

            device->removed = TRUE;
            if (device->link_name.Buffer)
            {
                IoSetDeviceInterfaceState(&device->link_name, FALSE);
                RtlFreeUnicodeString(&device->link_name);
            }
            if (device->descriptors)
                ExFreePool(device->descriptors);

            irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(irp);
            status = IoCallDriver(upper, irp);
            IoDetachDevice(upper);
            IoDeleteDevice(device_obj);
            return status;
        }

        default:
            break;
    }

    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(device->upper, irp);
}

static NTSTATUS WINAPI driver_add_device(DRIVER_OBJECT *driver, DEVICE_OBJECT *pdo)
{
    struct device *device;
    DEVICE_OBJECT *fdo;
    NTSTATUS status;

    TRACE("driver %p, pdo %p.\n", driver, pdo);

    if ((status = IoCreateDevice(driver, sizeof(*device), NULL, FILE_DEVICE_UNKNOWN,
            0, FALSE, &fdo)))
    {
        ERR("Failed to create FDO, status %#lx.\n", status);
        return status;
    }

    device = fdo->DeviceExtension;
    device->device_obj = fdo;
    device->bus_pdo = pdo;
    device->upper = IoAttachDeviceToDeviceStack(fdo, pdo);

    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    TRACE("driver %p, path %s.\n", driver, debugstr_w(path->Buffer));

    driver->DriverExtension->AddDevice = driver_add_device;
    driver->MajorFunction[IRP_MJ_CREATE] = driver_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = driver_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = driver_ioctl;
    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = driver_internal_ioctl;
    driver->MajorFunction[IRP_MJ_PNP] = driver_pnp;

    return STATUS_SUCCESS;
}
