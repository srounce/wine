/*
 * Dynamic devices support
 *
 * Copyright 2006 Alexandre Julliard
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
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
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

#define MAX_DOS_DRIVES 26
#define MAX_PORTS 256

static const WCHAR drive_types[][8] =
{
    L"",        /* DEVICE_UNKNOWN */
    L"",        /* DEVICE_HARDDISK */
    L"hd",      /* DEVICE_HARDDISK_VOL */
    L"floppy",  /* DEVICE_FLOPPY */
    L"cdrom",   /* DEVICE_CDROM */
    L"cdrom",   /* DEVICE_DVD */
    L"network", /* DEVICE_NETWORK */
    L"ramdisk"  /* DEVICE_RAMDISK */
};

enum fs_type
{
    FS_ERROR,    /* error accessing the device */
    FS_UNKNOWN,  /* unknown file system */
    FS_FAT1216,
    FS_FAT32,
    FS_ISO9660,
    FS_UDF       /* For reference [E] = Ecma-167.pdf, [U] = udf260.pdf */
};

/* struct disk_device represents any drive or filesystem (partition).
 * It differs from struct volume in two respects:
 *
 * - struct volume *only* represents filesystems. A whole physical drive has
 *   only a struct disk_device. Anything with an actual filesystem has both a
 *   struct disk_device and a struct volume, linked to each other.
 *
 * - struct disk_device is allocated as the DeviceExtension for the ntoskrnl
 *   device object. struct volume is allocated by us and we expect the pointer
 *   to remain stable in various places.
 *   Note that we can recreate the NT device in response to
 *   IOCTL_MOUNTMGR_DEFINE_UNIX_DRIVE if the type of device is changed
 *   (e.g. from hard disk to CD-ROM, which necessitates a differently
 *   named NT device), which means that the disk_device is reallocated.
 */

struct disk_device
{
    enum device_type      type;        /* drive type */
    DEVICE_OBJECT        *dev_obj;     /* disk device allocated for this volume */
    UNICODE_STRING        name;        /* device name */
    UNICODE_STRING        symlink;     /* device symlink if any */
    STORAGE_DEVICE_NUMBER devnum;      /* device number info */
    char                 *unix_device; /* unix device path */
    char                 *disk_serial; /* disk serial number */
    struct volume        *volume;      /* associated volume */
};

struct volume
{
    struct list           entry;       /* entry in volumes list */
    struct disk_device   *device;      /* disk device */
    char                 *udi;         /* unique identifier for dynamic volumes */
    unsigned int          ref;         /* ref count */
    char                 *unix_mount;  /* unix mount point path */
    GUID                  guid;        /* volume uuid */
    struct mount_point   *mount;       /* Volume{xxx} mount point */
    WCHAR                 label[256];  /* volume label */
    DWORD                 serial;      /* volume serial number */
    enum fs_type          fs_type;     /* file system type */
};

struct dos_drive
{
    struct list           entry;       /* entry in drives list */
    struct volume        *volume;      /* volume for this drive */
    int                   drive;       /* drive letter (0 = A: etc.) */
    struct mount_point   *mount;       /* DosDevices mount point */
};

static struct list drives_list = LIST_INIT(drives_list);
static struct list volumes_list = LIST_INIT(volumes_list);

static DRIVER_OBJECT *disk_driver;
static DRIVER_OBJECT *serial_driver;
static DRIVER_OBJECT *parallel_driver;

static CRITICAL_SECTION device_section;
static CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &device_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": device_section") }
};
static CRITICAL_SECTION device_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static const GUID *get_default_uuid( int letter )
{
    static GUID guid;

    guid.Data4[7] = 'A' + letter;
    return &guid;
}

/* send notification about a change to a given drive */
static void send_notify( int drive, int code )
{
    DEV_BROADCAST_VOLUME info;

    info.dbcv_size       = sizeof(info);
    info.dbcv_devicetype = DBT_DEVTYP_VOLUME;
    info.dbcv_reserved   = 0;
    info.dbcv_unitmask   = 1 << drive;
    info.dbcv_flags      = DBTF_MEDIA;
    BroadcastSystemMessageW( BSF_FORCEIFHUNG|BSF_QUERY, NULL,
                             WM_DEVICECHANGE, code, (LPARAM)&info );
}

#define BLOCK_SIZE 2048
#define SUPERBLOCK_SIZE BLOCK_SIZE

#define CDFRAMES_PERSEC         75
#define CDFRAMES_PERMIN         (CDFRAMES_PERSEC * 60)
#define FRAME_OF_ADDR(a)        ((a)[1] * CDFRAMES_PERMIN + (a)[2] * CDFRAMES_PERSEC + (a)[3])
#define FRAME_OF_TOC(toc, idx)  FRAME_OF_ADDR((toc)->TrackData[(idx) - (toc)->FirstTrack].Address)

#define GETWORD(buf,off)  MAKEWORD(buf[(off)],buf[(off+1)])
#define GETLONG(buf,off)  MAKELONG(GETWORD(buf,off),GETWORD(buf,off+2))

/* get the label by reading it from a file at the root of the filesystem */
static void get_filesystem_label( struct volume *volume )
{
    char buffer[256], *p;
    ULONG size = sizeof(buffer);
    struct read_volume_file_params params = { volume->unix_mount, ".windows-label", buffer, &size };

    volume->label[0] = 0;
    if (!volume->unix_mount) return;
    if (MOUNTMGR_CALL( read_volume_file, &params )) return;

    p = buffer + size;
    while (p > buffer && (p[-1] == ' ' || p[-1] == '\r' || p[-1] == '\n')) p--;
    *p = 0;
    if (!MultiByteToWideChar( CP_UNIXCP, 0, buffer, -1, volume->label, ARRAY_SIZE(volume->label) ))
        volume->label[ARRAY_SIZE(volume->label) - 1] = 0;
}

/* get the serial number by reading it from a file at the root of the filesystem */
static void get_filesystem_serial( struct volume *volume )
{
    char buffer[32];
    ULONG size = sizeof(buffer);
    struct read_volume_file_params params = { volume->unix_mount, ".windows-serial", buffer, &size };

    volume->serial = 0;
    if (!volume->unix_mount) return;
    if (MOUNTMGR_CALL( read_volume_file, &params )) return;

    buffer[size] = 0;
    volume->serial = strtoul( buffer, NULL, 16 );
}


/******************************************************************
 *		VOLUME_FindCdRomDataBestVoldesc
 */
static DWORD VOLUME_FindCdRomDataBestVoldesc( HANDLE handle )
{
    BYTE cur_vd_type, max_vd_type = 0;
    BYTE buffer[0x800];
    DWORD size, offs, best_offs = 0, extra_offs = 0;

    for (offs = 0x8000; offs <= 0x9800; offs += 0x800)
    {
        /* if 'CDROM' occurs at position 8, this is a pre-iso9660 cd, and
         * the volume label is displaced forward by 8
         */
        if (SetFilePointer( handle, offs, NULL, FILE_BEGIN ) != offs) break;
        if (!ReadFile( handle, buffer, sizeof(buffer), &size, NULL )) break;
        if (size != sizeof(buffer)) break;
        /* check for non-ISO9660 signature */
        if (!memcmp( buffer + 11, "ROM", 3 )) extra_offs = 8;
        cur_vd_type = buffer[extra_offs];
        if (cur_vd_type == 0xff) /* voldesc set terminator */
            break;
        if (cur_vd_type > max_vd_type)
        {
            max_vd_type = cur_vd_type;
            best_offs = offs + extra_offs;
        }
    }
    return best_offs;
}


/***********************************************************************
 *           VOLUME_ReadFATSuperblock
 */
static enum fs_type VOLUME_ReadFATSuperblock( HANDLE handle, BYTE *buff )
{
    DWORD size;

    /* try a fixed disk, with a FAT partition */
    if (SetFilePointer( handle, 0, NULL, FILE_BEGIN ) != 0 ||
        !ReadFile( handle, buff, SUPERBLOCK_SIZE, &size, NULL ))
    {
        if (GetLastError() == ERROR_BAD_DEV_TYPE) return FS_UNKNOWN;  /* not a real device */
        return FS_ERROR;
    }

    if (size < SUPERBLOCK_SIZE) return FS_UNKNOWN;

    /* FIXME: do really all FAT have their name beginning with
     * "FAT" ? (At least FAT12, FAT16 and FAT32 have :)
     */
    if (!memcmp(buff+0x36, "FAT", 3) || !memcmp(buff+0x52, "FAT", 3))
    {
        /* guess which type of FAT we have */
        int reasonable;
        unsigned int sectors,
                     sect_per_fat,
                     total_sectors,
                     num_boot_sectors,
                     num_fats,
                     num_root_dir_ents,
                     bytes_per_sector,
                     sectors_per_cluster,
                     nclust;
        sect_per_fat = GETWORD(buff, 0x16);
        if (!sect_per_fat) sect_per_fat = GETLONG(buff, 0x24);
        total_sectors = GETWORD(buff, 0x13);
        if (!total_sectors)
            total_sectors = GETLONG(buff, 0x20);
        num_boot_sectors = GETWORD(buff, 0x0e);
        num_fats =  buff[0x10];
        num_root_dir_ents = GETWORD(buff, 0x11);
        bytes_per_sector = GETWORD(buff, 0x0b);
        sectors_per_cluster = buff[0x0d];
        /* check if the parameters are reasonable and will not cause
         * arithmetic errors in the calculation */
        reasonable = num_boot_sectors < total_sectors &&
                     num_fats < 16 &&
                     bytes_per_sector >= 512 && bytes_per_sector % 512 == 0 &&
                     sectors_per_cluster >= 1;
        if (!reasonable) return FS_UNKNOWN;
        sectors =  total_sectors - num_boot_sectors - num_fats * sect_per_fat -
            (num_root_dir_ents * 32 + bytes_per_sector - 1) / bytes_per_sector;
        nclust = sectors / sectors_per_cluster;
        if ((buff[0x42] == 0x28 || buff[0x42] == 0x29) &&
                !memcmp(buff+0x52, "FAT", 3)) return FS_FAT32;
        if (nclust < 65525)
        {
            if ((buff[0x26] == 0x28 || buff[0x26] == 0x29) &&
                    !memcmp(buff+0x36, "FAT", 3))
                return FS_FAT1216;
        }
    }
    return FS_UNKNOWN;
}


/***********************************************************************
 *           VOLUME_ReadCDBlock
 */
static BOOL VOLUME_ReadCDBlock( HANDLE handle, BYTE *buff, INT offs )
{
    DWORD size, whence = offs >= 0 ? FILE_BEGIN : FILE_END;

    if (SetFilePointer( handle, offs, NULL, whence ) != offs ||
        !ReadFile( handle, buff, SUPERBLOCK_SIZE, &size, NULL ) ||
        size != SUPERBLOCK_SIZE)
        return FALSE;

    return TRUE;
}


/***********************************************************************
 *           VOLUME_ReadCDSuperblock
 */
static enum fs_type VOLUME_ReadCDSuperblock( HANDLE handle, BYTE *buff )
{
    int i;
    DWORD offs;

    /* Check UDF first as UDF and ISO9660 structures can coexist on the same medium
     *  Starting from sector 16, we may find :
     *  - a CD-ROM Volume Descriptor Set (ISO9660) containing one or more Volume Descriptors
     *  - an Extended Area (UDF) -- [E] 2/8.3.1 and [U] 2.1.7
     *  There is no explicit end so read 16 sectors and then give up */
    for( i=16; i<16+16; i++)
    {
        if (!VOLUME_ReadCDBlock(handle, buff, i*BLOCK_SIZE))
            continue;

        /* We are supposed to check "BEA01", "NSR0x" and "TEA01" IDs + verify tag checksum
         *  but we assume the volume is well-formatted */
        if (!memcmp(&buff[1], "BEA01", 5)) return FS_UDF;
    }

    offs = VOLUME_FindCdRomDataBestVoldesc( handle );
    if (!offs) return FS_UNKNOWN;

    if (!VOLUME_ReadCDBlock(handle, buff, offs))
        return FS_ERROR;

    /* check for the iso9660 identifier */
    if (!memcmp(&buff[1], "CD001", 5)) return FS_ISO9660;
    return FS_UNKNOWN;
}


/**************************************************************************
 *                        UDF_Find_PVD
 * Find the Primary Volume Descriptor
 */
static BOOL UDF_Find_PVD( HANDLE handle, BYTE pvd[] )
{
    unsigned int i;
    DWORD offset;
    INT locations[] = { 256, -1, -257, 512 };

    for(i=0; i<ARRAY_SIZE(locations); i++)
    {
        if (!VOLUME_ReadCDBlock(handle, pvd, locations[i]*BLOCK_SIZE))
            return FALSE;

        /* Tag Identifier of Anchor Volume Descriptor Pointer is 2 -- [E] 3/10.2.1 */
        if (pvd[0]==2 && pvd[1]==0)
        {
            /* Tag location (Uint32) at offset 12, little-endian */
            offset  = pvd[20 + 0];
            offset |= pvd[20 + 1] << 8;
            offset |= pvd[20 + 2] << 16;
            offset |= pvd[20 + 3] << 24;
            offset *= BLOCK_SIZE;

            if (!VOLUME_ReadCDBlock(handle, pvd, offset))
                return FALSE;

            /* Check for the Primary Volume Descriptor Tag Id -- [E] 3/10.1.1 */
            if (pvd[0]!=1 || pvd[1]!=0)
                return FALSE;

            /* 8 or 16 bits per character -- [U] 2.1.1 */
            if (!(pvd[24]==8 || pvd[24]==16))
                return FALSE;

            return TRUE;
        }
    }

    return FALSE;
}


/**************************************************************************
 *                              VOLUME_GetSuperblockLabel
 */
static void VOLUME_GetSuperblockLabel( struct volume *volume, HANDLE handle, const BYTE *superblock )
{
    const BYTE *label_ptr = NULL;
    DWORD label_len;
    BYTE pvd[BLOCK_SIZE];

    switch (volume->fs_type)
    {
    case FS_ERROR:
        label_len = 0;
        break;
    case FS_UNKNOWN:
        get_filesystem_label( volume );
        return;
    case FS_FAT1216:
        label_ptr = superblock + 0x2b;
        label_len = 11;
        break;
    case FS_FAT32:
        label_ptr = superblock + 0x47;
        label_len = 11;
        break;
    case FS_ISO9660:
        {
            BYTE ver = superblock[0x5a];

            if (superblock[0x58] == 0x25 && superblock[0x59] == 0x2f &&  /* Unicode ID */
                ((ver == 0x40) || (ver == 0x43) || (ver == 0x45)))
            { /* yippee, unicode */
                unsigned int i;

                for (i = 0; i < 16; i++)
                    volume->label[i] = (superblock[40+2*i] << 8) | superblock[41+2*i];
                volume->label[i] = 0;
                while (i && volume->label[i-1] == ' ') volume->label[--i] = 0;
                return;
            }
            label_ptr = superblock + 40;
            label_len = 32;
            break;
        }
    case FS_UDF:
        if(!UDF_Find_PVD(handle, pvd))
        {
            label_len = 0;
            break;
        }

        /* [E] 3/10.1.4 and [U] 2.1.1 */
        if(pvd[24]==8)
        {
            label_ptr = pvd + 24 + 1;
            label_len = pvd[24+32-1];
            break;
        }
        else
        {
            unsigned int i;

            label_len = 1 + pvd[24+32-1];
            for (i = 0; i < label_len; i += 2)
                volume->label[i/2] = (pvd[24+1+i] << 8) | pvd[24+1+i+1];
            volume->label[label_len] = 0;
            return;
        }
    }
    if (label_len) RtlMultiByteToUnicodeN( volume->label, sizeof(volume->label) - sizeof(WCHAR),
                                           &label_len, (const char *)label_ptr, label_len );
    label_len /= sizeof(WCHAR);
    volume->label[label_len] = 0;
    while (label_len && volume->label[label_len-1] == ' ') volume->label[--label_len] = 0;
}


/**************************************************************************
 *                              UDF_Find_FSD_Sector
 * Find the File Set Descriptor used to compute the serial of a UDF volume
 */
static int UDF_Find_FSD_Sector( HANDLE handle, BYTE block[] )
{
    int i, PVD_sector, PD_sector, PD_length;

    if(!UDF_Find_PVD(handle,block))
        goto default_sector;

    /* Retrieve the tag location of the PVD -- [E] 3/7.2 */
    PVD_sector  = block[12 + 0];
    PVD_sector |= block[12 + 1] << 8;
    PVD_sector |= block[12 + 2] << 16;
    PVD_sector |= block[12 + 3] << 24;

    /* Find the Partition Descriptor */
    for(i=PVD_sector+1; ; i++)
    {
        if(!VOLUME_ReadCDBlock(handle, block, i*BLOCK_SIZE))
            goto default_sector;

        /* Partition Descriptor Tag Id -- [E] 3/10.5.1 */
        if(block[0]==5 && block[1]==0)
            break;

        /* Terminating Descriptor Tag Id -- [E] 3/10.9.1 */
        if(block[0]==8 && block[1]==0)
            goto default_sector;
    }

    /* Find the partition starting location -- [E] 3/10.5.8 */
    PD_sector  = block[188 + 0];
    PD_sector |= block[188 + 1] << 8;
    PD_sector |= block[188 + 2] << 16;
    PD_sector |= block[188 + 3] << 24;

    /* Find the partition length -- [E] 3/10.5.9 */
    PD_length  = block[192 + 0];
    PD_length |= block[192 + 1] << 8;
    PD_length |= block[192 + 2] << 16;
    PD_length |= block[192 + 3] << 24;

    for(i=PD_sector; i<PD_sector+PD_length; i++)
    {
        if(!VOLUME_ReadCDBlock(handle, block, i*BLOCK_SIZE))
            goto default_sector;

        /* File Set Descriptor Tag Id -- [E] 3/14.1.1 */
        if(block[0]==0 && block[1]==1)
            return i;
    }

default_sector:
    WARN("FSD sector not found, serial may be incorrect\n");
    return 257;
}


/**************************************************************************
 *                              VOLUME_GetSuperblockSerial
 */
static void VOLUME_GetSuperblockSerial( struct volume *volume, HANDLE handle, const BYTE *superblock )
{
    int FSD_sector;
    BYTE block[BLOCK_SIZE];

    switch (volume->fs_type)
    {
    case FS_ERROR:
        break;
    case FS_UNKNOWN:
        get_filesystem_serial( volume );
        break;
    case FS_FAT1216:
        volume->serial = GETLONG( superblock, 0x27 );
        break;
    case FS_FAT32:
        volume->serial = GETLONG( superblock, 0x43 );
        break;
    case FS_UDF:
        FSD_sector = UDF_Find_FSD_Sector(handle, block);
        if (!VOLUME_ReadCDBlock(handle, block, FSD_sector*BLOCK_SIZE))
            break;
        superblock = block;
        /* fallthrough */
    case FS_ISO9660:
        {
            BYTE sum[4];
            int i;

            sum[0] = sum[1] = sum[2] = sum[3] = 0;
            for (i = 0; i < 2048; i += 4)
            {
                /* DON'T optimize this into DWORD !! (breaks overflow) */
                sum[0] += superblock[i+0];
                sum[1] += superblock[i+1];
                sum[2] += superblock[i+2];
                sum[3] += superblock[i+3];
            }
            /*
             * OK, another braindead one... argh. Just believe it.
             * Me$$ysoft chose to reverse the serial number in NT4/W2K.
             * It's true and nobody will ever be able to change it.
             */
            if ((GetVersion() & 0x80000000) || volume->fs_type == FS_UDF)
                volume->serial = (sum[3] << 24) | (sum[2] << 16) | (sum[1] << 8) | sum[0];
            else
                volume->serial = (sum[0] << 24) | (sum[1] << 16) | (sum[2] << 8) | sum[3];
        }
    }
}


/**************************************************************************
 *                              VOLUME_GetAudioCDSerial
 */
static DWORD VOLUME_GetAudioCDSerial( const CDROM_TOC *toc )
{
    DWORD serial = 0;
    int i;

    for (i = 0; i <= toc->LastTrack - toc->FirstTrack; i++)
        serial += ((toc->TrackData[i].Address[1] << 16) |
                   (toc->TrackData[i].Address[2] << 8) |
                   toc->TrackData[i].Address[3]);

    /*
     * dwStart, dwEnd collect the beginning and end of the disc respectively, in
     * frames.
     * There it is collected for correcting the serial when there are less than
     * 3 tracks.
     */
    if (toc->LastTrack - toc->FirstTrack + 1 < 3)
    {
        DWORD dwStart = FRAME_OF_TOC(toc, toc->FirstTrack);
        DWORD dwEnd = FRAME_OF_TOC(toc, toc->LastTrack + 1);
        serial += dwEnd - dwStart;
    }
    return serial;
}


/* create the disk device for a given volume */
static NTSTATUS create_disk_device( enum device_type type, struct disk_device **device_ret, struct volume *volume )
{
    UINT i, first = 0;
    NTSTATUS status = 0;
    const WCHAR *format = NULL;
    const WCHAR *link_format = NULL;
    UNICODE_STRING name;
    DEVICE_OBJECT *dev_obj;
    struct disk_device *device;
    DEVICE_TYPE nt_type;
    ULONG characteristics = FILE_DEVICE_IS_MOUNTED;

    switch(type)
    {
    case DEVICE_UNKNOWN:
    case DEVICE_HARDDISK:
    case DEVICE_NETWORK:  /* FIXME */
        format = L"\\Device\\Harddisk%u";
        link_format = L"\\??\\PhysicalDrive%u";
        nt_type = FILE_DEVICE_DISK;
        break;
    case DEVICE_HARDDISK_VOL:
        format = L"\\Device\\HarddiskVolume%u";
        nt_type = FILE_DEVICE_DISK;
        first = 1;  /* harddisk volumes start counting from 1 */
        break;
    case DEVICE_FLOPPY:
        format = L"\\Device\\Floppy%u";
        nt_type = FILE_DEVICE_DISK;
        characteristics |= FILE_REMOVABLE_MEDIA;
        break;
    case DEVICE_CDROM:
        format = L"\\Device\\CdRom%u";
        link_format = L"\\??\\CdRom%u";
        nt_type = FILE_DEVICE_CD_ROM;
        characteristics |= FILE_REMOVABLE_MEDIA | FILE_READ_ONLY_DEVICE;
        break;
    case DEVICE_DVD:
        format = L"\\Device\\CdRom%u";
        link_format = L"\\??\\CdRom%u";
        nt_type = FILE_DEVICE_DVD;
        break;
    case DEVICE_RAMDISK:
        format = L"\\Device\\Ramdisk%u";
        nt_type = FILE_DEVICE_DISK;
        break;
    }

    name.MaximumLength = (lstrlenW(format) + 10) * sizeof(WCHAR);
    name.Buffer = RtlAllocateHeap( GetProcessHeap(), 0, name.MaximumLength );
    for (i = first; i < 32; i++)
    {
        swprintf( name.Buffer, name.MaximumLength / sizeof(WCHAR), format, i );
        name.Length = lstrlenW(name.Buffer) * sizeof(WCHAR);
        status = IoCreateDevice( disk_driver, sizeof(*device), &name, nt_type, characteristics, FALSE, &dev_obj );
        if (status != STATUS_OBJECT_NAME_COLLISION) break;
    }
    if (!status)
    {
        device = dev_obj->DeviceExtension;
        device->dev_obj        = dev_obj;
        device->name           = name;
        device->type           = type;
        device->unix_device    = NULL;
        device->symlink.Buffer = NULL;
        device->volume         = volume;

        if (link_format)
        {
            UNICODE_STRING symlink;

            symlink.MaximumLength = (lstrlenW(link_format) + 10) * sizeof(WCHAR);
            if ((symlink.Buffer = RtlAllocateHeap( GetProcessHeap(), 0, symlink.MaximumLength )))
            {
                swprintf( symlink.Buffer, symlink.MaximumLength / sizeof(WCHAR), link_format, i );
                symlink.Length = lstrlenW(symlink.Buffer) * sizeof(WCHAR);
                if (!IoCreateSymbolicLink( &symlink, &name )) device->symlink = symlink;
            }
        }

        device->devnum.DeviceType = nt_type;

        switch (type)
        {
        case DEVICE_FLOPPY:
        case DEVICE_RAMDISK:
            device->devnum.DeviceNumber = i;
            device->devnum.PartitionNumber = ~0u;
            break;
        case DEVICE_CDROM:
            device->devnum.DeviceNumber = i;
            device->devnum.PartitionNumber = ~0u;
            break;
        case DEVICE_DVD:
            device->devnum.DeviceNumber = i;
            device->devnum.PartitionNumber = ~0u;
            break;
        case DEVICE_UNKNOWN:
        case DEVICE_HARDDISK:
        case DEVICE_NETWORK:  /* FIXME */
            device->devnum.DeviceNumber = i;
            device->devnum.PartitionNumber = 0;
            break;
        case DEVICE_HARDDISK_VOL:
            device->devnum.DeviceNumber = 0;
            device->devnum.PartitionNumber = i;
            break;
        }
        *device_ret = device;
        TRACE( "created device %s\n", debugstr_w(name.Buffer) );
    }
    else
    {
        FIXME( "IoCreateDevice %s got %lx\n", debugstr_w(name.Buffer), status );
        RtlFreeUnicodeString( &name );
    }
    return status;
}

/* delete the disk device for a given drive */
static void delete_disk_device( struct disk_device *device )
{
    TRACE( "deleting device %s\n", debugstr_w(device->name.Buffer) );
    if (device->symlink.Buffer)
    {
        IoDeleteSymbolicLink( &device->symlink );
        RtlFreeUnicodeString( &device->symlink );
    }
    free( device->unix_device );
    free( device->disk_serial );
    RtlFreeUnicodeString( &device->name );
    IoDeleteDevice( device->dev_obj );
}

/* grab another reference to a volume */
static struct volume *grab_volume( struct volume *volume )
{
    volume->ref++;
    return volume;
}

/* release a volume and delete the corresponding disk device when refcount is 0 */
static unsigned int release_volume( struct volume *volume )
{
    unsigned int ret = --volume->ref;

    if (!ret)
    {
        TRACE( "%s udi %s\n", debugstr_guid(&volume->guid), debugstr_a(volume->udi) );
        assert( !volume->udi );
        list_remove( &volume->entry );
        if (volume->mount) delete_mount_point( volume->mount );
        delete_disk_device( volume->device );
        free( volume->unix_mount );
        free( volume );
    }
    return ret;
}

/* set the volume udi */
static void set_volume_udi( struct volume *volume, const char *udi )
{
    if (udi)
    {
        assert( !volume->udi );
        /* having a udi means the HAL side holds an extra reference */
        if ((volume->udi = strdup( udi ))) grab_volume( volume );
    }
    else if (volume->udi)
    {
        free( volume->udi );
        volume->udi = NULL;
        release_volume( volume );
    }
}

/* create a disk volume */
static NTSTATUS create_volume( const char *udi, enum device_type type, struct volume **volume_ret )
{
    struct volume *volume;
    NTSTATUS status;

    if (!(volume = calloc( 1, sizeof(*volume) )))
        return STATUS_NO_MEMORY;

    if (!(status = create_disk_device( type, &volume->device, volume )))
    {
        if (udi) set_volume_udi( volume, udi );
        list_add_tail( &volumes_list, &volume->entry );
        *volume_ret = grab_volume( volume );
    }
    else free( volume );

    return status;
}

/* create the disk device for a given volume */
static NTSTATUS create_dos_device( struct volume *volume, const char *udi, int letter,
                                   enum device_type type, struct dos_drive **drive_ret )
{
    struct dos_drive *drive;
    NTSTATUS status;

    if (!(drive = malloc( sizeof(*drive) ))) return STATUS_NO_MEMORY;
    drive->drive = letter;
    drive->mount = NULL;

    if (volume)
    {
        if (udi) set_volume_udi( volume, udi );
        drive->volume = grab_volume( volume );
        status = STATUS_SUCCESS;
    }
    else status = create_volume( udi, type, &drive->volume );

    if (status == STATUS_SUCCESS)
    {
        list_add_tail( &drives_list, &drive->entry );
        *drive_ret = drive;
    }
    else free( drive );

    return status;
}

/* delete the disk device for a given drive */
static void delete_dos_device( struct dos_drive *drive )
{
    list_remove( &drive->entry );
    if (drive->mount) delete_mount_point( drive->mount );
    release_volume( drive->volume );
    free( drive );
}

/* find a volume that matches the parameters */
static struct volume *find_matching_volume( const char *udi, const char *unix_device,
                                            const char *mount_point, enum device_type type )
{
    struct volume *volume;
    struct disk_device *disk_device;

    LIST_FOR_EACH_ENTRY( volume, &volumes_list, struct volume, entry )
    {
        int match = 0;

        /* when we have a udi we only match drives added manually */
        if (udi && volume->udi) continue;
        /* and when we don't have a udi we only match dynamic drives */
        if (!udi && !volume->udi) continue;

        disk_device = volume->device;
        if (disk_device->type != type) continue;
        if (unix_device && disk_device->unix_device)
        {
            if (strcmp( unix_device, disk_device->unix_device )) continue;
            match++;
        }
        if (mount_point && volume->unix_mount)
        {
            if (strcmp( mount_point, volume->unix_mount )) continue;
            match++;
        }
        if (!match) continue;
        TRACE( "found matching volume %s for device %s mount %s type %u\n",
               debugstr_guid(&volume->guid), debugstr_a(unix_device), debugstr_a(mount_point), type );
        return grab_volume( volume );
    }
    return NULL;
}

static BOOL get_volume_device_info( struct volume *volume )
{
    const char *unix_device = volume->device->unix_device;
    WCHAR *name;
    HANDLE handle;
    CDROM_TOC toc;
    DWORD size;
    BYTE superblock[SUPERBLOCK_SIZE];

    if (!unix_device)
        return FALSE;

    if (MOUNTMGR_CALL( check_device_access, volume->device->unix_device )) return FALSE;

    size = 8 + strlen(unix_device) + 1;
    if (!(name = malloc( size * sizeof(WCHAR )))) return FALSE;
    wcscpy( name, L"\\\\?\\unix" );
    MultiByteToWideChar( CP_UNIXCP, 0, unix_device, -1, name + 8, size - 8 );
    handle = CreateFileW( name, GENERIC_READ | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          NULL, OPEN_EXISTING, 0, 0 );
    free( name );
    if (handle == INVALID_HANDLE_VALUE)
    {
        WARN("Failed to open %s, err %lu\n", debugstr_a(unix_device), GetLastError());
        return FALSE;
    }
    if (DeviceIoControl( handle, IOCTL_CDROM_READ_TOC, NULL, 0, &toc, sizeof(toc), &size, 0 ))
    {
        if (!(toc.TrackData[0].Control & 0x04))  /* audio track */
        {
            TRACE( "%s: found audio CD\n", debugstr_a(unix_device) );
            wcscpy( volume->label, L"Audio CD" );
            volume->serial = VOLUME_GetAudioCDSerial( &toc );
            volume->fs_type = FS_ISO9660;
            CloseHandle( handle );
            return TRUE;
        }
        volume->fs_type = VOLUME_ReadCDSuperblock( handle, superblock );
    }
    else
    {
        if(GetLastError() == ERROR_NOT_READY)
        {
            TRACE( "%s: removable drive with no inserted media\n", debugstr_a(unix_device) );
            volume->fs_type = FS_UNKNOWN;
            CloseHandle( handle );
            return TRUE;
        }

        volume->fs_type = VOLUME_ReadFATSuperblock( handle, superblock );
        if (volume->fs_type == FS_UNKNOWN) volume->fs_type = VOLUME_ReadCDSuperblock( handle, superblock );
    }

    TRACE( "%s: found fs type %d\n", debugstr_a(unix_device), volume->fs_type );
    if (volume->fs_type == FS_ERROR)
    {
        CloseHandle( handle );
        return FALSE;
    }

    VOLUME_GetSuperblockLabel( volume, handle, superblock );
    VOLUME_GetSuperblockSerial( volume, handle, superblock );

    CloseHandle( handle );
    return TRUE;
}

/* set disk serial for dos devices that reside on a given Unix device */
static void set_dos_devices_disk_serial( struct volume *volume )
{
    unsigned int devices;
    struct dos_drive *drive;
    struct get_volume_dos_devices_params params = { volume->unix_mount, &devices };

    if (!volume->device->disk_serial || !volume->unix_mount || MOUNTMGR_CALL( get_volume_dos_devices, &params ))
        return;

    LIST_FOR_EACH_ENTRY( drive, &drives_list, struct dos_drive, entry )
    {
        /* drives mapped to Unix devices already have serial set, if available */
        if (drive->volume->device->unix_device) continue;
        /* copy serial if drive resides on this Unix device */
        if (devices & (1 << drive->drive))
        {
            free( drive->volume->device->disk_serial );
            drive->volume->device->disk_serial = strdup( volume->device->disk_serial );
        }
    }
}

/* change the information for an existing volume */
static NTSTATUS set_volume_info( struct volume *volume, struct dos_drive *drive, const char *unix_device,
                                 const char *mount_point, enum device_type type, const GUID *guid,
                                 const char *label, const char *disk_serial )
{
    void *id = NULL;
    unsigned int id_len = 0;
    struct disk_device *disk_device = volume->device;
    NTSTATUS status;

    if (type != disk_device->type)
    {
        if ((status = create_disk_device( type, &disk_device, volume ))) return status;
        if (volume->mount)
        {
            delete_mount_point( volume->mount );
            volume->mount = NULL;
        }
        if (drive && drive->mount)
        {
            delete_mount_point( drive->mount );
            drive->mount = NULL;
        }
        delete_disk_device( volume->device );
        volume->device = disk_device;
    }
    else
    {
        free( disk_device->unix_device );
        free( disk_device->disk_serial );
    }
    disk_device->unix_device = strdup( unix_device );
    disk_device->disk_serial = strdup( disk_serial );

    free( volume->unix_mount );
    volume->unix_mount = strdup( mount_point );

    set_dos_devices_disk_serial( volume );

    if (!get_volume_device_info( volume ))
    {
        if (volume->device->type == DEVICE_CDROM)
            volume->fs_type = FS_ISO9660;
        else if (volume->device->type == DEVICE_DVD)
            volume->fs_type = FS_UDF;
        else
            volume->fs_type = FS_UNKNOWN;

        get_filesystem_label( volume );
        get_filesystem_serial( volume );
    }
    if (label && label[0] && !volume->label[0])
        RtlMultiByteToUnicodeN(volume->label, ARRAY_SIZE(volume->label), NULL, label, strlen(label)+1);

    TRACE("fs_type %#x, label %s, serial %08lx\n", volume->fs_type, debugstr_w(volume->label), volume->serial);

    if (guid && memcmp( &volume->guid, guid, sizeof(volume->guid) ))
    {
        volume->guid = *guid;
        if (volume->mount)
        {
            delete_mount_point( volume->mount );
            volume->mount = NULL;
        }
    }

    if (!volume->serial)
        memcpy(&volume->serial, &volume->guid.Data4[4], sizeof(DWORD));

    if (!volume->mount)
        volume->mount = add_volume_mount_point( disk_device->dev_obj, &disk_device->name, &volume->guid );
    if (drive && !drive->mount)
        drive->mount = add_dosdev_mount_point( disk_device->dev_obj, &disk_device->name, drive->drive );

    if (volume->unix_mount)
    {
        id = volume->unix_mount;
        id_len = strlen( volume->unix_mount ) + 1;
    }
    if (volume->mount) set_mount_point_id( volume->mount, id, id_len );
    if (drive && drive->mount) set_mount_point_id( drive->mount, id, id_len );

    return STATUS_SUCCESS;
}

/* change the drive letter or volume for an existing drive */
static void set_drive_info( struct dos_drive *drive, int letter, struct volume *volume )
{
    if (drive->drive != letter)
    {
        if (drive->mount) delete_mount_point( drive->mount );
        drive->mount = NULL;
        drive->drive = letter;
    }
    if (drive->volume != volume)
    {
        if (drive->mount) delete_mount_point( drive->mount );
        drive->mount = NULL;
        grab_volume( volume );
        release_volume( drive->volume );
        drive->volume = volume;
    }
}

/* create devices for mapped drives */
static void create_drive_devices(void)
{
    char dosdev[] = "a::";
    struct dos_drive *drive;
    struct volume *volume;
    unsigned int i;
    HKEY drives_key;
    enum device_type drive_type;
    WCHAR driveW[] = L"a:";

    if (RegOpenKeyW( HKEY_LOCAL_MACHINE, L"Software\\Wine\\Drives", &drives_key )) drives_key = 0;

    for (i = 0; i < MAX_DOS_DRIVES; i++)
    {
        char link[4096], unix_dev[4096];
        char *unix_device = NULL;
        struct get_dosdev_symlink_params params = { dosdev, link, sizeof(link) };

        dosdev[0] = 'a' + i;
        dosdev[2] = 0;
        if (MOUNTMGR_CALL( get_dosdev_symlink, &params )) continue;
        dosdev[2] = ':';
        params.dest = unix_dev;
        params.size = sizeof(unix_dev);
        if (!MOUNTMGR_CALL( get_dosdev_symlink, &params )) unix_device = unix_dev;

        drive_type = i < 2 ? DEVICE_FLOPPY : DEVICE_HARDDISK_VOL;
        if (drives_key)
        {
            WCHAR buffer[32];
            DWORD j, type, size = sizeof(buffer);

            driveW[0] = 'a' + i;
            if (!RegQueryValueExW( drives_key, driveW, NULL, &type, (BYTE *)buffer, &size ) &&
                type == REG_SZ)
            {
                for (j = 0; j < ARRAY_SIZE(drive_types); j++)
                    if (drive_types[j][0] && !wcsicmp( buffer, drive_types[j] ))
                    {
                        drive_type = j;
                        break;
                    }
                if (drive_type == DEVICE_FLOPPY && i >= 2) drive_type = DEVICE_HARDDISK;
            }
        }

        volume = find_matching_volume( NULL, unix_device, link, drive_type );
        if (!create_dos_device( volume, NULL, i, drive_type, &drive ))
        {
            /* don't reset uuid if we used an existing volume */
            const GUID *guid = volume ? NULL : get_default_uuid(i);
            set_volume_info( drive->volume, drive, unix_device, link, drive_type, guid, NULL, NULL );
        }
        if (volume) release_volume( volume );
    }
    RegCloseKey( drives_key );
}

/* fill in the "Logical Unit" key for a given SCSI address */
static void create_scsi_entry( struct volume *volume, const struct scsi_info *info )
{
    static UCHAR tape_no = 0;

    WCHAR dataW[50];
    DWORD sizeW;
    DWORD value;
    const char *data;
    HKEY scsi_key;
    HKEY port_key;
    HKEY bus_key;
    HKEY target_key;
    HKEY lun_key;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\Scsi", 0, KEY_READ|KEY_WRITE, &scsi_key )) return;

    swprintf( dataW, ARRAY_SIZE( dataW ), L"Scsi Port %d", info->addr.PortNumber );
    if (RegCreateKeyExW( scsi_key, dataW, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &port_key, NULL )) return;
    RegCloseKey( scsi_key );

    RtlMultiByteToUnicodeN( dataW, sizeof(dataW), &sizeW, info->driver, strlen(info->driver)+1);
    RegSetValueExW( port_key, L"Driver", 0, REG_SZ, (const BYTE *)dataW, sizeW );
    value = 10;
    RegSetValueExW( port_key, L"FirstBusTimeScanInMs", 0, REG_DWORD, (const BYTE *)&value, sizeof(value));

    value = 0;

    swprintf( dataW, ARRAY_SIZE( dataW ), L"Scsi Bus %d", info->addr.PathId );
    if (RegCreateKeyExW( port_key, dataW, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &bus_key, NULL )) return;
    RegCloseKey( port_key );

    swprintf( dataW, ARRAY_SIZE( dataW ), L"Initiator Id %d", info->init_id );
    if (RegCreateKeyExW( bus_key, dataW, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &target_key, NULL )) return;
    RegCloseKey( target_key );

    swprintf( dataW, ARRAY_SIZE( dataW ), L"Target Id %d", info->addr.TargetId );
    if (RegCreateKeyExW( bus_key, dataW, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &target_key, NULL )) return;
    RegCloseKey( bus_key );

    swprintf( dataW, ARRAY_SIZE( dataW ), L"Logical Unit Id %d", info->addr.Lun );
    if (RegCreateKeyExW( target_key, dataW, 0, NULL, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, NULL, &lun_key, NULL )) return;
    RegCloseKey( target_key );

    switch (info->type)
    {
    case SCSI_DISK_PERIPHERAL:              data = "DiskPeripheral"; break;
    case SCSI_TAPE_PERIPHERAL:              data = "TapePeripheral"; break;
    case SCSI_PRINTER_PERIPHERAL:           data = "PrinterPeripheral"; break;
    case SCSI_WORM_PERIPHERAL:              data = "WormPeripheral"; break;
    case SCSI_CDROM_PERIPHERAL:             data = "CdRomPeripheral"; break;
    case SCSI_SCANNER_PERIPHERAL:           data = "ScannerPeripheral"; break;
    case SCSI_OPTICAL_DISK_PERIPHERAL:      data = "OpticalDiskPeripheral"; break;
    case SCSI_MEDIUM_CHANGER_PERIPHERAL:    data = "MediumChangerPeripheral"; break;
    case SCSI_COMMS_PERIPHERAL:             data = "CommunicationsPeripheral"; break;
    case SCSI_ASC_GRAPHICS_PERIPHERAL:
    case SCSI_ASC_GRAPHICS2_PERIPHERAL:     data = "ASCPrePressGraphicsPeripheral"; break;
    case SCSI_ARRAY_PERIPHERAL:             data = "ArrayPeripheral"; break;
    case SCSI_ENCLOSURE_PERIPHERAL:         data = "EnclosurePeripheral"; break;
    case SCSI_REDUCED_DISK_PERIPHERAL:      data = "RBCPeripheral"; break;
    case SCSI_CARD_READER_PERIPHERAL:       data = "CardReaderPeripheral"; break;
    case SCSI_BRIDGE_PERIPHERAL:            data = "BridgePeripheral"; break;
    case SCSI_OBJECT_STORAGE_PERIPHERAL:    /* Object-based storage devices */
    case SCSI_DRIVE_CONTROLLER_PERIPHERAL:  /* Automation/drive controllers */
    case SCSI_REDUCED_CDROM_PERIPHERAL:     /* Reduced-commands MM devices */
    case SCSI_PROCESSOR_PERIPHERAL:         /* Processor devices (considered to be "Other" by Windows) */
    default:                                data = "OtherPeripheral"; break;
    }
    RtlMultiByteToUnicodeN( dataW, sizeof(dataW), &sizeW, data, strlen(data)+1);
    RegSetValueExW( lun_key, L"Type", 0, REG_SZ, (const BYTE *)dataW, sizeW );

    RtlMultiByteToUnicodeN( dataW, sizeof(dataW), &sizeW, info->model, strlen(info->model)+1);
    RegSetValueExW( lun_key, L"Identifier", 0, REG_SZ, (const BYTE *)dataW, sizeW );

    if (volume)
    {
        UNICODE_STRING *dev = &volume->device->name;
        WCHAR *buffer = wcschr( dev->Buffer+1, '\\' ) + 1;
        ULONG length = dev->Length - (buffer - dev->Buffer)*sizeof(WCHAR);
        RegSetValueExW( lun_key, L"DeviceName", 0, REG_SZ, (const BYTE *)buffer, length );
    }
    else if (info->type == SCSI_TAPE_PERIPHERAL)
    {
        swprintf( dataW, ARRAY_SIZE( dataW ), L"Tape%d", tape_no++ );
        RegSetValueExW( lun_key, L"DeviceName", 0, REG_SZ, (const BYTE *)dataW, lstrlenW( dataW ) );
    }

    RegCloseKey( lun_key );
}

/* create a new disk volume */
NTSTATUS add_volume( const char *udi, const char *unix_device, const char *mount_point,
                     enum device_type type, const GUID *guid, const char *disk_serial,
                     const char *label, const struct scsi_info *scsi_info )
{
    struct volume *volume;
    NTSTATUS status = STATUS_SUCCESS;

    TRACE( "adding %s device %s mount %s type %u uuid %s\n", debugstr_a(udi),
           debugstr_a(unix_device), debugstr_a(mount_point), type, debugstr_guid(guid) );

    EnterCriticalSection( &device_section );
    LIST_FOR_EACH_ENTRY( volume, &volumes_list, struct volume, entry )
        if (volume->udi && !strcmp( udi, volume->udi ))
        {
            grab_volume( volume );
            goto found;
        }

    /* udi not found, search for a non-dynamic volume */
    if ((volume = find_matching_volume( udi, unix_device, mount_point, type ))) set_volume_udi( volume, udi );
    else status = create_volume( udi, type, &volume );

found:
    if (!status) status = set_volume_info( volume, NULL, unix_device, mount_point, type, guid, label, disk_serial );
    if (!status && scsi_info) create_scsi_entry( volume, scsi_info );
    if (volume) release_volume( volume );
    LeaveCriticalSection( &device_section );
    return status;
}

/* remove a disk volume */
NTSTATUS remove_volume( const char *udi )
{
    NTSTATUS status = STATUS_NO_SUCH_DEVICE;
    struct volume *volume;

    EnterCriticalSection( &device_section );
    LIST_FOR_EACH_ENTRY( volume, &volumes_list, struct volume, entry )
    {
        if (!volume->udi || strcmp( udi, volume->udi )) continue;
        set_volume_udi( volume, NULL );
        status = STATUS_SUCCESS;
        break;
    }
    LeaveCriticalSection( &device_section );
    return status;
}


/* create a new dos drive */
NTSTATUS add_dos_device( int letter, const char *udi, const char *unix_device,
                         const char *mount_point, enum device_type type, const GUID *guid,
                         const char *label, const struct scsi_info *scsi_info )
{
    HKEY hkey;
    NTSTATUS status = STATUS_SUCCESS;
    struct dos_drive *drive, *next;
    struct volume *volume;
    int notify = -1;
    char dosdev[] = "a::";

    EnterCriticalSection( &device_section );
    volume = find_matching_volume( udi, unix_device, mount_point, type );

    if (letter == -1)  /* auto-assign a letter */
    {
        struct add_drive_params params = { unix_device, type, &letter };
        if ((status = MOUNTMGR_CALL( add_drive, &params ))) goto done;

        LIST_FOR_EACH_ENTRY_SAFE( drive, next, &drives_list, struct dos_drive, entry )
        {
            if (drive->volume->udi && !strcmp( udi, drive->volume->udi )) goto found;
            if (drive->drive == letter) delete_dos_device( drive );
        }
    }
    else  /* simply reset the device symlink */
    {
        struct set_dosdev_symlink_params params = { dosdev, unix_device };

        LIST_FOR_EACH_ENTRY( drive, &drives_list, struct dos_drive, entry )
            if (drive->drive == letter) break;

        dosdev[0] = 'a' + letter;
        if (&drive->entry == &drives_list)
        {
            MOUNTMGR_CALL( set_dosdev_symlink, &params );
        }
        else
        {
            if (!unix_device || !drive->volume->device->unix_device ||
                strcmp( unix_device, drive->volume->device->unix_device ))
                MOUNTMGR_CALL( set_dosdev_symlink, &params );
            delete_dos_device( drive );
        }
    }

    if ((status = create_dos_device( volume, udi, letter, type, &drive ))) goto done;

found:
    if (!guid && !volume) guid = get_default_uuid( letter );
    if (!volume) volume = grab_volume( drive->volume );
    set_drive_info( drive, letter, volume );
    dosdev[0] = 'a' + drive->drive;
    dosdev[2] = 0;
    if (!mount_point || !volume->unix_mount || strcmp( mount_point, volume->unix_mount ))
    {
        struct set_dosdev_symlink_params params = { dosdev, mount_point };
        MOUNTMGR_CALL( set_dosdev_symlink, &params );
    }
    set_volume_info( volume, drive, unix_device, mount_point, type, guid, label, NULL );

    TRACE( "added device %c: udi %s for %s on %s type %u\n",
           'a' + drive->drive, debugstr_a(udi), debugstr_a(unix_device),
           debugstr_a(mount_point), type );

    /* hack: force the drive type in the registry */
    if (!RegCreateKeyW( HKEY_LOCAL_MACHINE, L"Software\\Wine\\Drives", &hkey ))
    {
        const WCHAR *type_name = drive_types[type];
        WCHAR name[] = L"a:";

        name[0] += drive->drive;
        if (!type_name[0] && type == DEVICE_HARDDISK) type_name = drive_types[DEVICE_FLOPPY];
        if (type_name[0])
            RegSetValueExW( hkey, name, 0, REG_SZ, (const BYTE *)type_name,
                            (lstrlenW(type_name) + 1) * sizeof(WCHAR) );
        else
            RegDeleteValueW( hkey, name );
        RegCloseKey( hkey );
    }

    if (udi) notify = drive->drive;
    if (scsi_info) create_scsi_entry( volume, scsi_info );

done:
    if (volume) release_volume( volume );
    LeaveCriticalSection( &device_section );
    if (notify != -1) send_notify( notify, DBT_DEVICEARRIVAL );
    return status;
}

/* remove an existing dos drive, by letter or udi */
NTSTATUS remove_dos_device( int letter, const char *udi )
{
    NTSTATUS status = STATUS_NO_SUCH_DEVICE;
    HKEY hkey;
    struct dos_drive *drive;
    char dosdev[] = "a:";
    int notify = -1;

    EnterCriticalSection( &device_section );
    LIST_FOR_EACH_ENTRY( drive, &drives_list, struct dos_drive, entry )
    {
        struct set_dosdev_symlink_params params = { dosdev, NULL };

        if (udi)
        {
            if (!drive->volume->udi) continue;
            if (strcmp( udi, drive->volume->udi )) continue;
            set_volume_udi( drive->volume, NULL );
        }
        else if (drive->drive != letter) continue;

        dosdev[0] = 'a' + drive->drive;
        MOUNTMGR_CALL( set_dosdev_symlink, &params );

        /* clear the registry key too */
        if (!RegOpenKeyW( HKEY_LOCAL_MACHINE, L"Software\\Wine\\Drives", &hkey ))
        {
            WCHAR name[] = L"a:";
            name[0] += drive->drive;
            RegDeleteValueW( hkey, name );
            RegCloseKey( hkey );
        }

        if (udi && drive->volume->unix_mount) notify = drive->drive;

        delete_dos_device( drive );
        status = STATUS_SUCCESS;
        break;
    }
    LeaveCriticalSection( &device_section );
    if (notify != -1) send_notify( notify, DBT_DEVICEREMOVECOMPLETE );
    return status;
}

static enum mountmgr_fs_type get_mountmgr_fs_type(enum fs_type fs_type)
{
    switch (fs_type)
    {
    case FS_ISO9660: return MOUNTMGR_FS_TYPE_ISO9660;
    case FS_UDF:     return MOUNTMGR_FS_TYPE_UDF;
    case FS_FAT1216: return MOUNTMGR_FS_TYPE_FAT;
    case FS_FAT32:   return MOUNTMGR_FS_TYPE_FAT32;
    default:         return MOUNTMGR_FS_TYPE_NTFS;
    }
}

/* query information about an existing dos drive, by letter or udi */
static struct volume *find_volume_by_letter( int letter )
{
    struct volume *volume = NULL;
    struct dos_drive *drive;

    LIST_FOR_EACH_ENTRY( drive, &drives_list, struct dos_drive, entry )
    {
        if (drive->drive != letter) continue;
        volume = grab_volume( drive->volume );
        TRACE( "found matching volume %s for drive letter %c:\n", debugstr_guid(&volume->guid),
               'a' + letter );
        break;
    }
    return volume;
}

/* query information about an existing unix device, by dev_t */
static struct volume *find_volume_by_unixdev( ULONGLONG unix_dev )
{
    struct volume *volume;

    LIST_FOR_EACH_ENTRY( volume, &volumes_list, struct volume, entry )
    {
        struct match_unixdev_params params = { volume->device->unix_device, unix_dev };
        if (!volume->device->unix_device || !MOUNTMGR_CALL( match_unixdev, &params ))
            continue;

        TRACE( "found matching volume %s\n", debugstr_guid(&volume->guid) );
        return grab_volume( volume );
    }
    return NULL;
}

/* implementation of IOCTL_MOUNTMGR_QUERY_UNIX_DRIVE */
NTSTATUS query_unix_drive( void *buff, SIZE_T insize, SIZE_T outsize, IO_STATUS_BLOCK *iosb )
{
    const struct mountmgr_unix_drive *input = buff;
    struct mountmgr_unix_drive *output = NULL;
    char *unix_device, *mount_point;
    int letter = towlower( input->letter );
    DWORD size, type = DEVICE_UNKNOWN, serial;
    NTSTATUS status = STATUS_SUCCESS;
    enum mountmgr_fs_type fs_type;
    enum device_type device_type;
    struct volume *volume;
    char *ptr;
    WCHAR *label;

    if (letter && (letter < 'a' || letter > 'z')) return STATUS_INVALID_PARAMETER;

    EnterCriticalSection( &device_section );
    if (letter)
        volume = find_volume_by_letter( letter - 'a' );
    else
        volume = find_volume_by_unixdev( input->unix_dev );
    if (volume)
    {
        device_type = volume->device->type;
        fs_type = get_mountmgr_fs_type( volume->fs_type );
        serial = volume->serial;
        unix_device = strdup( volume->device->unix_device );
        mount_point = strdup( volume->unix_mount );
        label = wcsdup( volume->label );
        release_volume( volume );
    }
    LeaveCriticalSection( &device_section );

    if (!volume)
        return STATUS_NO_SUCH_DEVICE;

    switch (device_type)
    {
    case DEVICE_UNKNOWN:      type = DRIVE_UNKNOWN; break;
    case DEVICE_HARDDISK:     type = DRIVE_REMOVABLE; break;
    case DEVICE_HARDDISK_VOL: type = DRIVE_FIXED; break;
    case DEVICE_FLOPPY:       type = DRIVE_REMOVABLE; break;
    case DEVICE_CDROM:        type = DRIVE_CDROM; break;
    case DEVICE_DVD:          type = DRIVE_CDROM; break;
    case DEVICE_NETWORK:      type = DRIVE_REMOTE; break;
    case DEVICE_RAMDISK:      type = DRIVE_RAMDISK; break;
    }

    size = sizeof(*output);
    if (label) size += (lstrlenW(label) + 1) * sizeof(WCHAR);
    if (unix_device) size += strlen( unix_device ) + 1;
    if (mount_point) size += strlen(mount_point) + 1;

    input = NULL;
    output = buff;
    output->size = size;
    output->letter = letter;
    output->type = type;
    output->fs_type = fs_type;
    output->serial = serial;
    output->mount_point_offset = 0;
    output->device_offset = 0;
    output->label_offset = 0;

    ptr = (char *)(output + 1);

    if (label && ptr + (lstrlenW(label) + 1) * sizeof(WCHAR) - (char *)output <= outsize)
    {
        output->label_offset = ptr - (char *)output;
        lstrcpyW( (WCHAR *)ptr, label );
        ptr += (lstrlenW(label) + 1) * sizeof(WCHAR);
    }
    if (mount_point && ptr + strlen(mount_point) + 1 - (char *)output <= outsize)
    {
        output->mount_point_offset = ptr - (char *)output;
        strcpy( ptr, mount_point );
        ptr += strlen(ptr) + 1;
    }
    if (unix_device && ptr + strlen( unix_device ) + 1 - (char *)output <= outsize)
    {
        output->device_offset = ptr - (char *)output;
        strcpy( ptr, unix_device );
        ptr += strlen(ptr) + 1;
    }

    TRACE( "returning %c: dev %s mount %s type %lu\n",
           letter, debugstr_a(unix_device), debugstr_a(mount_point), type );

    iosb->Information = ptr - (char *)output;
    if (size > outsize) status = STATUS_BUFFER_OVERFLOW;

    free( unix_device );
    free( mount_point );
    free( label );
    return status;
}

static NTSTATUS query_property( struct disk_device *device, IRP *irp )
{
    IO_STACK_LOCATION *irpsp = IoGetCurrentIrpStackLocation( irp );
    STORAGE_PROPERTY_QUERY *query = irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status;

    if (!irp->AssociatedIrp.SystemBuffer
        || irpsp->Parameters.DeviceIoControl.InputBufferLength < sizeof(STORAGE_PROPERTY_QUERY))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Try to persuade application not to check property */
    if (query->QueryType == PropertyExistsQuery)
    {
        return STATUS_NOT_SUPPORTED;
    }

    switch (query->PropertyId)
    {
    case StorageDeviceProperty:
    {
        STORAGE_DEVICE_DESCRIPTOR *descriptor;
        DWORD len = sizeof(*descriptor);

        if (device->disk_serial) len += strlen( device->disk_serial ) + 1;

        if (irpsp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(STORAGE_DESCRIPTOR_HEADER))
            status = STATUS_INVALID_PARAMETER;
        else if (irpsp->Parameters.DeviceIoControl.OutputBufferLength < len)
        {
            descriptor = irp->AssociatedIrp.SystemBuffer;
            descriptor->Version = sizeof(STORAGE_DEVICE_DESCRIPTOR);
            descriptor->Size = len;
            irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            status = STATUS_SUCCESS;
        }
        else
        {
            FIXME( "Faking StorageDeviceProperty data\n" );

            memset( irp->AssociatedIrp.SystemBuffer, 0, irpsp->Parameters.DeviceIoControl.OutputBufferLength );
            descriptor = irp->AssociatedIrp.SystemBuffer;
            descriptor->Version = sizeof(STORAGE_DEVICE_DESCRIPTOR);
            descriptor->Size = len;
            descriptor->DeviceType = FILE_DEVICE_DISK;
            descriptor->DeviceTypeModifier = 0;
            descriptor->RemovableMedia = FALSE;
            descriptor->CommandQueueing = FALSE;
            descriptor->VendorIdOffset = 0;
            descriptor->ProductIdOffset = 0;
            descriptor->ProductRevisionOffset = 0;
            descriptor->BusType = BusTypeScsi;
            descriptor->RawPropertiesLength = 0;
            if (!device->disk_serial) descriptor->SerialNumberOffset = 0;
            else
            {
                descriptor->SerialNumberOffset = sizeof(*descriptor);
                strcpy( (char *)descriptor + descriptor->SerialNumberOffset, device->disk_serial );
            }
            irp->IoStatus.Information = len;
            status = STATUS_SUCCESS;
        }

        break;
    }
    case StorageDeviceSeekPenaltyProperty:
    {
        DEVICE_SEEK_PENALTY_DESCRIPTOR *d = irp->AssociatedIrp.SystemBuffer;

        if (irpsp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(STORAGE_DESCRIPTOR_HEADER))
        {
            status = STATUS_INVALID_PARAMETER;
        }
        else
        {
            if (irpsp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(*d))
            {
                d->Version = d->Size = sizeof(*d);
                irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            }
            else
            {
                FIXME( "Faking StorageDeviceSeekPenaltyProperty data.\n" );

                memset( d, 0, sizeof(*d) );
                d->Version = d->Size = sizeof(*d);
                irp->IoStatus.Information = sizeof(*d);
            }
            status = STATUS_SUCCESS;
        }
        break;
    }
    case StorageDeviceTrimProperty:
    {
        DEVICE_TRIM_DESCRIPTOR *d = irp->AssociatedIrp.SystemBuffer;

        if (irpsp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(STORAGE_DESCRIPTOR_HEADER))
            status = STATUS_INVALID_PARAMETER;
        else
        {
            if (irpsp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(*d))
            {
                d->Version = d->Size = sizeof(*d);
                irp->IoStatus.Information = sizeof(STORAGE_DESCRIPTOR_HEADER);
            }
            else
            {
                FIXME( "Returning TRUE for StorageDeviceTrimProperty.\n" );
                d->Version = d->Size = sizeof(*d);
                d->TrimEnabled = TRUE;
                irp->IoStatus.Information = sizeof(*d);
            }
            status = STATUS_SUCCESS;
        }
        break;
    }

    default:
        FIXME( "Unsupported property %#x\n", query->PropertyId );
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    return status;
}

static NTSTATUS WINAPI disk_query_volume( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *irpsp = IoGetCurrentIrpStackLocation( irp );
    int info_class = irpsp->Parameters.QueryVolume.FsInformationClass;
    ULONG length = irpsp->Parameters.QueryVolume.Length;
    struct disk_device *dev = device->DeviceExtension;
    PIO_STATUS_BLOCK io = &irp->IoStatus;
    struct volume *volume;
    NTSTATUS status;

    TRACE( "volume query %x length %lu\n", info_class, length );

    EnterCriticalSection( &device_section );
    volume = dev->volume;
    if (!volume)
    {
        status = STATUS_BAD_DEVICE_TYPE;
        goto done;
    }

    switch(info_class)
    {
    case FileFsVolumeInformation:
    {

        FILE_FS_VOLUME_INFORMATION *info = irp->AssociatedIrp.SystemBuffer;

        if (length < sizeof(FILE_FS_VOLUME_INFORMATION))
        {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }

        info->VolumeCreationTime.QuadPart = 0; /* FIXME */
        info->VolumeSerialNumber = volume->serial;
        info->VolumeLabelLength = min( lstrlenW(volume->label) * sizeof(WCHAR),
                                       length - offsetof( FILE_FS_VOLUME_INFORMATION, VolumeLabel ) );
        info->SupportsObjects = (get_mountmgr_fs_type(volume->fs_type) == MOUNTMGR_FS_TYPE_NTFS);
        memcpy( info->VolumeLabel, volume->label, info->VolumeLabelLength );

        io->Information = offsetof( FILE_FS_VOLUME_INFORMATION, VolumeLabel ) + info->VolumeLabelLength;
        status = STATUS_SUCCESS;
        break;
    }
    case FileFsSizeInformation:
    {
        FILE_FS_SIZE_INFORMATION *info = irp->AssociatedIrp.SystemBuffer;
        struct size_info size_info = { 0, 0, 0, 0, 0 };
        struct get_volume_size_info_params params = { volume->unix_mount, &size_info };

        if (length < sizeof(FILE_FS_SIZE_INFORMATION))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if ((status = MOUNTMGR_CALL( get_volume_size_info, &params )) == STATUS_SUCCESS)
        {
            info->TotalAllocationUnits.QuadPart = size_info.total_allocation_units;
            info->AvailableAllocationUnits.QuadPart = size_info.caller_available_allocation_units;
            info->SectorsPerAllocationUnit = size_info.sectors_per_allocation_unit;
            info->BytesPerSector = size_info.bytes_per_sector;
            io->Information = sizeof(*info);
            status = STATUS_SUCCESS;
        }

        break;
    }
    case FileFsAttributeInformation:
    {
        FILE_FS_ATTRIBUTE_INFORMATION *info = irp->AssociatedIrp.SystemBuffer;
        enum mountmgr_fs_type fs_type = get_mountmgr_fs_type(volume->fs_type);
        const WCHAR *fsname;

        if (length < sizeof(FILE_FS_ATTRIBUTE_INFORMATION))
        {
            status = STATUS_INFO_LENGTH_MISMATCH;
            break;
        }

        switch (fs_type)
        {
        case MOUNTMGR_FS_TYPE_ISO9660:
            fsname = L"CDFS";
            info->FileSystemAttributes = FILE_READ_ONLY_VOLUME;
            info->MaximumComponentNameLength = 221;
            break;
        case MOUNTMGR_FS_TYPE_UDF:
            fsname = L"UDF";
            info->FileSystemAttributes = FILE_READ_ONLY_VOLUME | FILE_UNICODE_ON_DISK | FILE_CASE_SENSITIVE_SEARCH;
            info->MaximumComponentNameLength = 255;
            break;
        case MOUNTMGR_FS_TYPE_FAT:
            fsname = L"FAT";
            info->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES; /* FIXME */
            info->MaximumComponentNameLength = 255;
            break;
        case MOUNTMGR_FS_TYPE_FAT32:
            fsname = L"FAT32";
            info->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES; /* FIXME */
            info->MaximumComponentNameLength = 255;
            break;
        default:
            fsname = L"NTFS";
            info->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES | FILE_PERSISTENT_ACLS |
                                         FILE_SUPPORTS_REPARSE_POINTS | FILE_SUPPORTS_OPEN_BY_FILE_ID;
            info->MaximumComponentNameLength = 255;
            break;
        }
        info->FileSystemNameLength = min( wcslen(fsname) * sizeof(WCHAR), length - offsetof( FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName ) );
        memcpy(info->FileSystemName, fsname, info->FileSystemNameLength);
        io->Information = offsetof( FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName ) + info->FileSystemNameLength;
        status = STATUS_SUCCESS;
        break;
    }
    case FileFsFullSizeInformation:
    {
        FILE_FS_FULL_SIZE_INFORMATION *info = irp->AssociatedIrp.SystemBuffer;
        struct size_info size_info = { 0, 0, 0, 0, 0 };
        struct get_volume_size_info_params params = { volume->unix_mount, &size_info };

        if (length < sizeof(FILE_FS_FULL_SIZE_INFORMATION))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if ((status = MOUNTMGR_CALL( get_volume_size_info, &params )) == STATUS_SUCCESS)
        {
            info->TotalAllocationUnits.QuadPart = size_info.total_allocation_units;
            info->CallerAvailableAllocationUnits.QuadPart = size_info.caller_available_allocation_units;
            info->ActualAvailableAllocationUnits.QuadPart = size_info.actual_available_allocation_units;
            info->SectorsPerAllocationUnit = size_info.sectors_per_allocation_unit;
            info->BytesPerSector = size_info.bytes_per_sector;
            io->Information = sizeof(*info);
            status = STATUS_SUCCESS;
        }

        break;
    }

    case FileFsFullSizeInformationEx:
    {
        FILE_FS_FULL_SIZE_INFORMATION_EX *info = irp->AssociatedIrp.SystemBuffer;
        struct size_info size_info;
        struct get_volume_size_info_params params = { volume->unix_mount, &size_info };

        if (length < sizeof(FILE_FS_FULL_SIZE_INFORMATION_EX))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }

        if ((status = MOUNTMGR_CALL( get_volume_size_info, &params )) == STATUS_SUCCESS)
        {
            info->ActualTotalAllocationUnits = size_info.total_allocation_units;
            info->ActualAvailableAllocationUnits = size_info.actual_available_allocation_units;
            info->ActualPoolUnavailableAllocationUnits = 0;
            info->CallerAvailableAllocationUnits = size_info.caller_available_allocation_units;
            info->CallerPoolUnavailableAllocationUnits = 0;
            info->UsedAllocationUnits = info->ActualTotalAllocationUnits - info->ActualAvailableAllocationUnits;
            info->CallerTotalAllocationUnits = info->CallerAvailableAllocationUnits + info->UsedAllocationUnits;
            info->TotalReservedAllocationUnits = 0;
            info->VolumeStorageReserveAllocationUnits = 0;
            info->AvailableCommittedAllocationUnits = 0;
            info->PoolAvailableAllocationUnits = 0;
            info->SectorsPerAllocationUnit = size_info.sectors_per_allocation_unit;
            info->BytesPerSector = size_info.bytes_per_sector;
            io->Information = sizeof(*info);
            status = STATUS_SUCCESS;
        }

        break;
    }

    default:
        FIXME("Unsupported volume query %x\n", irpsp->Parameters.QueryVolume.FsInformationClass);
        status = STATUS_NOT_SUPPORTED;
        break;
    }

done:
    io->Status = status;
    LeaveCriticalSection( &device_section );
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

static NTSTATUS WINAPI disk_ioctl( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *irpsp = IoGetCurrentIrpStackLocation( irp );
    struct disk_device *dev = device->DeviceExtension;
    NTSTATUS status;

    TRACE( "ioctl %lx insize %lu outsize %lu\n",
           irpsp->Parameters.DeviceIoControl.IoControlCode,
           irpsp->Parameters.DeviceIoControl.InputBufferLength,
           irpsp->Parameters.DeviceIoControl.OutputBufferLength );

    EnterCriticalSection( &device_section );

    switch(irpsp->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    {
        DISK_GEOMETRY info;
        DWORD len = min( sizeof(info), irpsp->Parameters.DeviceIoControl.OutputBufferLength );

        info.Cylinders.QuadPart = 10000;
        info.MediaType = (dev->devnum.DeviceType == FILE_DEVICE_DISK) ? FixedMedia : RemovableMedia;
        info.TracksPerCylinder = 255;
        info.SectorsPerTrack = 63;
        info.BytesPerSector = 512;
        memcpy( irp->AssociatedIrp.SystemBuffer, &info, len );
        irp->IoStatus.Information = len;
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_DISK_GET_DRIVE_GEOMETRY_EX:
    {
        DISK_GEOMETRY_EX info;
        DWORD len = min( sizeof(info), irpsp->Parameters.DeviceIoControl.OutputBufferLength );

        FIXME("The DISK_PARTITION_INFO and DISK_DETECTION_INFO structures will not be filled\n");

        info.Geometry.Cylinders.QuadPart = 10000;
        info.Geometry.MediaType = (dev->devnum.DeviceType == FILE_DEVICE_DISK) ? FixedMedia : RemovableMedia;
        info.Geometry.TracksPerCylinder = 255;
        info.Geometry.SectorsPerTrack = 63;
        info.Geometry.BytesPerSector = 512;
        info.DiskSize.QuadPart = info.Geometry.Cylinders.QuadPart * info.Geometry.TracksPerCylinder *
                                 info.Geometry.SectorsPerTrack * info.Geometry.BytesPerSector;
        info.Data[0]  = 0;
        memcpy( irp->AssociatedIrp.SystemBuffer, &info, len );
        irp->IoStatus.Information = len;
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_STORAGE_GET_DEVICE_NUMBER:
    {
        DWORD len = min( sizeof(dev->devnum), irpsp->Parameters.DeviceIoControl.OutputBufferLength );

        memcpy( irp->AssociatedIrp.SystemBuffer, &dev->devnum, len );
        irp->IoStatus.Information = len;
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS:
    {
        struct size_info size_info = { 0, 0, 0, 0, 0 };
        struct get_volume_size_info_params params;
        VOLUME_DISK_EXTENTS info = { 0 };

        if (!dev->volume)
        {
            status = STATUS_INVALID_DEVICE_REQUEST;
            break;
        }

        if (irpsp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(info))
        {
            TRACE( "len %lu is too small.\n", irpsp->Parameters.DeviceIoControl.OutputBufferLength );
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        FIXME( "IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS semi-stub.\n" );

        info.NumberOfDiskExtents = 1;
        info.Extents[0].DiskNumber = dev->devnum.DeviceNumber;
        info.Extents[0].StartingOffset.QuadPart = 0;

        params.unix_mount = dev->volume->unix_mount;
        params.info = &size_info;

        if ((status = MOUNTMGR_CALL( get_volume_size_info, &params )) == STATUS_SUCCESS)
        {
            info.Extents[0].ExtentLength.QuadPart = size_info.total_allocation_units
                                                    * size_info.sectors_per_allocation_unit
                                                    * size_info.bytes_per_sector;
        }
        memcpy( irp->AssociatedIrp.SystemBuffer, &info, sizeof(info) );
        irp->IoStatus.Information = sizeof(info);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_STORAGE_QUERY_PROPERTY:
        status = query_property( dev, irp );
        break;
    default:
    {
        ULONG code = irpsp->Parameters.DeviceIoControl.IoControlCode;

        if (dev->type == DEVICE_CDROM || dev->type == DEVICE_DVD)
        {
            struct cdrom_ioctl_params params;

            params.cdrom = irpsp->FileObject->FsContext;
            params.code = code;
            params.input_size = irpsp->Parameters.DeviceIoControl.InputBufferLength;
            params.output_size = irpsp->Parameters.DeviceIoControl.OutputBufferLength;
            params.input = irp->AssociatedIrp.SystemBuffer;
            params.output = irp->UserBuffer;
            status = MOUNTMGR_CALL( cdrom_ioctl, &params );
            irp->IoStatus.Information = params.ret_size;
            break;
        }

        FIXME("Unsupported ioctl %lx (device=%lx access=%lx func=%lx method=%lx)\n",
              code, code >> 16, (code >> 14) & 3, (code >> 2) & 0xfff, code & 3);
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    }

    irp->IoStatus.Status = status;
    LeaveCriticalSection( &device_section );
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

static NTSTATUS WINAPI disk_create( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    struct disk_device *dev = device->DeviceExtension;
    NTSTATUS status;

    if (dev->type == DEVICE_CDROM || dev->type == DEVICE_DVD)
    {
        struct cdrom_open_params params = {.unix_device = dev->unix_device};
        if (!(status = MOUNTMGR_CALL( cdrom_open, &params )))
            stack->FileObject->FsContext = params.cdrom;
    }
    else
        status = STATUS_SUCCESS;

    irp->IoStatus.Status = status;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

static NTSTATUS WINAPI disk_close( DEVICE_OBJECT *device, IRP *irp )
{
    IO_STACK_LOCATION *stack = IoGetCurrentIrpStackLocation( irp );
    struct disk_device *dev = device->DeviceExtension;
    NTSTATUS status;

    if (dev->type == DEVICE_CDROM || dev->type == DEVICE_DVD)
        status = MOUNTMGR_CALL( cdrom_close, stack->FileObject->FsContext );
    else
        status = STATUS_SUCCESS;

    irp->IoStatus.Status = status;
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

/* driver entry point for the harddisk driver */
NTSTATUS WINAPI disk_driver_entry( DRIVER_OBJECT *driver, UNICODE_STRING *path )
{
    struct disk_device *device;

    disk_driver = driver;
    driver->MajorFunction[IRP_MJ_CREATE] = disk_create;
    driver->MajorFunction[IRP_MJ_CLOSE] = disk_close;
    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = disk_ioctl;
    driver->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] = disk_query_volume;

    /* create a harddisk0 device that isn't assigned to any drive */
    create_disk_device( DEVICE_HARDDISK, &device, NULL );

    create_drive_devices();

    return STATUS_SUCCESS;
}


/* serial or parallel port state, indexed by port number - 1 */
struct port_slot
{
    DEVICE_OBJECT *dev_obj;       /* NT device, NULL if the port doesn't exist */
    char          *unix_path;     /* Unix device the dosdevices symlink points to */
    BOOL           user_defined;  /* port comes from Software\Wine\Ports */
    WCHAR         *instance_id;   /* Enum\USB key of a detected USB serial port */
    int            home;          /* number a USB serial port goes back to once it is free */
    BOOL           not_usb;       /* detected serial port known not to be a USB device */
};

struct port_type
{
    const WCHAR *wine_prefix;
    const char  *dosdev_format;
    const WCHAR *nt_name_format;
    const WCHAR *reg_value_format;
    const WCHAR *symlink_format;
    const WCHAR *default_device;
    const WCHAR *windows_ports_key_name;
    struct port_slot *slots;
    char **detected;              /* Unix devices found, in detection order */
};

static struct port_slot serial_slots[MAX_PORTS];
static struct port_slot parallel_slots[MAX_PORTS];
static char *serial_detected[MAX_PORTS];
static char *parallel_detected[MAX_PORTS];

static const struct port_type serial_type =
{
    L"COM", "com%u", L"\\Device\\Serial%u", L"COM%u", L"\\DosDevices\\COM%u",
    L"\\DosDevices\\AUX", L"HARDWARE\\DEVICEMAP\\SERIALCOMM", serial_slots,
    serial_detected
};

static const struct port_type parallel_type =
{
    L"LPT", "lpt%u", L"\\Device\\Parallel%u", L"\\DosDevices\\LPT%u", L"\\DosDevices\\LPT%u",
    L"\\DosDevices\\PRN", L"HARDWARE\\DEVICEMAP\\PARALLEL PORTS", parallel_slots,
    parallel_detected
};

static CRITICAL_SECTION port_section;
static CRITICAL_SECTION_DEBUG port_critsect_debug =
{
    0, 0, &port_section,
    { &port_critsect_debug.ProcessLocksList, &port_critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": port_section") }
};
static CRITICAL_SECTION port_section = { &port_critsect_debug, -1, 0, 0, 0, 0 };

static const struct port_type *get_port_type( DRIVER_OBJECT *driver )
{
    if (driver == serial_driver) return &serial_type;
    assert( driver == parallel_driver );
    return &parallel_type;
}

#define ENUM_USB_KEY L"System\\CurrentControlSet\\Enum\\USB"
static const WCHAR ports_class_guid[] = L"{4D36E978-E325-11CE-BFC1-08002BE10318}";
static const WCHAR usb_class_guid[] = L"{36FC9E60-C465-11CF-8056-444553540000}";
/* DEVPKEY_Device_Parent, as cfgmgr32 stores device properties */
#define PARENT_PROPERTY_KEY L"Properties\\{4340A6C5-93FA-4706-972C-7B648008A5A7}\\0008"
#define DEVPROP_TYPE_STRING_REG 0xffff0012

static void set_reg_string( HKEY key, const WCHAR *name, const WCHAR *value )
{
    RegSetValueExW( key, name, 0, REG_SZ, (const BYTE *)value, (wcslen( value ) + 1) * sizeof(WCHAR) );
}

/* get the USB identity of a serial port; STATUS_NOT_SUPPORTED if it isn't a USB device,
 * other errors if it couldn't be read */
static NTSTATUS get_usb_port_info( const char *unix_path, struct serial_port_info *info )
{
    NTSTATUS status;

    memset( info, 0, sizeof(*info) );
    info->unix_path = unix_path;
    if (!(status = MOUNTMGR_CALL( get_serial_port_info, info )) && !info->usb) status = STATUS_NOT_SUPPORTED;
    return status;
}

/* check whether a USB serial number can make up an instance ID */
static BOOL is_valid_usb_serial( const char *serial )
{
    const char *p;

    /* Windows only uses serial numbers made of printable ASCII without commas */
    if (!*serial) return FALSE;
    for (p = serial; *p; p++)
        if (*p <= ' ' || *p > '~' || *p == ',' || *p == '\\' || *p == '&') return FALSE;
    return TRUE;
}

/* build the device instance ID of a USB serial port, relative to Enum\USB */
static void get_usb_instance_id( const struct serial_port_info *info, WCHAR *id, size_t len )
{
    /* the ports of one interface share the device's serial number, and the interfaces of a
     * composite device leave it to their parent */
    BOOL use_serial = !info->composite && !info->multi_port && is_valid_usb_serial( info->serial );
    unsigned int hash = 2166136261u;
    const char *p;
    int pos;

    pos = swprintf( id, len, L"VID_%04X&PID_%04X", info->vid, info->pid );
    if (info->composite) pos += swprintf( id + pos, len - pos, L"&MI_%02X", info->interface );
    if (use_serial) swprintf( id + pos, len - pos, L"\\%hs", info->serial );
    /* a generated ID contains '&', so that it isn't taken for a serial number; like the ID that
     * Windows derives from the parent device, it follows the device to another USB port if the
     * device has a serial number, and is tied to the USB port otherwise */
    else
    {
        if (!is_valid_usb_serial( info->serial ))
            pos += swprintf( id + pos, len - pos, L"\\7&wine&%hs&%u", info->location, info->interface );
        else
        {
            /* FNV-1 hash of the device, as the PnP manager does for the parent ID prefix */
            hash = (hash * 16777619) ^ info->vid;
            hash = (hash * 16777619) ^ info->pid;
            for (p = info->serial; *p; p++) hash = (hash * 16777619) ^ (unsigned char)*p;
            pos += swprintf( id + pos, len - pos, L"\\7&wine&%08x&%u", hash, info->interface );
        }
        if (info->multi_port) swprintf( id + pos, len - pos, L"&%u", info->port );
    }
}

/* build the instance ID of the composite device a USB serial port belongs to, relative to
 * Enum\USB; applications such as those using Qt find the serial number there */
static void get_usb_parent_id( const struct serial_port_info *info, WCHAR *id, size_t len )
{
    int pos = swprintf( id, len, L"VID_%04X&PID_%04X", info->vid, info->pid );

    if (is_valid_usb_serial( info->serial )) swprintf( id + pos, len - pos, L"\\%hs", info->serial );
    else swprintf( id + pos, len - pos, L"\\7&wine&%hs", info->location );
}

/* get the COM port number remembered for a USB serial port, 0 if none */
static int get_usb_port_number( HKEY usb_key, const WCHAR *id )
{
    WCHAR path[MAX_PATH], name[16];
    DWORD home, size = sizeof(home);
    int n;

    /* the port may sit elsewhere while a user-defined port has its number */
    if (!RegGetValueW( usb_key, id, L"WinePortNumber", RRF_RT_REG_DWORD, NULL, &home, &size ))
        return home >= 1 && home <= MAX_PORTS ? home : 0;

    size = sizeof(name);
    swprintf( path, ARRAY_SIZE(path), L"%s\\Device Parameters", id );
    if (RegGetValueW( usb_key, path, L"PortName", RRF_RT_REG_SZ, NULL, name, &size )) return 0;
    if (wcsnicmp( name, L"COM", 3 )) return 0;
    n = wcstol( name + 3, NULL, 10 );
    return n >= 1 && n <= MAX_PORTS ? n : 0;
}

/* collect the COM port numbers remembered for USB serial ports, present or not; if hide is
 * set, also mark the ports created by mountmgr as not present until they are detected again */
static void scan_usb_port_entries( BOOL reserved[MAX_PORTS], BOOL hide )
{
    WCHAR device[MAX_PATH], instance[MAX_PATH], id[MAX_PATH], class[40];
    DWORD size, marker, one = 1;
    HKEY usb_key, device_key, instance_key;
    int i, j, n;

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, ENUM_USB_KEY, 0, KEY_READ | KEY_SET_VALUE, &usb_key )) return;

    for (i = 0; !RegEnumKeyW( usb_key, i, device, ARRAY_SIZE(device) ); i++)
    {
        if (RegOpenKeyExW( usb_key, device, 0, KEY_ENUMERATE_SUB_KEYS, &device_key )) continue;
        for (j = 0; !RegEnumKeyW( device_key, j, instance, ARRAY_SIZE(instance) ); j++)
        {
            if (RegOpenKeyExW( device_key, instance, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &instance_key ))
                continue;
            size = sizeof(class);
            if (!RegGetValueW( instance_key, NULL, L"ClassGUID", RRF_RT_REG_SZ, NULL, class, &size ) &&
                !wcsicmp( class, ports_class_guid ))
            {
                swprintf( id, ARRAY_SIZE(id), L"%s\\%s", device, instance );
                if ((n = get_usb_port_number( usb_key, id ))) reserved[n - 1] = TRUE;
            }

            /* this includes the composite parents created for the ports */
            size = sizeof(marker);
            if (hide && !RegGetValueW( instance_key, NULL, L"WineSerialPort", RRF_RT_REG_DWORD, NULL,
                                       &marker, &size ) && marker)
                RegSetValueExW( instance_key, L"Phantom", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
            RegCloseKey( instance_key );
        }
        RegCloseKey( device_key );
    }
    RegCloseKey( usb_key );
}

/* create the Enum\USB entry of the composite device a USB serial port belongs to, and link the
 * port's entry to it */
static void set_usb_parent_entry( HKEY port_key, const struct serial_port_info *info )
{
    WCHAR id[MAX_PATH], path[MAX_PATH], buffer[MAX_PATH];
    HKEY key;
    DWORD one = 1;
    int len;

    get_usb_parent_id( info, id, ARRAY_SIZE(id) );
    swprintf( path, ARRAY_SIZE(path), ENUM_USB_KEY L"\\%s", id );
    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL )) return;

    set_reg_string( key, L"Class", L"USB" );
    set_reg_string( key, L"ClassGUID", usb_class_guid );
    set_reg_string( key, L"DeviceDesc", L"USB Composite Device" );
    set_reg_string( key, L"Service", L"usbccgp" );
    len = swprintf( buffer, ARRAY_SIZE(buffer) - 1, L"USB\\VID_%04X&PID_%04X", info->vid, info->pid );
    buffer[len + 1] = 0;
    RegSetValueExW( key, L"HardwareID", 0, REG_MULTI_SZ, (BYTE *)buffer, (len + 2) * sizeof(WCHAR) );
    RegSetValueExW( key, L"WineSerialPort", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    RegDeleteValueW( key, L"Phantom" );
    RegCloseKey( key );

    swprintf( buffer, ARRAY_SIZE(buffer), L"USB\\%s", id );
    if (RegCreateKeyExW( port_key, PARENT_PROPERTY_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL )) return;
    RegSetValueExW( key, NULL, 0, DEVPROP_TYPE_STRING_REG, (BYTE *)buffer, (wcslen( buffer ) + 1) * sizeof(WCHAR) );
    RegCloseKey( key );
}

/* create or update the Enum\USB entry of a present USB serial port, so that SetupDi lists it */
static void set_usb_port_entry( const WCHAR *id, const struct serial_port_info *info, int n, DWORD home )
{
    WCHAR path[MAX_PATH], product[128], buffer[256];
    HKEY key, params_key;
    DWORD zero = 0, one = 1;
    const WCHAR *instance;
    int len;

    swprintf( path, ARRAY_SIZE(path), ENUM_USB_KEY L"\\%s", id );
    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL )) return;

    if (!info->product[0] ||
        !MultiByteToWideChar( CP_UTF8, 0, info->product, -1, product, ARRAY_SIZE(product) ))
        wcscpy( product, L"USB Serial Device" );

    set_reg_string( key, L"Class", L"Ports" );
    set_reg_string( key, L"ClassGUID", ports_class_guid );
    set_reg_string( key, L"DeviceDesc", product );
    swprintf( buffer, ARRAY_SIZE(buffer), L"%s (COM%u)", product, n );
    set_reg_string( key, L"FriendlyName", buffer );
    if (!info->manufacturer[0] ||
        !MultiByteToWideChar( CP_UTF8, 0, info->manufacturer, -1, buffer, ARRAY_SIZE(buffer) ))
        wcscpy( buffer, L"(Standard port types)" );
    set_reg_string( key, L"Mfg", buffer );
    set_reg_string( key, L"Service", L"usbser" );

    /* the hardware ID is the device ID without the instance */
    instance = wcschr( id, '\\' );
    len = swprintf( buffer, ARRAY_SIZE(buffer) - 1, L"USB\\%.*s", (int)(instance - id), id );
    buffer[len + 1] = 0;
    RegSetValueExW( key, L"HardwareID", 0, REG_MULTI_SZ, (BYTE *)buffer, (len + 2) * sizeof(WCHAR) );

    RegSetValueExW( key, L"ConfigFlags", 0, REG_DWORD, (BYTE *)&zero, sizeof(zero) );
    RegSetValueExW( key, L"WineSerialPort", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    RegSetValueExW( key, L"WinePortNumber", 0, REG_DWORD, (BYTE *)&home, sizeof(home) );
    RegDeleteValueW( key, L"Phantom" );

    if (info->composite) set_usb_parent_entry( key, info );

    if (!RegCreateKeyExW( key, L"Device Parameters", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &params_key, NULL ))
    {
        swprintf( buffer, ARRAY_SIZE(buffer), L"COM%u", n );
        set_reg_string( params_key, L"PortName", buffer );
        RegCloseKey( params_key );
    }
    RegCloseKey( key );
}

/* mark the Enum\USB entry of a USB serial port as not present, with the composite parent created
 * for it; another serial interface of the device would only go away with it when unplugged */
static void hide_usb_port_entry( const WCHAR *id )
{
    WCHAR path[MAX_PATH], parent[MAX_PATH];
    DWORD one = 1, marker, size = sizeof(parent) - sizeof(WCHAR);
    HKEY key, prop_key;
    LSTATUS err = ERROR_FILE_NOT_FOUND;

    swprintf( path, ARRAY_SIZE(path), ENUM_USB_KEY L"\\%s", id );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_SET_VALUE, &key )) return;
    RegSetValueExW( key, L"Phantom", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    /* the property type isn't a registry type, so RegGetValue would reject it */
    if (!RegOpenKeyExW( key, PARENT_PROPERTY_KEY, 0, KEY_QUERY_VALUE, &prop_key ))
    {
        err = RegQueryValueExW( prop_key, NULL, NULL, NULL, (BYTE *)parent, &size );
        RegCloseKey( prop_key );
    }
    RegCloseKey( key );

    if (err) return;
    parent[size / sizeof(WCHAR)] = 0;
    if (wcsnicmp( parent, L"USB\\", 4 )) return;
    swprintf( path, ARRAY_SIZE(path), ENUM_USB_KEY L"\\%s", parent + 4 );
    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key )) return;
    size = sizeof(marker);
    if (!RegGetValueW( key, NULL, L"WineSerialPort", RRF_RT_REG_DWORD, NULL, &marker, &size ) && marker)
        RegSetValueExW( key, L"Phantom", 0, REG_DWORD, (BYTE *)&one, sizeof(one) );
    RegCloseKey( key );
}

/* create or remove the dosdevices symlink of a port */
static NTSTATUS set_port_dosdev_symlink( const struct port_type *type, int n, const char *unix_path )
{
    char dosdev[8];
    struct set_dosdev_symlink_params params = { dosdev, unix_path };

    sprintf( dosdev, type->dosdev_format, n );
    return MOUNTMGR_CALL( set_dosdev_symlink, &params );
}

/* create a serial or parallel port */
static BOOL create_port_device( DRIVER_OBJECT *driver, int n, const char *unix_path, BOOL user_defined,
                                const WCHAR *instance_id )
{
    const struct port_type *type = get_port_type( driver );
    struct port_slot *slot = &type->slots[n - 1];
    WCHAR reg_value[256], nt_buffer[32], symlink_buffer[32];
    UNICODE_STRING nt_name, symlink_name, default_name;
    DEVICE_OBJECT *dev_obj;
    HKEY windows_ports_key;
    NTSTATUS status;
    char *path_copy;

    if (!(path_copy = strdup( unix_path ))) return FALSE;

    /* create DOS device */
    if (set_port_dosdev_symlink( type, n, unix_path ))
    {
        free( path_copy );
        return FALSE;
    }

    /* create NT device */
    swprintf( nt_buffer, ARRAY_SIZE(nt_buffer), type->nt_name_format, n - 1 );
    RtlInitUnicodeString( &nt_name, nt_buffer );
    status = IoCreateDevice( driver, 0, &nt_name, 0, 0, FALSE, &dev_obj );
    if (status != STATUS_SUCCESS)
    {
        FIXME( "IoCreateDevice %s got %lx\n", debugstr_w(nt_name.Buffer), status );
        free( path_copy );
        return FALSE;
    }
    swprintf( symlink_buffer, ARRAY_SIZE(symlink_buffer), type->symlink_format, n );
    RtlInitUnicodeString( &symlink_name, symlink_buffer );
    IoCreateSymbolicLink( &symlink_name, &nt_name );
    if (n == 1)
    {
        RtlInitUnicodeString( &default_name, type->default_device );
        IoCreateSymbolicLink( &default_name, &symlink_name );
    }

    /* TODO: store information about the Unix device in the NT device */

    /* create registry entry */
    if (!RegCreateKeyExW( HKEY_LOCAL_MACHINE, type->windows_ports_key_name, 0, NULL, REG_OPTION_VOLATILE,
                          KEY_ALL_ACCESS, NULL, &windows_ports_key, NULL ))
    {
        swprintf( reg_value, ARRAY_SIZE(reg_value), type->reg_value_format, n );
        RegSetValueExW( windows_ports_key, nt_name.Buffer, 0, REG_SZ,
                        (BYTE *)reg_value, (lstrlenW( reg_value ) + 1) * sizeof(WCHAR) );
        RegCloseKey( windows_ports_key );
    }

    slot->dev_obj = dev_obj;
    slot->unix_path = path_copy;
    slot->user_defined = user_defined;
    slot->instance_id = instance_id ? wcsdup( instance_id ) : NULL;
    return TRUE;
}

/* remove a serial or parallel port created by create_port_device */
static void remove_port_device( DRIVER_OBJECT *driver, int n )
{
    const struct port_type *type = get_port_type( driver );
    struct port_slot *slot = &type->slots[n - 1];
    WCHAR nt_buffer[32], symlink_buffer[32];
    UNICODE_STRING symlink_name, default_name;
    HKEY windows_ports_key;

    TRACE( "removing %s%u -> %s\n", debugstr_w(type->wine_prefix), n, debugstr_a(slot->unix_path) );

    if (n == 1)
    {
        RtlInitUnicodeString( &default_name, type->default_device );
        IoDeleteSymbolicLink( &default_name );
    }
    swprintf( symlink_buffer, ARRAY_SIZE(symlink_buffer), type->symlink_format, n );
    RtlInitUnicodeString( &symlink_name, symlink_buffer );
    IoDeleteSymbolicLink( &symlink_name );
    IoDeleteDevice( slot->dev_obj );

    if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, type->windows_ports_key_name, 0, KEY_SET_VALUE, &windows_ports_key ))
    {
        swprintf( nt_buffer, ARRAY_SIZE(nt_buffer), type->nt_name_format, n - 1 );
        RegDeleteValueW( windows_ports_key, nt_buffer );
        RegCloseKey( windows_ports_key );
    }

    set_port_dosdev_symlink( type, n, NULL );

    if (slot->instance_id) hide_usb_port_entry( slot->instance_id );

    free( slot->unix_path );
    free( slot->instance_id );
    memset( slot, 0, sizeof(*slot) );
}

/* read the user-defined ports from HKLM\Software\Wine\Ports, indexed by port number - 1 */
static void read_wine_ports( const struct port_type *type, char *unix_paths[MAX_PORTS] )
{
    HKEY wine_ports_key;
    char unix_path[256];
    WCHAR reg_value[256];
    WCHAR port[7];
    DWORD port_len, type_id, size;
    int i, n;

    memset( unix_paths, 0, MAX_PORTS * sizeof(*unix_paths) );

    if (RegOpenKeyExW( HKEY_LOCAL_MACHINE, L"Software\\Wine\\Ports", 0, KEY_QUERY_VALUE, &wine_ports_key ))
        return;

    for (i = 0; ; i++)
    {
        port_len = ARRAY_SIZE(port);
        size = sizeof(reg_value);
        if (RegEnumValueW( wine_ports_key, i, port, &port_len, NULL,
                    &type_id, (BYTE*)reg_value, &size ) != ERROR_SUCCESS)
            break;
        if (type_id != REG_SZ || wcsnicmp( port, type->wine_prefix, 3 ))
            continue;

        n = wcstol( port + 3, NULL, 10 );
        if (n < 1 || n >= MAX_PORTS)
            continue;

        if (!WideCharToMultiByte( CP_UNIXCP, WC_ERR_INVALID_CHARS, reg_value, size/sizeof(WCHAR),
                    unix_path, sizeof(unix_path), NULL, NULL))
            continue;

        free( unix_paths[n - 1] );
        unix_paths[n - 1] = strdup( unix_path );
    }

    RegCloseKey( wine_ports_key );
}

/* replace the list of detected ports; port_section must be held */
static void set_detected_ports( const struct port_type *type, const char *devices )
{
    int i;

    for (i = 0; i < MAX_PORTS; i++)
    {
        free( type->detected[i] );
        type->detected[i] = NULL;
    }
    for (i = 0; i < MAX_PORTS && *devices; i++, devices += strlen( devices ) + 1)
        type->detected[i] = strdup( devices );
}

/* check that the device behind a detected port is still the one it was created for */
static BOOL is_detected_port( const struct port_type *type, const struct port_slot *slot )
{
    struct serial_port_info info;
    WCHAR id[MAX_PATH];
    int i;

    for (i = 0; i < MAX_PORTS && type->detected[i]; i++)
        if (!strcmp( slot->unix_path, type->detected[i] )) break;
    if (i == MAX_PORTS || !type->detected[i]) return FALSE;
    if (type != &serial_type || slot->not_usb) return TRUE;

    /* a USB device may have been replaced by another one with the same Unix device; a failed
     * lookup may only mean that sysfs isn't ready, so it doesn't take away a port in use */
    if (get_usb_port_info( slot->unix_path, &info )) return TRUE;
    get_usb_instance_id( &info, id, ARRAY_SIZE(id) );
    return slot->instance_id && !wcscmp( slot->instance_id, id );
}

/* create the ports of the detected devices that have none, without moving the others;
 * port_section must be held */
static void place_detected_ports( DRIVER_OBJECT *driver, char *unix_paths[MAX_PORTS] )
{
    const struct port_type *type = get_port_type( driver );
    BOOL reserved[MAX_PORTS] = { 0 }, scanned = FALSE;
    struct serial_port_info info;
    struct port_slot *slot;
    NTSTATUS status;
    WCHAR id[MAX_PATH];
    HKEY usb_key;
    int i, n, home;

    for (i = 0; i < MAX_PORTS && type->detected[i]; i++)
    {
        for (n = 1; n <= MAX_PORTS; n++)
        {
            slot = &type->slots[n - 1];
            if (slot->dev_obj && !slot->user_defined && !strcmp( slot->unix_path, type->detected[i] ))
                break;
        }
        if (n <= MAX_PORTS) continue;

        /* keep the numbers of absent USB serial ports for when they come back, as Windows does */
        if (type == &serial_type && !scanned)
        {
            scan_usb_port_entries( reserved, FALSE );
            scanned = TRUE;
        }

        n = home = 0;
        status = type == &serial_type ? get_usb_port_info( type->detected[i], &info ) : STATUS_NOT_SUPPORTED;
        if (!status)
        {
            get_usb_instance_id( &info, id, ARRAY_SIZE(id) );
            if (!RegOpenKeyExW( HKEY_LOCAL_MACHINE, ENUM_USB_KEY, 0, KEY_READ, &usb_key ))
            {
                home = get_usb_port_number( usb_key, id );
                RegCloseKey( usb_key );
            }
            /* while a user-defined port has the number, the port waits elsewhere to go back to it;
             * if another device took it while the port was absent, the port gets a new one */
            if (home && !unix_paths[home - 1])
            {
                if (!type->slots[home - 1].dev_obj) n = home;
                else home = 0;
            }
        }
        if (!n)
        {
            for (n = 1; n <= MAX_PORTS; n++)
                if (!type->slots[n - 1].dev_obj && !unix_paths[n - 1] && !reserved[n - 1]) break;
            if (n > MAX_PORTS) break;
        }
        if (!home) home = n;

        TRACE( "placing detected %s%u -> %s\n", debugstr_w(type->wine_prefix), n, debugstr_a(type->detected[i]) );
        if (!create_port_device( driver, n, type->detected[i], FALSE, status ? NULL : id )) continue;

        slot = &type->slots[n - 1];
        slot->not_usb = status == STATUS_NOT_SUPPORTED;
        if (!status)
        {
            slot->home = home;
            set_usb_port_entry( id, &info, n, home );
        }
    }
}

/* find and create serial or parallel ports */
static void create_port_devices( DRIVER_OBJECT *driver, const char *devices )
{
    const struct port_type *type = get_port_type( driver );
    char *unix_paths[MAX_PORTS];
    BOOL reserved[MAX_PORTS] = { 0 };
    HKEY wine_ports_key;
    int n;

    /* @@ Wine registry key: HKLM\Software\Wine\Ports */

    if (!RegCreateKeyExW( HKEY_LOCAL_MACHINE, L"Software\\Wine\\Ports", 0, NULL, 0,
                          KEY_QUERY_VALUE, NULL, &wine_ports_key, NULL ))
        RegCloseKey( wine_ports_key );

    read_wine_ports( type, unix_paths );

    /* USB serial ports left over from a previous session are present only once detected */
    if (type == &serial_type) scan_usb_port_entries( reserved, TRUE );

    EnterCriticalSection( &port_section );

    set_detected_ports( type, devices );

    /* add user-defined serial ports */
    for (n = 1; n <= MAX_PORTS; n++)
        if (unix_paths[n - 1]) create_port_device( driver, n, unix_paths[n - 1], TRUE, NULL );

    /* add the ports found in the usual places */
    place_detected_ports( driver, unix_paths );

    LeaveCriticalSection( &port_section );

    for (n = 0; n < MAX_PORTS; n++) free( unix_paths[n] );
}

/* bring the ports in line with HKLM\Software\Wine\Ports and, if given, the detected devices */
static void sync_port_devices( DRIVER_OBJECT *driver, const char *devices )
{
    const struct port_type *type;
    char *unix_paths[MAX_PORTS];
    struct port_slot *slot;
    char *path_copy;
    int n;

    if (!driver) return;  /* the driver failed to load */
    type = get_port_type( driver );

    read_wine_ports( type, unix_paths );

    EnterCriticalSection( &port_section );

    if (devices) set_detected_ports( type, devices );

    for (n = 1; n <= MAX_PORTS; n++)
    {
        slot = &type->slots[n - 1];

        if (unix_paths[n - 1])
        {
            if (slot->dev_obj && !slot->user_defined)
            {
                /* user-defined port replaces a detected one, as it would at startup;
                 * the detected one moves to a free slot in place_detected_ports */
                remove_port_device( driver, n );
            }
            if (!slot->dev_obj)
            {
                TRACE( "adding %s%u -> %s\n", debugstr_w(type->wine_prefix), n, debugstr_a(unix_paths[n - 1]) );
                create_port_device( driver, n, unix_paths[n - 1], TRUE, NULL );
            }
            else if (strcmp( slot->unix_path, unix_paths[n - 1] ))
            {
                TRACE( "changing %s%u -> %s\n", debugstr_w(type->wine_prefix), n, debugstr_a(unix_paths[n - 1]) );
                if ((path_copy = strdup( unix_paths[n - 1] )) &&
                    !set_port_dosdev_symlink( type, n, unix_paths[n - 1] ))
                {
                    free( slot->unix_path );
                    slot->unix_path = path_copy;
                }
                else free( path_copy );
            }
        }
        else if (slot->dev_obj && slot->user_defined)
        {
            /* only a USB serial port pushed aside from this number moves back into it, below */
            remove_port_device( driver, n );
        }
        else if (slot->dev_obj && !is_detected_port( type, slot ))
        {
            /* the device is gone, or replaced by another one that gets its own port */
            TRACE( "removing detected %s%u -> %s\n", debugstr_w(type->wine_prefix), n, debugstr_a(slot->unix_path) );
            remove_port_device( driver, n );
        }
    }

    /* a USB serial port pushed aside by a user-defined port goes back to its number once it is
     * free; it is removed here and placed there again */
    for (n = 1; n <= MAX_PORTS; n++)
    {
        slot = &type->slots[n - 1];
        if (!slot->dev_obj || !slot->home || slot->home == n) continue;
        if (type->slots[slot->home - 1].dev_obj || unix_paths[slot->home - 1]) continue;
        TRACE( "moving %s%u back to %s%u\n", debugstr_w(type->wine_prefix), n,
               debugstr_w(type->wine_prefix), slot->home );
        remove_port_device( driver, n );
    }

    place_detected_ports( driver, unix_paths );

    LeaveCriticalSection( &port_section );

    for (n = 0; n < MAX_PORTS; n++) free( unix_paths[n] );
}

/* thread signaling its event when serial ports appear or disappear; it owns the event handle */
static DWORD WINAPI port_hotplug_thread( void *arg )
{
    NTSTATUS status;

    /* the first call only starts watching, the ports are rescanned after it to miss no change */
    while (!(status = MOUNTMGR_CALL( wait_serial_ports, NULL ))) SetEvent( arg );
    WARN( "not watching for serial port changes, status %#lx\n", status );
    CloseHandle( arg );
    return 0;
}

/* start watching for serial port changes, return the event signaled on a change or NULL */
static HANDLE start_port_hotplug(void)
{
    HANDLE event, thread_event, thread;

    if (!(event = CreateEventW( NULL, FALSE, FALSE, NULL ))) return NULL;
    if (DuplicateHandle( GetCurrentProcess(), event, GetCurrentProcess(), &thread_event,
                         0, FALSE, DUPLICATE_SAME_ACCESS ))
    {
        if ((thread = CreateThread( NULL, 0, port_hotplug_thread, thread_event, 0, NULL )))
        {
            CloseHandle( thread );
            return event;
        }
        CloseHandle( thread_event );
    }
    WARN( "failed to start watching for serial port changes\n" );
    CloseHandle( event );
    return NULL;
}

/* thread watching HKLM\Software\Wine\Ports and the serial devices for changes */
DWORD WINAPI port_watch_thread( void *arg )
{
    char devices[4096];
    struct detect_ports_params params = { devices, sizeof(devices) };
    HANDLE events[2];
    DWORD count;
    HKEY key;

    /* watch the parent key, deleting the ports key only notifies its parent */
    if (RegCreateKeyExW( HKEY_LOCAL_MACHINE, L"Software\\Wine", 0, NULL, 0, KEY_NOTIFY, NULL, &key, NULL ))
    {
        ERR( "failed to open the Wine key, not watching for port changes\n" );
        return 0;
    }

    if (!(events[0] = CreateEventW( NULL, FALSE, FALSE, NULL )))
    {
        ERR( "failed to create an event, not watching for port changes\n" );
        RegCloseKey( key );
        return 0;
    }
    /* without hotplug, still follow the registry */
    count = (events[1] = start_port_hotplug()) ? 2 : 1;

    for (;;)
    {
        if (RegNotifyChangeKeyValue( key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET,
                                     events[0], TRUE ))
        {
            ERR( "failed to watch the Wine key\n" );
            break;
        }

        /* sync after arming the notification so that no change is missed */
        MOUNTMGR_CALL( detect_serial_ports, &params );
        sync_port_devices( serial_driver, devices );
        sync_port_devices( parallel_driver, NULL );

        WaitForMultipleObjects( count, events, FALSE, INFINITE );
    }

    RegCloseKey( key );
    CloseHandle( events[0] );
    if (events[1]) CloseHandle( events[1] );
    return 0;
}

/* driver entry point for the serial port driver */
NTSTATUS WINAPI serial_driver_entry( DRIVER_OBJECT *driver, UNICODE_STRING *path )
{
    char devices[4096];
    struct detect_ports_params params = { devices, sizeof(devices) };

    serial_driver = driver;
    /* TODO: fill in driver->MajorFunction */

    MOUNTMGR_CALL( detect_serial_ports, &params );
    create_port_devices( driver, devices );

    return STATUS_SUCCESS;
}

/* driver entry point for the parallel port driver */
NTSTATUS WINAPI parallel_driver_entry( DRIVER_OBJECT *driver, UNICODE_STRING *path )
{
    char devices[4096];
    struct detect_ports_params params = { devices, sizeof(devices) };

    parallel_driver = driver;
    /* TODO: fill in driver->MajorFunction */

    MOUNTMGR_CALL( detect_parallel_ports, &params );
    create_port_devices( driver, devices );

    return STATUS_SUCCESS;
}
