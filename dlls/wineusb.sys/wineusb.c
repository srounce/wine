/*
 * USB root device enumerator using libusb
 *
 * Copyright 2020 Zebediah Figura
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

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "ntstatus.h"
#include "windef.h"
#include "winioctl.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "initguid.h"
#include "ddk/usb.h"
#include "ddk/usbioctl.h"
#include "wine/asm.h"
#include "wine/debug.h"
#include "wine/list.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(wineusb);

#ifdef __ASM_USE_FASTCALL_WRAPPER

extern void * WINAPI wrap_fastcall_func1( void *func, const void *a );
__ASM_STDCALL_FUNC( wrap_fastcall_func1, 8,
                   "popl %ecx\n\t"
                   "popl %eax\n\t"
                   "xchgl (%esp),%ecx\n\t"
                   "jmp *%eax" );

#define call_fastcall_func1(func,a) wrap_fastcall_func1(func,a)

#else

#define call_fastcall_func1(func,a) func(a)

#endif

#define DECLARE_CRITICAL_SECTION(cs) \
    static CRITICAL_SECTION cs; \
    static CRITICAL_SECTION_DEBUG cs##_debug = \
    { 0, 0, &cs, { &cs##_debug.ProcessLocksList, &cs##_debug.ProcessLocksList }, \
      0, 0, { (DWORD_PTR)(__FILE__ ": " # cs) }}; \
    static CRITICAL_SECTION cs = { &cs##_debug, -1, 0, 0, 0, 0 };

DECLARE_CRITICAL_SECTION(wineusb_cs);

enum device_kind
{
    DEVICE_KIND_FDO,
    DEVICE_KIND_CONTROLLER,
    DEVICE_KIND_HUB,
    DEVICE_KIND_DEVICE,
};

/* Common header of every device extension created by this driver. */
struct usb_object
{
    enum device_kind kind;
    DEVICE_OBJECT *device_obj;
    UNICODE_STRING link_name;
};

static const GUID *device_interface_guid(enum device_kind kind)
{
    switch (kind)
    {
        case DEVICE_KIND_CONTROLLER:
            return &GUID_DEVINTERFACE_USB_HOST_CONTROLLER;
        case DEVICE_KIND_HUB:
            return &GUID_DEVINTERFACE_USB_HUB;
        default:
            return &GUID_DEVINTERFACE_USB_DEVICE;
    }
}

static void register_device_interface(struct usb_object *obj)
{
    NTSTATUS status;

    if ((status = IoRegisterDeviceInterface(obj->device_obj, device_interface_guid(obj->kind),
            NULL, &obj->link_name)))
    {
        ERR("Failed to register interface, status %#lx.\n", status);
        return;
    }
    IoSetDeviceInterfaceState(&obj->link_name, TRUE);
}

static void unregister_device_interface(struct usb_object *obj)
{
    if (obj->link_name.Buffer)
    {
        IoSetDeviceInterfaceState(&obj->link_name, FALSE);
        RtlFreeUnicodeString(&obj->link_name);
        obj->link_name.Buffer = NULL;
    }
}

/* A fake host controller, one per Linux bus, child of the bus FDO. */
struct usb_controller
{
    struct usb_object obj;

    struct list entry;
    BOOL removed;

    uint8_t busnum;
    struct usb_hub *hub;
};

/* A fake root hub; every device on the bus is exposed as one of its
 * children, connected at the port matching its bus address, regardless of
 * the physical hub topology. */
struct usb_hub
{
    struct usb_object obj;

    BOOL removed;
    /* Whether the hub PDO has been started. Children are only reported once
     * the hub itself is enumerated; before that, invalidating its relations
     * could enumerate children with a parent the PnP manager does not know
     * about yet. */
    BOOL started;

    uint8_t busnum;
    struct usb_controller *controller;
    struct list children;
};

static struct list controller_list = LIST_INIT(controller_list);

struct usb_device
{
    struct usb_object obj;

    struct list entry;
    BOOL removed;

    struct usb_hub *hub;

    bool interface;
    int16_t interface_index;

    uint8_t class, subclass, protocol, busnum, portnum;
    uint8_t devnum, speed;
    uint8_t port_path[USB_MAX_PORT_DEPTH];
    uint8_t port_path_len;

    uint16_t vendor, product, revision, usbver;

    /* Cached device descriptor followed by the raw descriptor set of every
     * configuration; only present for whole-device PDOs. */
    void *descriptors;
    uint32_t descriptors_len;

    struct unix_device *unix_device;

    LIST_ENTRY irp_list;
};

static DRIVER_OBJECT *driver_obj;
static DEVICE_OBJECT *bus_fdo, *bus_pdo;

static void destroy_unix_device(struct unix_device *unix_device)
{
    struct usb_destroy_device_params params =
    {
        .device = unix_device,
    };

    WINE_UNIX_CALL(unix_usb_destroy_device, &params);
}

static DEVICE_OBJECT *create_pdo(ULONG extension_size)
{
    static unsigned int name_index;
    DEVICE_OBJECT *device_obj;
    UNICODE_STRING string;
    NTSTATUS status;
    WCHAR name[26];

    swprintf(name, ARRAY_SIZE(name), L"\\Device\\USBPDO-%u", name_index++);
    RtlInitUnicodeString(&string, name);
    if ((status = IoCreateDevice(driver_obj, extension_size, &string,
            FILE_DEVICE_USB, 0, FALSE, &device_obj)))
    {
        ERR("Failed to create device, status %#lx.\n", status);
        return NULL;
    }
    return device_obj;
}

/* Called from the event thread only. */
static struct usb_controller *get_usb_controller(uint8_t busnum)
{
    DEVICE_OBJECT *controller_obj, *hub_obj;
    struct usb_controller *controller;
    struct usb_hub *hub;

    LIST_FOR_EACH_ENTRY(controller, &controller_list, struct usb_controller, entry)
    {
        if (controller->busnum == busnum && !controller->removed)
            return controller;
    }

    if (!(controller_obj = create_pdo(sizeof(*controller))))
        return NULL;
    if (!(hub_obj = create_pdo(sizeof(*hub))))
    {
        IoDeleteDevice(controller_obj);
        return NULL;
    }

    controller = controller_obj->DeviceExtension;
    controller->obj.kind = DEVICE_KIND_CONTROLLER;
    controller->obj.device_obj = controller_obj;
    controller->busnum = busnum;

    hub = hub_obj->DeviceExtension;
    hub->obj.kind = DEVICE_KIND_HUB;
    hub->obj.device_obj = hub_obj;
    hub->busnum = busnum;
    hub->controller = controller;
    list_init(&hub->children);
    controller->hub = hub;

    EnterCriticalSection(&wineusb_cs);
    list_add_tail(&controller_list, &controller->entry);
    LeaveCriticalSection(&wineusb_cs);

    /* The controller and hub report their own children once started, so a
     * single invalidation enumerates the whole chain in order. */
    IoInvalidateDeviceRelations(bus_pdo, BusRelations);
    return controller;
}

static void add_unix_device(const struct usb_add_device_event *event)
{
    struct usb_controller *controller;
    struct usb_device *device;
    DEVICE_OBJECT *device_obj;
    BOOL started;

    TRACE("Adding new device %p, vendor %04x, product %04x.\n", event->device,
            event->vendor, event->product);

    if (!(controller = get_usb_controller(event->busnum)))
        return;

    if (!(device_obj = create_pdo(sizeof(*device))))
        return;

    device = device_obj->DeviceExtension;
    device->obj.kind = DEVICE_KIND_DEVICE;
    device->obj.device_obj = device_obj;
    device->unix_device = event->device;
    InitializeListHead(&device->irp_list);
    device->removed = FALSE;
    device->hub = controller->hub;

    device->interface = event->interface;
    device->interface_index = event->interface_index;

    device->class = event->class;
    device->subclass = event->subclass;
    device->protocol = event->protocol;
    device->busnum = event->busnum;
    device->portnum = event->portnum;
    device->devnum = event->devnum;
    device->speed = event->speed;
    memcpy(device->port_path, event->port_path, sizeof(device->port_path));
    device->port_path_len = event->port_path_len;

    device->vendor = event->vendor;
    device->product = event->product;
    device->revision = event->revision;
    device->usbver = event->usbver;

    if (!device->interface)
    {
        struct usb_get_descriptors_params params = {.device = device->unix_device};
        UINT32 needed = 0;

        params.needed = &needed;
        if (WINE_UNIX_CALL(unix_usb_get_descriptors, &params) == STATUS_BUFFER_TOO_SMALL
                && (device->descriptors = ExAllocatePool(NonPagedPool, needed)))
        {
            params.buffer = device->descriptors;
            params.size = needed;
            if (WINE_UNIX_CALL(unix_usb_get_descriptors, &params))
            {
                ExFreePool(device->descriptors);
                device->descriptors = NULL;
            }
            else
            {
                device->descriptors_len = needed;
            }
        }
        if (!device->descriptors)
            WARN("Failed to cache descriptors for device %p.\n", event->device);
    }

    EnterCriticalSection(&wineusb_cs);
    list_add_tail(&controller->hub->children, &device->entry);
    /* If the hub is not started yet, it reports its children when it is. */
    started = controller->hub->started;
    LeaveCriticalSection(&wineusb_cs);

    if (started)
        IoInvalidateDeviceRelations(controller->hub->obj.device_obj, BusRelations);
}

static void remove_unix_device(struct unix_device *unix_device)
{
    struct usb_controller *controller;
    struct usb_hub *hub = NULL;
    struct usb_device *device;

    TRACE("Removing device %p.\n", unix_device);

    EnterCriticalSection(&wineusb_cs);
    LIST_FOR_EACH_ENTRY(controller, &controller_list, struct usb_controller, entry)
    {
        if (!controller->hub)
            continue;
        LIST_FOR_EACH_ENTRY(device, &controller->hub->children, struct usb_device, entry)
        {
            if (device->unix_device == unix_device)
            {
                hub = controller->hub;
                if (!device->removed)
                {
                    device->removed = TRUE;
                    list_remove(&device->entry);
                }
                break;
            }
        }
        if (hub)
            break;
    }
    LeaveCriticalSection(&wineusb_cs);

    if (hub)
        IoInvalidateDeviceRelations(hub->obj.device_obj, BusRelations);
}

static HANDLE event_thread;

static void complete_irp(IRP *irp)
{
    EnterCriticalSection(&wineusb_cs);
    RemoveEntryList(&irp->Tail.Overlay.ListEntry);
    LeaveCriticalSection(&wineusb_cs);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
}

static DWORD CALLBACK event_thread_proc(void *arg)
{
    struct usb_event event;
    struct usb_main_loop_params params =
    {
        .event = &event,
    };

    TRACE("Starting event thread.\n");

    if (WINE_UNIX_CALL(unix_usb_init, NULL) != STATUS_SUCCESS)
        return 0;

    while (WINE_UNIX_CALL(unix_usb_main_loop, &params) == STATUS_PENDING)
    {
        switch (event.type)
        {
            case USB_EVENT_ADD_DEVICE:
                add_unix_device(&event.u.added_device);
                break;

            case USB_EVENT_REMOVE_DEVICE:
                remove_unix_device(event.u.removed_device);
                break;

            case USB_EVENT_TRANSFER_COMPLETE:
                complete_irp(event.u.completed_irp);
                break;
        }
    }

    TRACE("Shutting down event thread.\n");
    return 0;
}

static NTSTATUS fdo_pnp(IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS ret;

    TRACE("irp %p, minor function %#x.\n", irp, stack->MinorFunction);

    switch (stack->MinorFunction)
    {
        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            struct usb_controller *controller;
            DEVICE_RELATIONS *devices;
            unsigned int i = 0;

            if (stack->Parameters.QueryDeviceRelations.Type != BusRelations)
            {
                FIXME("Unhandled device relations type %#x.\n", stack->Parameters.QueryDeviceRelations.Type);
                break;
            }

            EnterCriticalSection(&wineusb_cs);

            if (!(devices = ExAllocatePool(PagedPool,
                    offsetof(DEVICE_RELATIONS, Objects[list_count(&controller_list)]))))
            {
                LeaveCriticalSection(&wineusb_cs);
                irp->IoStatus.Status = STATUS_NO_MEMORY;
                break;
            }

            LIST_FOR_EACH_ENTRY(controller, &controller_list, struct usb_controller, entry)
            {
                devices->Objects[i++] = controller->obj.device_obj;
                call_fastcall_func1(ObfReferenceObject, controller->obj.device_obj);
            }

            LeaveCriticalSection(&wineusb_cs);

            devices->Count = i;
            irp->IoStatus.Information = (ULONG_PTR)devices;
            irp->IoStatus.Status = STATUS_SUCCESS;
            break;
        }

        case IRP_MN_START_DEVICE:
            event_thread = CreateThread(NULL, 0, event_thread_proc, NULL, 0, NULL);

            irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            irp->IoStatus.Status = STATUS_SUCCESS;
            break;

        case IRP_MN_REMOVE_DEVICE:
        {
            struct usb_controller *controller, *cursor;
            struct usb_device *device, *cursor2;

            WINE_UNIX_CALL(unix_usb_exit, NULL);
            WaitForSingleObject(event_thread, INFINITE);
            CloseHandle(event_thread);

            EnterCriticalSection(&wineusb_cs);
            /* Normally we unlink all devices either:
             *
             * - as a result of hot-unplug, which unlinks the device, and causes
             *   a subsequent IRP_MN_REMOVE_DEVICE which will free it;
             *
             * - if the parent is deleted (at shutdown time), in which case
             *   ntoskrnl will send us IRP_MN_SURPRISE_REMOVAL and
             *   IRP_MN_REMOVE_DEVICE unprompted.
             *
             * But we can get devices hotplugged between when shutdown starts
             * and now, in which case they'll be stuck in this list and never
             * freed.
             *
             * FIXME: This is still broken, though. If a device is hotplugged
             * and then removed, it'll be unlinked and never freed. */
            LIST_FOR_EACH_ENTRY_SAFE(controller, cursor, &controller_list, struct usb_controller, entry)
            {
                if (controller->hub)
                {
                    LIST_FOR_EACH_ENTRY_SAFE(device, cursor2, &controller->hub->children, struct usb_device, entry)
                    {
                        assert(!device->removed);
                        destroy_unix_device(device->unix_device);
                        list_remove(&device->entry);
                        if (device->descriptors)
                            ExFreePool(device->descriptors);
                        IoDeleteDevice(device->obj.device_obj);
                    }
                    IoDeleteDevice(controller->hub->obj.device_obj);
                }
                list_remove(&controller->entry);
                IoDeleteDevice(controller->obj.device_obj);
            }
            LeaveCriticalSection(&wineusb_cs);

            irp->IoStatus.Status = STATUS_SUCCESS;
            IoSkipCurrentIrpStackLocation(irp);
            ret = IoCallDriver(bus_pdo, irp);
            IoDetachDevice(bus_pdo);
            IoDeleteDevice(bus_fdo);
            return ret;
        }

        case IRP_MN_QUERY_ID:
            break;

        default:
            FIXME("Unhandled minor function %#x.\n", stack->MinorFunction);
    }

    IoSkipCurrentIrpStackLocation(irp);
    return IoCallDriver(bus_pdo, irp);
}

struct string_buffer
{
    WCHAR *string;
    size_t len;
};

static void WINAPIV append_id(struct string_buffer *buffer, const WCHAR *format, ...)
{
    va_list args;
    WCHAR *string;
    int len;

    va_start(args, format);

    len = _vsnwprintf(NULL, 0, format, args) + 1;
    if (!(string = ExAllocatePool(PagedPool, (buffer->len + len) * sizeof(WCHAR))))
    {
        if (buffer->string)
            ExFreePool(buffer->string);
        buffer->string = NULL;
        return;
    }
    if (buffer->string)
    {
        memcpy(string, buffer->string, buffer->len * sizeof(WCHAR));
        ExFreePool(buffer->string);
    }
    _vsnwprintf(string + buffer->len, len, format, args);
    buffer->string = string;
    buffer->len += len;

    va_end(args);
}

static void get_device_id(const struct usb_device *device, struct string_buffer *buffer)
{
    if (device->interface)
        append_id(buffer, L"USB\\VID_%04X&PID_%04X&MI_%02X",
                device->vendor, device->product, device->interface_index);
    else
        append_id(buffer, L"USB\\VID_%04X&PID_%04X", device->vendor, device->product);
}

static void get_instance_id(const struct usb_device *device, struct string_buffer *buffer)
{
    append_id(buffer, L"%u&%u&%u&%u", device->usbver, device->revision, device->busnum, device->portnum);
}

static void get_hardware_ids(const struct usb_device *device, struct string_buffer *buffer)
{
    if (device->interface)
        append_id(buffer, L"USB\\VID_%04X&PID_%04X&REV_%04X&MI_%02X",
                device->vendor, device->product, device->revision, device->interface_index);
    else
        append_id(buffer, L"USB\\VID_%04X&PID_%04X&REV_%04X",
                device->vendor, device->product, device->revision);

    get_device_id(device, buffer);
    append_id(buffer, L"");
}

static void get_compatible_ids(const struct usb_device *device, struct string_buffer *buffer)
{
    if (device->interface_index != -1)
    {
        append_id(buffer, L"USB\\Class_%02x&SubClass_%02x&Prot_%02x",
                device->class, device->subclass, device->protocol);
        append_id(buffer, L"USB\\Class_%02x&SubClass_%02x", device->class, device->subclass);
        append_id(buffer, L"USB\\Class_%02x", device->class);
    }
    else
    {
        append_id(buffer, L"USB\\DevClass_%02x&SubClass_%02x&Prot_%02x",
                device->class, device->subclass, device->protocol);
        append_id(buffer, L"USB\\DevClass_%02x&SubClass_%02x", device->class, device->subclass);
        append_id(buffer, L"USB\\DevClass_%02x", device->class);
    }
    append_id(buffer, L"");
}

static NTSTATUS query_id(struct usb_device *device, IRP *irp, BUS_QUERY_ID_TYPE type)
{
    struct string_buffer buffer = {0};

    TRACE("type %#x.\n", type);

    switch (type)
    {
        case BusQueryDeviceID:
            get_device_id(device, &buffer);
            break;

        case BusQueryInstanceID:
            get_instance_id(device, &buffer);
            break;

        case BusQueryHardwareIDs:
            get_hardware_ids(device, &buffer);
            break;

        case BusQueryCompatibleIDs:
            get_compatible_ids(device, &buffer);
            break;

        default:
            FIXME("Unhandled ID query type %#x.\n", type);
            return irp->IoStatus.Status;
    }

    if (!buffer.string)
        return STATUS_NO_MEMORY;

    irp->IoStatus.Information = (ULONG_PTR)buffer.string;
    return STATUS_SUCCESS;
}

static NTSTATUS controller_query_id(struct usb_controller *controller, IRP *irp, BUS_QUERY_ID_TYPE type)
{
    struct string_buffer buffer = {0};

    TRACE("type %#x.\n", type);

    switch (type)
    {
        case BusQueryDeviceID:
            append_id(&buffer, L"PCI\\VEN_1D6B&DEV_0003");
            break;

        case BusQueryInstanceID:
            append_id(&buffer, L"%u", controller->busnum);
            break;

        case BusQueryHardwareIDs:
            append_id(&buffer, L"PCI\\VEN_1D6B&DEV_0003");
            append_id(&buffer, L"");
            break;

        case BusQueryCompatibleIDs:
            append_id(&buffer, L"");
            break;

        default:
            FIXME("Unhandled ID query type %#x.\n", type);
            return irp->IoStatus.Status;
    }

    if (!buffer.string)
        return STATUS_NO_MEMORY;

    irp->IoStatus.Information = (ULONG_PTR)buffer.string;
    return STATUS_SUCCESS;
}

static NTSTATUS hub_query_id(struct usb_hub *hub, IRP *irp, BUS_QUERY_ID_TYPE type)
{
    struct string_buffer buffer = {0};

    TRACE("type %#x.\n", type);

    switch (type)
    {
        case BusQueryDeviceID:
            append_id(&buffer, L"USB\\ROOT_HUB30");
            break;

        case BusQueryInstanceID:
            append_id(&buffer, L"%u", hub->busnum);
            break;

        case BusQueryHardwareIDs:
            append_id(&buffer, L"USB\\ROOT_HUB30&VID1D6B&PID0003");
            append_id(&buffer, L"USB\\ROOT_HUB30");
            append_id(&buffer, L"");
            break;

        case BusQueryCompatibleIDs:
            append_id(&buffer, L"");
            break;

        default:
            FIXME("Unhandled ID query type %#x.\n", type);
            return irp->IoStatus.Status;
    }

    if (!buffer.string)
        return STATUS_NO_MEMORY;

    irp->IoStatus.Information = (ULONG_PTR)buffer.string;
    return STATUS_SUCCESS;
}

static NTSTATUS controller_pnp(DEVICE_OBJECT *device_obj, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct usb_controller *controller = device_obj->DeviceExtension;
    NTSTATUS ret = irp->IoStatus.Status;

    TRACE("device_obj %p, irp %p, minor function %#x.\n", device_obj, irp, stack->MinorFunction);

    switch (stack->MinorFunction)
    {
        case IRP_MN_QUERY_ID:
            ret = controller_query_id(controller, irp, stack->Parameters.QueryId.IdType);
            break;

        case IRP_MN_QUERY_CAPABILITIES:
        {
            DEVICE_CAPABILITIES *caps = stack->Parameters.DeviceCapabilities.Capabilities;

            caps->RawDeviceOK = 1;
            caps->UniqueID = 1;

            ret = STATUS_SUCCESS;
            break;
        }

        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            DEVICE_RELATIONS *devices;
            unsigned int i = 0;

            if (stack->Parameters.QueryDeviceRelations.Type != BusRelations)
            {
                FIXME("Unhandled device relations type %#x.\n", stack->Parameters.QueryDeviceRelations.Type);
                break;
            }

            EnterCriticalSection(&wineusb_cs);

            if (!(devices = ExAllocatePool(PagedPool, offsetof(DEVICE_RELATIONS, Objects[1]))))
            {
                LeaveCriticalSection(&wineusb_cs);
                ret = STATUS_NO_MEMORY;
                break;
            }

            if (controller->hub && !controller->hub->removed)
            {
                devices->Objects[i++] = controller->hub->obj.device_obj;
                call_fastcall_func1(ObfReferenceObject, controller->hub->obj.device_obj);
            }

            LeaveCriticalSection(&wineusb_cs);

            devices->Count = i;
            irp->IoStatus.Information = (ULONG_PTR)devices;
            ret = STATUS_SUCCESS;
            break;
        }

        case IRP_MN_START_DEVICE:
            register_device_interface(&controller->obj);
            IoInvalidateDeviceRelations(device_obj, BusRelations);
            ret = STATUS_SUCCESS;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            EnterCriticalSection(&wineusb_cs);
            if (!controller->removed)
            {
                controller->removed = TRUE;
                list_remove(&controller->entry);
            }
            LeaveCriticalSection(&wineusb_cs);
            ret = STATUS_SUCCESS;
            break;

        case IRP_MN_REMOVE_DEVICE:
            unregister_device_interface(&controller->obj);
            if (controller->removed)
                IoDeleteDevice(controller->obj.device_obj);
            /* Otherwise the controller is still linked; the bus FDO will
             * delete it. */
            ret = STATUS_SUCCESS;
            break;

        default:
            FIXME("Unhandled minor function %#x.\n", stack->MinorFunction);
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return ret;
}

static NTSTATUS hub_pnp(DEVICE_OBJECT *device_obj, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct usb_hub *hub = device_obj->DeviceExtension;
    NTSTATUS ret = irp->IoStatus.Status;

    TRACE("device_obj %p, irp %p, minor function %#x.\n", device_obj, irp, stack->MinorFunction);

    switch (stack->MinorFunction)
    {
        case IRP_MN_QUERY_ID:
            ret = hub_query_id(hub, irp, stack->Parameters.QueryId.IdType);
            break;

        case IRP_MN_QUERY_CAPABILITIES:
        {
            DEVICE_CAPABILITIES *caps = stack->Parameters.DeviceCapabilities.Capabilities;

            caps->RawDeviceOK = 1;
            caps->UniqueID = 1;

            ret = STATUS_SUCCESS;
            break;
        }

        case IRP_MN_QUERY_DEVICE_RELATIONS:
        {
            struct usb_device *device;
            DEVICE_RELATIONS *devices;
            unsigned int i = 0;

            if (stack->Parameters.QueryDeviceRelations.Type != BusRelations)
            {
                FIXME("Unhandled device relations type %#x.\n", stack->Parameters.QueryDeviceRelations.Type);
                break;
            }

            EnterCriticalSection(&wineusb_cs);

            if (!(devices = ExAllocatePool(PagedPool,
                    offsetof(DEVICE_RELATIONS, Objects[list_count(&hub->children)]))))
            {
                LeaveCriticalSection(&wineusb_cs);
                ret = STATUS_NO_MEMORY;
                break;
            }

            LIST_FOR_EACH_ENTRY(device, &hub->children, struct usb_device, entry)
            {
                devices->Objects[i++] = device->obj.device_obj;
                call_fastcall_func1(ObfReferenceObject, device->obj.device_obj);
            }

            LeaveCriticalSection(&wineusb_cs);

            devices->Count = i;
            irp->IoStatus.Information = (ULONG_PTR)devices;
            ret = STATUS_SUCCESS;
            break;
        }

        case IRP_MN_START_DEVICE:
            register_device_interface(&hub->obj);
            EnterCriticalSection(&wineusb_cs);
            hub->started = TRUE;
            LeaveCriticalSection(&wineusb_cs);
            IoInvalidateDeviceRelations(device_obj, BusRelations);
            ret = STATUS_SUCCESS;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            EnterCriticalSection(&wineusb_cs);
            if (!hub->removed)
            {
                hub->removed = TRUE;
                hub->controller->hub = NULL;
            }
            LeaveCriticalSection(&wineusb_cs);
            ret = STATUS_SUCCESS;
            break;

        case IRP_MN_REMOVE_DEVICE:
            unregister_device_interface(&hub->obj);
            if (hub->removed)
                IoDeleteDevice(hub->obj.device_obj);
            ret = STATUS_SUCCESS;
            break;

        default:
            FIXME("Unhandled minor function %#x.\n", stack->MinorFunction);
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return ret;
}

static void remove_pending_irps(struct usb_device *device)
{
    LIST_ENTRY *entry;
    IRP *irp;

    while ((entry = RemoveHeadList(&device->irp_list)) != &device->irp_list)
    {
        irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
        irp->IoStatus.Status = STATUS_DELETE_PENDING;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

static NTSTATUS pdo_pnp(DEVICE_OBJECT *device_obj, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct usb_device *device = device_obj->DeviceExtension;
    NTSTATUS ret = irp->IoStatus.Status;

    TRACE("device_obj %p, irp %p, minor function %#x.\n", device_obj, irp, stack->MinorFunction);

    switch (stack->MinorFunction)
    {
        case IRP_MN_QUERY_ID:
            ret = query_id(device, irp, stack->Parameters.QueryId.IdType);
            break;

        case IRP_MN_QUERY_CAPABILITIES:
        {
            DEVICE_CAPABILITIES *caps = stack->Parameters.DeviceCapabilities.Capabilities;

            caps->RawDeviceOK = 1;
            caps->Removable = 1;
            caps->Address = device->devnum;

            ret = STATUS_SUCCESS;
            break;
        }

        case IRP_MN_START_DEVICE:
            if (!device->interface)
                register_device_interface(&device->obj);
            ret = STATUS_SUCCESS;
            break;

        case IRP_MN_SURPRISE_REMOVAL:
            EnterCriticalSection(&wineusb_cs);
            remove_pending_irps(device);
            if (!device->removed)
            {
                device->removed = TRUE;
                list_remove(&device->entry);
            }
            LeaveCriticalSection(&wineusb_cs);
            ret = STATUS_SUCCESS;
            break;

        case IRP_MN_REMOVE_DEVICE:
            assert(device->removed);
            remove_pending_irps(device);

            unregister_device_interface(&device->obj);
            destroy_unix_device(device->unix_device);

            if (device->descriptors)
                ExFreePool(device->descriptors);
            IoDeleteDevice(device->obj.device_obj);
            ret = STATUS_SUCCESS;
            break;

        case IRP_MN_QUERY_DEVICE_TEXT:
            if (stack->Parameters.QueryDeviceText.DeviceTextType == DeviceTextLocationInformation)
            {
                struct string_buffer buffer = {0};

                append_id(&buffer, L"Port_#%04u.Hub_#%04u", device->devnum, device->busnum);
                if (!buffer.string)
                {
                    ret = STATUS_NO_MEMORY;
                    break;
                }
                irp->IoStatus.Information = (ULONG_PTR)buffer.string;
                ret = STATUS_SUCCESS;
                break;
            }
            WARN("Unhandled IRP_MN_QUERY_DEVICE_TEXT text type %u.\n", stack->Parameters.QueryDeviceText.DeviceTextType);
            break;

        default:
            FIXME("Unhandled minor function %#x.\n", stack->MinorFunction);
    }

    irp->IoStatus.Status = ret;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return ret;
}

static NTSTATUS WINAPI driver_pnp(DEVICE_OBJECT *device, IRP *irp)
{
    struct usb_object *obj = device->DeviceExtension;

    switch (obj->kind)
    {
        case DEVICE_KIND_FDO:
            return fdo_pnp(irp);
        case DEVICE_KIND_CONTROLLER:
            return controller_pnp(device, irp);
        case DEVICE_KIND_HUB:
            return hub_pnp(device, irp);
        default:
            return pdo_pnp(device, irp);
    }
}

static NTSTATUS usb_submit_urb(struct usb_device *device, IRP *irp)
{
    URB *urb = IoGetCurrentIrpStackLocation(irp)->Parameters.Others.Argument1;
    NTSTATUS status;

    TRACE("type %#x.\n", urb->UrbHeader.Function);

    switch (urb->UrbHeader.Function)
    {
        case URB_FUNCTION_ABORT_PIPE:
        {
            struct _URB_PIPE_REQUEST *req = &urb->UrbPipeRequest;
            LIST_ENTRY *entry, *mark;

            /* The documentation states that URB_FUNCTION_ABORT_PIPE may
             * complete before outstanding requests complete, so we don't need
             * to wait for them. */
            EnterCriticalSection(&wineusb_cs);
            mark = &device->irp_list;
            for (entry = mark->Flink; entry != mark; entry = entry->Flink)
            {
                IRP *queued_irp = CONTAINING_RECORD(entry, IRP, Tail.Overlay.ListEntry);
                struct usb_cancel_transfer_params params =
                {
                    .transfer = queued_irp->Tail.Overlay.DriverContext[0],
                };

                if (req->PipeHandle)
                {
                    URB *queued_urb = IoGetCurrentIrpStackLocation(queued_irp)->Parameters.Others.Argument1;
                    HANDLE pipe;

                    switch (queued_urb->UrbHeader.Function)
                    {
                        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
                            pipe = queued_urb->UrbBulkOrInterruptTransfer.PipeHandle;
                            break;
                        case URB_FUNCTION_CONTROL_TRANSFER:
                            pipe = queued_urb->UrbControlTransfer.PipeHandle;
                            break;
                        default:
                            pipe = NULL;
                    }
                    if (pipe != req->PipeHandle)
                        continue;
                }

                WINE_UNIX_CALL(unix_usb_cancel_transfer, &params);
            }
            LeaveCriticalSection(&wineusb_cs);

            return STATUS_SUCCESS;
        }

        case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
        case URB_FUNCTION_SELECT_CONFIGURATION:
        case URB_FUNCTION_SELECT_INTERFACE:
        case URB_FUNCTION_CONTROL_TRANSFER:
        case URB_FUNCTION_VENDOR_DEVICE:
        case URB_FUNCTION_VENDOR_INTERFACE:
        case URB_FUNCTION_VENDOR_ENDPOINT:
        {
            struct usb_submit_urb_params params =
            {
                .device = device->unix_device,
                .irp = irp,
            };

            switch (urb->UrbHeader.Function)
            {
                case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
                {
                    struct _URB_BULK_OR_INTERRUPT_TRANSFER *req = &urb->UrbBulkOrInterruptTransfer;
                    if (req->TransferBufferMDL)
                        params.transfer_buffer = MmGetSystemAddressForMdlSafe(req->TransferBufferMDL, NormalPagePriority);
                    else
                        params.transfer_buffer = req->TransferBuffer;
                    break;
                }

                case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
                {
                    struct _URB_CONTROL_DESCRIPTOR_REQUEST *req = &urb->UrbControlDescriptorRequest;
                    if (req->TransferBufferMDL)
                        params.transfer_buffer = MmGetSystemAddressForMdlSafe(req->TransferBufferMDL, NormalPagePriority);
                    else
                        params.transfer_buffer = req->TransferBuffer;
                    break;
                }

                case URB_FUNCTION_CONTROL_TRANSFER:
                {
                    struct _URB_CONTROL_TRANSFER *req = &urb->UrbControlTransfer;
                    if (req->TransferBufferMDL)
                        params.transfer_buffer = MmGetSystemAddressForMdlSafe(req->TransferBufferMDL, NormalPagePriority);
                    else
                        params.transfer_buffer = req->TransferBuffer;
                    break;
                }

                case URB_FUNCTION_VENDOR_DEVICE:
                case URB_FUNCTION_VENDOR_INTERFACE:
                case URB_FUNCTION_VENDOR_ENDPOINT:
                {
                    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *req = &urb->UrbControlVendorClassRequest;
                    if (req->TransferBufferMDL)
                        params.transfer_buffer = MmGetSystemAddressForMdlSafe(req->TransferBufferMDL, NormalPagePriority);
                    else
                        params.transfer_buffer = req->TransferBuffer;
                    break;
                }
            }

            /* Hold the wineusb lock while submitting and queuing, and
             * similarly hold it in complete_irp(). That way, if libusb reports
             * completion between submitting and queuing, we won't try to
             * dequeue the IRP until it's actually been queued. */
            EnterCriticalSection(&wineusb_cs);
            status = WINE_UNIX_CALL(unix_usb_submit_urb, &params);
            if (status == STATUS_PENDING)
            {
                IoMarkIrpPending(irp);
                InsertTailList(&device->irp_list, &irp->Tail.Overlay.ListEntry);
            }
            LeaveCriticalSection(&wineusb_cs);

            return status;
        }

        default:
            FIXME("Unhandled function %#x.\n", urb->UrbHeader.Function);
    }

    return STATUS_NOT_IMPLEMENTED;
}

static NTSTATUS WINAPI driver_internal_ioctl(DEVICE_OBJECT *device_obj, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    struct usb_device *device = device_obj->DeviceExtension;
    NTSTATUS status = STATUS_NOT_IMPLEMENTED;
    BOOL removed;

    TRACE("device_obj %p, irp %p, code %#lx.\n", device_obj, irp, code);

    if (device->obj.kind != DEVICE_KIND_DEVICE)
    {
        irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }

    EnterCriticalSection(&wineusb_cs);
    removed = device->removed;
    LeaveCriticalSection(&wineusb_cs);

    if (removed)
    {
        irp->IoStatus.Status = STATUS_DELETE_PENDING;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_DELETE_PENDING;
    }

    switch (code)
    {
        case IOCTL_INTERNAL_USB_SUBMIT_URB:
            status = usb_submit_urb(device, irp);
            break;

        default:
            FIXME("Unhandled ioctl %#lx (device %#lx, access %#lx, function %#lx, method %#lx).\n",
                    code, code >> 16, (code >> 14) & 3, (code >> 2) & 0xfff, code & 3);
    }

    if (status != STATUS_PENDING)
    {
        irp->IoStatus.Status = status;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
    return status;
}

static UCHAR usb_device_speed(const struct usb_device *device)
{
    switch (device->speed)
    {
        case USB_SPEED_LOW:
            return UsbLowSpeed;
        case USB_SPEED_HIGH:
            return UsbHighSpeed;
        case USB_SPEED_SUPER:
        case USB_SPEED_SUPER_PLUS:
            return UsbSuperSpeed;
        default:
            return UsbFullSpeed;
    }
}

/* Find the whole-device child connected at the given port. The bus address
 * doubles as the port number, so interface PDOs of composite devices, which
 * share their parent's address, are skipped. Called with wineusb_cs held. */
static struct usb_device *hub_find_connection(struct usb_hub *hub, ULONG index)
{
    struct usb_device *device;

    LIST_FOR_EACH_ENTRY(device, &hub->children, struct usb_device, entry)
    {
        if (!device->interface && device->devnum == index)
            return device;
    }
    return NULL;
}

/* Return the raw descriptor set of the given configuration, cached at
 * enumeration time. Called with wineusb_cs held. */
static const UCHAR *get_cached_config_descriptor(const struct usb_device *device,
        UCHAR index, USHORT *total_len)
{
    const UCHAR *descriptors = device->descriptors;
    uint32_t offset = sizeof(USB_DEVICE_DESCRIPTOR);
    UCHAR i;

    if (!descriptors)
        return NULL;

    for (i = 0;; ++i)
    {
        USHORT len;

        if (offset + sizeof(USB_CONFIGURATION_DESCRIPTOR) > device->descriptors_len)
            return NULL;
        len = descriptors[offset + 2] | (descriptors[offset + 3] << 8);
        if (offset + len > device->descriptors_len)
            return NULL;
        if (i == index)
        {
            *total_len = len;
            return descriptors + offset;
        }
        offset += len;
    }
}

static NTSTATUS hub_ioctl(struct usb_hub *hub, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG inlen = stack->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outlen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    void *buffer = irp->AssociatedIrp.SystemBuffer;

    switch (code)
    {
        case IOCTL_USB_GET_NODE_INFORMATION:
        {
            USB_NODE_INFORMATION *info = buffer;

            if (outlen < sizeof(*info))
                return STATUS_BUFFER_TOO_SMALL;

            memset(info, 0, sizeof(*info));
            info->NodeType = UsbHub;
            info->u.HubInformation.HubDescriptor.bDescriptorLength = 9;
            info->u.HubInformation.HubDescriptor.bDescriptorType = 0x29;
            info->u.HubInformation.HubDescriptor.bNumberOfPorts = 127;
            irp->IoStatus.Information = sizeof(*info);
            return STATUS_SUCCESS;
        }

        case IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX:
        {
            USB_NODE_CONNECTION_INFORMATION_EX *info = buffer;
            ULONG size = offsetof(USB_NODE_CONNECTION_INFORMATION_EX, PipeList[0]);
            struct usb_device *device;
            ULONG index;

            if (inlen < sizeof(info->ConnectionIndex) || outlen < size)
                return STATUS_BUFFER_TOO_SMALL;

            index = info->ConnectionIndex;
            memset(info, 0, size);
            info->ConnectionIndex = index;

            EnterCriticalSection(&wineusb_cs);
            if ((device = hub_find_connection(hub, index)))
            {
                if (device->descriptors)
                {
                    const UCHAR *config;
                    USHORT config_len;

                    memcpy(&info->DeviceDescriptor, device->descriptors,
                            min(device->descriptors_len, sizeof(info->DeviceDescriptor)));
                    if ((config = get_cached_config_descriptor(device, 0, &config_len)))
                        info->CurrentConfigurationValue = config[5];
                }
                info->Speed = usb_device_speed(device);
                info->DeviceAddress = device->devnum;
                info->ConnectionStatus = DeviceConnected;
            }
            LeaveCriticalSection(&wineusb_cs);

            irp->IoStatus.Information = size;
            return STATUS_SUCCESS;
        }

        case IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX_V2:
        {
            USB_NODE_CONNECTION_INFORMATION_EX_V2 *info = buffer;
            struct usb_device *device;
            ULONG index;

            if (inlen < sizeof(info->ConnectionIndex) || outlen < sizeof(*info))
                return STATUS_BUFFER_TOO_SMALL;

            index = info->ConnectionIndex;
            memset(info, 0, sizeof(*info));
            info->ConnectionIndex = index;
            info->Length = sizeof(*info);

            EnterCriticalSection(&wineusb_cs);
            if ((device = hub_find_connection(hub, index)))
            {
                info->SupportedUsbProtocols.ul = 0x03; /* Usb110 | Usb200 */
                if (device->speed >= USB_SPEED_SUPER)
                {
                    info->SupportedUsbProtocols.ul |= 0x04; /* Usb300 */
                    /* DeviceIsOperatingAtSuperSpeedOrHigher | DeviceIsSuperSpeedCapableOrHigher */
                    info->Flags.ul = 0x03;
                }
            }
            LeaveCriticalSection(&wineusb_cs);

            irp->IoStatus.Information = sizeof(*info);
            return STATUS_SUCCESS;
        }

        case IOCTL_USB_GET_DESCRIPTOR_FROM_NODE_CONNECTION:
        {
            USB_DESCRIPTOR_REQUEST *req = buffer;
            ULONG header_len = offsetof(USB_DESCRIPTOR_REQUEST, Data[0]);
            struct usb_device *device;
            NTSTATUS status;
            ULONG copied = 0;

            if (inlen < header_len || outlen < header_len)
                return STATUS_BUFFER_TOO_SMALL;

            EnterCriticalSection(&wineusb_cs);
            if (!(device = hub_find_connection(hub, req->ConnectionIndex)) || !device->descriptors)
            {
                status = STATUS_DEVICE_NOT_CONNECTED;
            }
            else
            {
                UCHAR type = req->SetupPacket.wValue >> 8;
                ULONG space = min(outlen - header_len, req->SetupPacket.wLength);

                switch (type)
                {
                    case USB_DEVICE_DESCRIPTOR_TYPE:
                        copied = min(space, min(device->descriptors_len, sizeof(USB_DEVICE_DESCRIPTOR)));
                        memcpy(req->Data, device->descriptors, copied);
                        status = STATUS_SUCCESS;
                        break;

                    case USB_CONFIGURATION_DESCRIPTOR_TYPE:
                    {
                        USHORT config_len;
                        const UCHAR *config = get_cached_config_descriptor(device,
                                req->SetupPacket.wValue & 0xff, &config_len);

                        if (config)
                        {
                            copied = min(space, config_len);
                            memcpy(req->Data, config, copied);
                            status = STATUS_SUCCESS;
                        }
                        else
                        {
                            status = STATUS_UNSUCCESSFUL;
                        }
                        break;
                    }

                    default:
                        FIXME("Unhandled descriptor type %#x.\n", type);
                        status = STATUS_NOT_SUPPORTED;
                }
            }
            LeaveCriticalSection(&wineusb_cs);

            if (!status)
                irp->IoStatus.Information = header_len + copied;
            return status;
        }

        default:
            FIXME("Unhandled ioctl %#lx (device %#lx, access %#lx, function %#lx, method %#lx).\n",
                    code, code >> 16, (code >> 14) & 3, (code >> 2) & 0xfff, code & 3);
            return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS controller_ioctl(struct usb_controller *controller, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    ULONG code = stack->Parameters.DeviceIoControl.IoControlCode;
    ULONG outlen = stack->Parameters.DeviceIoControl.OutputBufferLength;
    void *buffer = irp->AssociatedIrp.SystemBuffer;

    switch (code)
    {
        case IOCTL_USB_GET_ROOT_HUB_NAME:
        {
            USB_ROOT_HUB_NAME *name = buffer;
            ULONG header_len = offsetof(USB_ROOT_HUB_NAME, RootHubName[0]);
            const WCHAR *link;
            ULONG needed, len;

            if (outlen < sizeof(name->ActualLength))
                return STATUS_BUFFER_TOO_SMALL;

            EnterCriticalSection(&wineusb_cs);
            if (!controller->hub || !controller->hub->obj.link_name.Buffer)
            {
                LeaveCriticalSection(&wineusb_cs);
                return STATUS_UNSUCCESSFUL;
            }

            /* Strip the "\??\" prefix; the caller expects a name it can
             * prepend "\\.\" to. */
            link = controller->hub->obj.link_name.Buffer + 4;
            len = wcslen(link);
            needed = header_len + (len + 1) * sizeof(WCHAR);

            name->ActualLength = needed;
            if (outlen >= needed)
            {
                memcpy(name->RootHubName, link, (len + 1) * sizeof(WCHAR));
                irp->IoStatus.Information = needed;
            }
            else
            {
                if (outlen > header_len)
                {
                    len = (outlen - header_len) / sizeof(WCHAR);
                    if (len)
                    {
                        memcpy(name->RootHubName, link, (len - 1) * sizeof(WCHAR));
                        name->RootHubName[len - 1] = 0;
                    }
                }
                irp->IoStatus.Information = min(outlen, needed);
            }
            LeaveCriticalSection(&wineusb_cs);
            return STATUS_SUCCESS;
        }

        default:
            FIXME("Unhandled ioctl %#lx (device %#lx, access %#lx, function %#lx, method %#lx).\n",
                    code, code >> 16, (code >> 14) & 3, (code >> 2) & 0xfff, code & 3);
            return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS WINAPI driver_ioctl(DEVICE_OBJECT *device_obj, IRP *irp)
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation(irp);
    struct usb_object *obj = device_obj->DeviceExtension;
    NTSTATUS status = STATUS_NOT_SUPPORTED;

    TRACE("device_obj %p, irp %p, code %#lx.\n", device_obj, irp,
            stack->Parameters.DeviceIoControl.IoControlCode);

    switch (obj->kind)
    {
        case DEVICE_KIND_CONTROLLER:
            status = controller_ioctl((struct usb_controller *)obj, irp);
            break;

        case DEVICE_KIND_HUB:
            status = hub_ioctl((struct usb_hub *)obj, irp);
            break;

        default:
            FIXME("Unhandled ioctl %#lx for device kind %u.\n",
                    stack->Parameters.DeviceIoControl.IoControlCode, obj->kind);
    }

    irp->IoStatus.Status = status;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

static NTSTATUS WINAPI driver_create(DEVICE_OBJECT *device, IRP *irp)
{
    TRACE("device %p, irp %p.\n", device, irp);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI driver_close(DEVICE_OBJECT *device, IRP *irp)
{
    TRACE("device %p, irp %p.\n", device, irp);

    irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static NTSTATUS WINAPI driver_add_device(DRIVER_OBJECT *driver, DEVICE_OBJECT *pdo)
{
    NTSTATUS ret;

    TRACE("driver %p, pdo %p.\n", driver, pdo);

    if ((ret = IoCreateDevice(driver, sizeof(struct usb_object), NULL, FILE_DEVICE_BUS_EXTENDER, 0, FALSE, &bus_fdo)))
    {
        ERR("Failed to create FDO, status %#lx.\n", ret);
        return ret;
    }

    ((struct usb_object *)bus_fdo->DeviceExtension)->kind = DEVICE_KIND_FDO;
    ((struct usb_object *)bus_fdo->DeviceExtension)->device_obj = bus_fdo;

    IoAttachDeviceToDeviceStack(bus_fdo, pdo);
    bus_pdo = pdo;
    bus_fdo->Flags &= ~DO_DEVICE_INITIALIZING;

    return STATUS_SUCCESS;
}

static void WINAPI driver_unload(DRIVER_OBJECT *driver)
{
}

NTSTATUS WINAPI DriverEntry(DRIVER_OBJECT *driver, UNICODE_STRING *path)
{
    NTSTATUS status;

    TRACE("driver %p, path %s.\n", driver, debugstr_w(path->Buffer));

    if ((status = __wine_init_unix_call()))
    {
        ERR("Failed to initialize Unix library, status %#lx.\n", status);
        return status;
    }

    driver_obj = driver;

    driver->DriverExtension->AddDevice = driver_add_device;
    driver->DriverUnload = driver_unload;
    driver->MajorFunction[IRP_MJ_CREATE] = driver_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = driver_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = driver_ioctl;
    driver->MajorFunction[IRP_MJ_PNP] = driver_pnp;
    driver->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = driver_internal_ioctl;

    return STATUS_SUCCESS;
}
