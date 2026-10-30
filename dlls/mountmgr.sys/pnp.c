/*
 * Plug and Play devices of the USB serial ports
 *
 * Copyright 2026 giantorth
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

/* The root device root\wine\mountmgr makes mountmgr the bus driver of the USB serial ports: each
 * port is reported to the PnP manager, under the composite device it belongs to if any, so that
 * it gets its Enum entry, its device parent and a GUID_DEVINTERFACE_COMPORT interface, and the
 * PnP manager notifies their arrival and removal.
 *
 * The PnP manager queues the devices whose relations are invalidated without referencing them,
 * so a device must stay around until it is enumerated. The port code only invalidates the bus;
 * a composite device invalidates itself when it starts or its ports change, so its PDO is never
 * deleted and is reported again when the device comes back. The PDO of a port is deleted when it
 * is removed, and may be told so again when its composite device is reported anew. */

#include <stdarg.h>
#include <stdlib.h>

#include "mountmgr.h"
#include "winreg.h"
#include "winnls.h"
#include "winuser.h"
#include "dbt.h"
#include "unixlib.h"

#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mountmgr);

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

/* GUID_DEVINTERFACE_COMPORT, which libuuid doesn't provide */
static const GUID comport_guid = { 0x86e0d1e0, 0x8089, 0x11d0, { 0x9c, 0xe4, 0x08, 0x00, 0x3e, 0x30, 0x1f, 0x73 } };

/* a USB serial port, or the composite USB device it belongs to */
struct usb_node
{
    struct list      entry;
    struct usb_node *parent;          /* composite device of a port, NULL if the port is the whole device */
    BOOL             composite;
    WCHAR           *device_id;       /* "USB\VID_xxxx&PID_xxxx[&MI_xx]" */
    WCHAR           *instance_id;     /* the rest of the device instance ID */
    WCHAR           *hardware_ids;    /* REG_MULTI_SZ */
    WCHAR           *compatible_ids;  /* REG_MULTI_SZ */
    WCHAR           *description;
    int              port;            /* COM port number of a port */
    int              home;            /* COM port number the port goes back to */
    unsigned int     present;         /* port is plugged in, or number of plugged-in ports of a composite */
    DEVICE_OBJECT   *pdo;             /* PDO of a port while it exists, of a composite once created */
    BOOL             started;         /* composite PDO started and not removed */
    BOOL             replace;         /* the port changed number, its PDO is replaced with a new one */
};

struct pdo_ext
{
    struct usb_node *node;            /* NULL once the PDO of a port is removed */
    UNICODE_STRING   interface;       /* GUID_DEVINTERFACE_COMPORT link of a port */
    BOOL             enabled;         /* the interface is enabled */
    int              port;            /* COM port number the interface was enabled for */
    BOOL             removed;
};

static CRITICAL_SECTION pnp_cs;
static CRITICAL_SECTION_DEBUG pnp_cs_debug =
{
    0, 0, &pnp_cs,
    { &pnp_cs_debug.ProcessLocksList, &pnp_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": pnp_cs") }
};
static CRITICAL_SECTION pnp_cs = { &pnp_cs_debug, -1, 0, 0, 0, 0 };

static struct list usb_nodes = LIST_INIT( usb_nodes );
static DEVICE_OBJECT *bus_fdo, *bus_pdo;
static BOOL bus_started;

/* duplicate a string, or a REG_MULTI_SZ if multi is set, from the pool for an IRP */
static WCHAR *pool_strdup( const WCHAR *str, BOOL multi )
{
    const WCHAR *end = str;
    WCHAR *ret;

    if (multi) while (*end) end += wcslen( end ) + 1;
    else end += wcslen( end );
    if ((ret = ExAllocatePool( PagedPool, (end - str + 1) * sizeof(WCHAR) )))
        memcpy( ret, str, (end - str + 1) * sizeof(WCHAR) );
    return ret;
}

/* duplicate a REG_MULTI_SZ built in a buffer that ends at pos */
static WCHAR *multi_strdup( const WCHAR *buffer, const WCHAR *pos )
{
    WCHAR *ret;

    if ((ret = malloc( (pos - buffer + 1) * sizeof(WCHAR) )))
    {
        memcpy( ret, buffer, (pos - buffer) * sizeof(WCHAR) );
        ret[pos - buffer] = 0;
    }
    return ret;
}

static struct usb_node *find_usb_node( const WCHAR *id )
{
    const WCHAR *instance = wcschr( id, '\\' );
    struct usb_node *node;

    LIST_FOR_EACH_ENTRY( node, &usb_nodes, struct usb_node, entry )
    {
        /* the device ID has a "USB\" prefix that the given ID doesn't */
        if (wcsnicmp( node->device_id + 4, id, instance - id ) || node->device_id[4 + instance - id]) continue;
        if (!wcsicmp( node->instance_id, instance + 1 )) return node;
    }
    return NULL;
}

static void free_usb_node( struct usb_node *node )
{
    list_remove( &node->entry );
    free( node->device_id );
    free( node->instance_id );
    free( node->hardware_ids );
    free( node->compatible_ids );
    free( node->description );
    free( node );
}

/* create the node of a port, or of a composite device; id is relative to Enum\USB */
static struct usb_node *create_usb_node( const struct serial_port_info *info, const WCHAR *id,
                                         struct usb_node *parent, BOOL composite )
{
    const WCHAR *instance = wcschr( id, '\\' );
    WCHAR buffer[512], *pos = buffer, *end = buffer + ARRAY_SIZE(buffer) - 1;
    const BYTE *class = composite ? info->dev_class : info->if_class;
    const WCHAR *class_name = composite ? L"DevClass" : L"Class";
    struct usb_node *node;

    if (!(node = calloc( 1, sizeof(*node) ))) return NULL;
    node->parent = parent;
    node->composite = composite;

    swprintf( buffer, ARRAY_SIZE(buffer), L"USB\\%.*s", (int)(instance - id), id );
    node->device_id = wcsdup( buffer );
    node->instance_id = wcsdup( instance + 1 );

    pos += swprintf( pos, end - pos, L"USB\\VID_%04X&PID_%04X&REV_%04X", info->vid, info->pid, info->revision );
    if (parent) pos += swprintf( pos, end - pos, L"&MI_%02X", info->interface );
    pos++;
    pos += swprintf( pos, end - pos, L"%s", node->device_id ) + 1;
    node->hardware_ids = multi_strdup( buffer, pos );

    pos = buffer;
    /* matched first, so that mountmgr's INF makes it a port rather than winusb.inf takes it */
    if (!composite) pos += swprintf( pos, end - pos, L"WINE\\USBSERIAL" ) + 1;
    pos += swprintf( pos, end - pos, L"USB\\%s_%02X&SubClass_%02X&Prot_%02X",
                     class_name, class[0], class[1], class[2] ) + 1;
    pos += swprintf( pos, end - pos, L"USB\\%s_%02X&SubClass_%02X", class_name, class[0], class[1] ) + 1;
    pos += swprintf( pos, end - pos, L"USB\\%s_%02X", class_name, class[0] ) + 1;
    if (composite) pos += swprintf( pos, end - pos, L"USB\\COMPOSITE" ) + 1;
    node->compatible_ids = multi_strdup( buffer, pos );

    if (!info->product[0] || !MultiByteToWideChar( CP_UTF8, 0, info->product, -1, buffer, ARRAY_SIZE(buffer) ))
        wcscpy( buffer, composite ? L"USB Composite Device" : L"USB Serial Device" );
    node->description = wcsdup( buffer );

    if (!node->device_id || !node->instance_id || !node->hardware_ids || !node->compatible_ids || !node->description)
    {
        free( node->device_id );
        free( node->instance_id );
        free( node->hardware_ids );
        free( node->compatible_ids );
        free( node->description );
        free( node );
        return NULL;
    }
    list_add_tail( &usb_nodes, &node->entry );
    return node;
}

/* open the Enum entry of a node if it exists; the PnP manager creates it, with its class */
static HKEY open_enum_key( const struct usb_node *node, REGSAM access )
{
    WCHAR path[MAX_PATH];
    HKEY key;

    swprintf( path, ARRAY_SIZE(path), L"System\\CurrentControlSet\\Enum\\%s\\%s", node->device_id, node->instance_id );
    return RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, access, &key ) ? NULL : key;
}

/* mark the Enum entry of a node as present or not; Wine has no other way to tell that a device is
 * gone, and the PnP manager can't open an entry marked phantom, so it is cleared before the node
 * is reported */
static void set_node_present( const struct usb_node *node, BOOL present )
{
    WCHAR service[16];
    DWORD one = 1, size = sizeof(service);
    HKEY key;

    if (!(key = open_enum_key( node, KEY_QUERY_VALUE | KEY_SET_VALUE ))) return;
    RegSetValueExW( key, L"WineSerialPort", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    if (!present) RegSetValueExW( key, L"Phantom", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    else
    {
        RegDeleteValueW( key, L"Phantom" );
        /* entries written before the ports were PnP devices name a driver that doesn't exist */
        if (!RegGetValueW( key, NULL, L"Service", RRF_RT_REG_SZ, NULL, service, &size ) &&
            !wcsicmp( service, node->composite ? L"usbccgp" : L"usbser" ))
            RegDeleteValueW( key, L"Service" );
    }
    RegCloseKey( key );
}

/* set the values of the Enum entry of a started port that tell its COM port */
static void set_port_entry( const struct usb_node *node, int port, DWORD home )
{
    WCHAR buffer[256];
    HKEY key, params_key;
    DWORD one = 1;

    if (!(key = open_enum_key( node, KEY_ALL_ACCESS ))) return;
    /* the entry of a new port is created after it arrived, it is marked here */
    RegSetValueExW( key, L"WineSerialPort", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    swprintf( buffer, ARRAY_SIZE(buffer), L"%s (COM%u)", node->description, port );
    RegSetValueExW( key, L"FriendlyName", 0, REG_SZ, (BYTE *)buffer, (wcslen( buffer ) + 1) * sizeof(WCHAR) );
    /* the number that the port gets back when it returns, see place_detected_ports */
    RegSetValueExW( key, L"WinePortNumber", 0, REG_DWORD, (BYTE *)&home, sizeof(home) );
    if (!RegCreateKeyExW( key, L"Device Parameters", 0, NULL, 0, KEY_SET_VALUE, NULL, &params_key, NULL ))
    {
        swprintf( buffer, ARRAY_SIZE(buffer), L"COM%u", port );
        RegSetValueExW( params_key, L"PortName", 0, REG_SZ, (BYTE *)buffer, (wcslen( buffer ) + 1) * sizeof(WCHAR) );
        RegCloseKey( params_key );
    }
    RegCloseKey( key );
}

/* have the PnP manager query the devices of the bus again; pnp_cs must be held */
static void invalidate_bus(void)
{
    if (bus_started) IoInvalidateDeviceRelations( bus_pdo, BusRelations );
}

/* have the PnP manager query the parent of a port again, after the port changed; pnp_cs must be held */
static void invalidate_parent( struct usb_node *node )
{
    if (!node->parent) invalidate_bus();
    else if (node->parent->started) IoInvalidateDeviceRelations( node->parent->pdo, BusRelations );
    /* otherwise the composite device reports its ports when it starts */
}

/* a USB serial port was created; id and parent_id are relative to Enum\USB */
void usb_serial_port_arrived( const struct serial_port_info *info, const WCHAR *id, const WCHAR *parent_id,
                              int port, int home )
{
    struct usb_node *parent = NULL, *node;

    EnterCriticalSection( &pnp_cs );

    if (parent_id && !(parent = find_usb_node( parent_id )) && !(parent = create_usb_node( info, parent_id, NULL, TRUE )))
        goto done;
    if (!(node = find_usb_node( id )) && !(node = create_usb_node( info, id, parent, FALSE ))) goto done;
    if (node->present) goto done;

    TRACE( "%s\\%s arrived as COM%u\n", debugstr_w(node->device_id), debugstr_w(node->instance_id), port );
    /* a port that moved to another number while its PDO still exists is reported anew */
    if (node->pdo && node->port != port) node->replace = TRUE;
    node->present = 1;
    node->port = port;
    node->home = home;
    set_node_present( node, TRUE );
    /* the bus reports a composite device once one of its ports is present */
    if (parent && !parent->present++)
    {
        set_node_present( parent, TRUE );
        invalidate_bus();
    }
    invalidate_parent( node );

done:
    LeaveCriticalSection( &pnp_cs );
}

/* a USB serial port was removed; id is relative to Enum\USB */
void usb_serial_port_removed( const WCHAR *id )
{
    struct usb_node *node, *parent;

    EnterCriticalSection( &pnp_cs );

    if ((node = find_usb_node( id )) && node->present)
    {
        TRACE( "%s\\%s removed\n", debugstr_w(node->device_id), debugstr_w(node->instance_id) );
        node->present = 0;
        set_node_present( node, FALSE );
        /* removing a composite device removes its ports along with it */
        if ((parent = node->parent) && !--parent->present)
        {
            set_node_present( parent, FALSE );
            invalidate_bus();
        }
        invalidate_parent( node );
        if (!node->pdo) free_usb_node( node );
    }

    LeaveCriticalSection( &pnp_cs );
}

static NTSTATUS create_pdo( struct usb_node *node )
{
    struct pdo_ext *ext;
    NTSTATUS status;

    if ((status = IoCreateDevice( bus_fdo->DriverObject, sizeof(*ext), NULL,
                                  node->composite ? FILE_DEVICE_BUS_EXTENDER : FILE_DEVICE_SERIAL_PORT,
                                  FILE_AUTOGENERATED_DEVICE_NAME, FALSE, &node->pdo )))
    {
        ERR( "failed to create the PDO of %s\\%s, status %#lx\n", debugstr_w(node->device_id),
             debugstr_w(node->instance_id), status );
        return status;
    }
    ext = node->pdo->DeviceExtension;
    memset( ext, 0, sizeof(*ext) );
    ext->node = node;
    node->pdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/* build the devices of the bus, or of a composite device */
static NTSTATUS query_bus_relations( struct usb_node *parent, DEVICE_RELATIONS **ret )
{
    DEVICE_RELATIONS *relations;
    struct usb_node *node;
    ULONG count = 0;

    EnterCriticalSection( &pnp_cs );

    LIST_FOR_EACH_ENTRY( node, &usb_nodes, struct usb_node, entry )
        if (node->parent == parent) count++;

    if (!(relations = ExAllocatePool( PagedPool, offsetof( DEVICE_RELATIONS, Objects[count] ) )))
    {
        LeaveCriticalSection( &pnp_cs );
        return STATUS_NO_MEMORY;
    }
    relations->Count = 0;

    /* a composite device that isn't started reports nothing, it does once started again */
    if (!parent || parent->started)
    {
        LIST_FOR_EACH_ENTRY( node, &usb_nodes, struct usb_node, entry )
        {
            if (node->parent != parent || !node->present || node->replace) continue;
            if (!node->pdo && create_pdo( node )) continue;
            call_fastcall_func1( ObfReferenceObject, node->pdo );
            relations->Objects[relations->Count++] = node->pdo;
        }
    }

    LeaveCriticalSection( &pnp_cs );

    TRACE( "reporting %lu devices\n", relations->Count );
    *ret = relations;
    return STATUS_SUCCESS;
}

/* tell the windows that a COM port arrived or was removed */
static void broadcast_port_change( int port, UINT code )
{
    DEV_BROADCAST_PORT_W *info;
    WCHAR name[16];
    DWORD size;
    int len;

    len = swprintf( name, ARRAY_SIZE(name), L"COM%u", port );
    size = offsetof( DEV_BROADCAST_PORT_W, dbcp_name[len + 1] );
    if (!(info = calloc( 1, size ))) return;
    info->dbcp_size = size;
    info->dbcp_devicetype = DBT_DEVTYP_PORT;
    wcscpy( info->dbcp_name, name );
    BroadcastSystemMessageW( BSF_FORCEIFHUNG | BSF_QUERY, NULL, WM_DEVICECHANGE, code, (LPARAM)info );
    free( info );
}

static void disable_port_interface( struct pdo_ext *ext )
{
    if (!ext->enabled) return;
    IoSetDeviceInterfaceState( &ext->interface, FALSE );
    ext->enabled = FALSE;
    broadcast_port_change( ext->port, DBT_DEVICEREMOVECOMPLETE );
}

static NTSTATUS query_id( struct usb_node *node, BUS_QUERY_ID_TYPE type, ULONG_PTR *info )
{
    WCHAR *id;

    switch (type)
    {
    case BusQueryDeviceID:     id = pool_strdup( node->device_id, FALSE ); break;
    case BusQueryInstanceID:   id = pool_strdup( node->instance_id, FALSE ); break;
    case BusQueryHardwareIDs:  id = pool_strdup( node->hardware_ids, TRUE ); break;
    case BusQueryCompatibleIDs: id = pool_strdup( node->compatible_ids, TRUE ); break;
    default: return STATUS_NOT_SUPPORTED;
    }
    *info = (ULONG_PTR)id;
    return id ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}

static NTSTATUS pdo_pnp( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    struct pdo_ext *ext = device->DeviceExtension;
    NTSTATUS status = irp->IoStatus.Status;
    DEVICE_CAPABILITIES *caps;
    struct usb_node *node;
    BOOL composite = FALSE;
    int port = 0, home = 0;

    TRACE( "device %p, minor function %#x\n", device, stack->MinorFunction );

    EnterCriticalSection( &pnp_cs );
    if ((node = ext->node))
    {
        composite = node->composite;
        port = node->port;
        home = node->home;
    }
    LeaveCriticalSection( &pnp_cs );

    /* a port whose PDO was removed may still be told to go away */
    if (!node)
    {
        if (stack->MinorFunction == IRP_MN_SURPRISE_REMOVAL || stack->MinorFunction == IRP_MN_REMOVE_DEVICE)
            status = STATUS_SUCCESS;
        else if (stack->MinorFunction != IRP_MN_QUERY_DEVICE_RELATIONS)
            status = STATUS_NO_SUCH_DEVICE;
        irp->IoStatus.Status = status;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
        return status;
    }

    switch (stack->MinorFunction)
    {
    case IRP_MN_QUERY_ID:
        /* the IDs never change, so they can be read without the lock */
        if ((status = query_id( node, stack->Parameters.QueryId.IdType, &irp->IoStatus.Information )) == STATUS_NOT_SUPPORTED)
            status = irp->IoStatus.Status;
        break;

    case IRP_MN_QUERY_DEVICE_TEXT:
        if (stack->Parameters.QueryDeviceText.DeviceTextType == DeviceTextDescription)
        {
            irp->IoStatus.Information = (ULONG_PTR)pool_strdup( node->description, FALSE );
            status = irp->IoStatus.Information ? STATUS_SUCCESS : STATUS_NO_MEMORY;
        }
        break;

    case IRP_MN_QUERY_CAPABILITIES:
        caps = stack->Parameters.DeviceCapabilities.Capabilities;
        /* the instance IDs are unique already, and the ports need no function driver */
        caps->UniqueID = TRUE;
        caps->RawDeviceOK = TRUE;
        caps->SurpriseRemovalOK = TRUE;
        caps->Removable = !node->parent;
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (composite && stack->Parameters.QueryDeviceRelations.Type == BusRelations)
            status = query_bus_relations( node, (DEVICE_RELATIONS **)&irp->IoStatus.Information );
        break;

    case IRP_MN_START_DEVICE:
        if (composite)
        {
            /* marked so that it is hidden when mountmgr starts, along with the ports */
            set_node_present( node, TRUE );
            EnterCriticalSection( &pnp_cs );
            node->started = TRUE;
            IoInvalidateDeviceRelations( device, BusRelations );
            LeaveCriticalSection( &pnp_cs );
        }
        else if (!ext->enabled)
        {
            set_port_entry( node, port, home );
            if (!ext->interface.Buffer &&
                (status = IoRegisterDeviceInterface( device, &comport_guid, NULL, &ext->interface )))
                ERR( "failed to register the interface of COM%u, status %#lx\n", port, status );
            else if ((status = IoSetDeviceInterfaceState( &ext->interface, TRUE )))
                ERR( "failed to enable the interface of COM%u, status %#lx\n", port, status );
            else
            {
                ext->enabled = TRUE;
                ext->port = port;
                broadcast_port_change( port, DBT_DEVICEARRIVAL );
            }
        }
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        if (composite)
        {
            EnterCriticalSection( &pnp_cs );
            node->started = FALSE;
            LeaveCriticalSection( &pnp_cs );
        }
        else disable_port_interface( ext );
        status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        if (composite)
        {
            /* the PDO is kept, an invalidation of it may still be queued */
            EnterCriticalSection( &pnp_cs );
            node->started = FALSE;
            LeaveCriticalSection( &pnp_cs );
            status = STATUS_SUCCESS;
            break;
        }

        disable_port_interface( ext );
        RtlFreeUnicodeString( &ext->interface );

        EnterCriticalSection( &pnp_cs );
        node->pdo = NULL;
        node->replace = FALSE;
        ext->node = NULL;
        /* a port that came back before its PDO was removed gets a new one */
        if (node->present) invalidate_parent( node );
        else free_usb_node( node );
        LeaveCriticalSection( &pnp_cs );

        irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest( irp, IO_NO_INCREMENT );
        IoDeleteDevice( device );
        return STATUS_SUCCESS;

    default:
        break;
    }

    irp->IoStatus.Status = status;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

static NTSTATUS fdo_pnp( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    DEVICE_OBJECT *lower = bus_pdo;
    NTSTATUS status;

    TRACE( "minor function %#x\n", stack->MinorFunction );

    switch (stack->MinorFunction)
    {
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (stack->Parameters.QueryDeviceRelations.Type == BusRelations)
            irp->IoStatus.Status = query_bus_relations( NULL, (DEVICE_RELATIONS **)&irp->IoStatus.Information );
        break;

    case IRP_MN_START_DEVICE:
        EnterCriticalSection( &pnp_cs );
        bus_started = TRUE;
        /* report the ports created before the bus started */
        invalidate_bus();
        LeaveCriticalSection( &pnp_cs );
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_SURPRISE_REMOVAL:
        irp->IoStatus.Status = STATUS_SUCCESS;
        break;

    case IRP_MN_REMOVE_DEVICE:
        EnterCriticalSection( &pnp_cs );
        bus_started = FALSE;
        bus_fdo = bus_pdo = NULL;
        LeaveCriticalSection( &pnp_cs );

        irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation( irp );
        status = IoCallDriver( lower, irp );
        IoDetachDevice( lower );
        IoDeleteDevice( device );
        return status;

    default:
        break;
    }

    IoSkipCurrentIrpStackLocation( irp );
    return IoCallDriver( lower, irp );
}

NTSTATUS WINAPI serial_bus_pnp( DEVICE_OBJECT *device, IRP *irp )
{
    if (device == bus_fdo) return fdo_pnp( device, irp );
    return pdo_pnp( device, irp );
}

NTSTATUS WINAPI serial_bus_add_device( DRIVER_OBJECT *driver, DEVICE_OBJECT *pdo )
{
    DEVICE_OBJECT *fdo;
    NTSTATUS status;

    TRACE( "driver %p, pdo %p\n", driver, pdo );

    if ((status = IoCreateDevice( driver, 0, NULL, FILE_DEVICE_BUS_EXTENDER, 0, FALSE, &fdo )))
    {
        ERR( "failed to create the bus device, status %#lx\n", status );
        return status;
    }
    IoAttachDeviceToDeviceStack( fdo, pdo );

    EnterCriticalSection( &pnp_cs );
    bus_fdo = fdo;
    bus_pdo = pdo;
    LeaveCriticalSection( &pnp_cs );

    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
