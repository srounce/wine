/*
 * DXGI desktop duplication, Unix side interface
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

#ifndef __WINE_DXGI_UNIXLIB_H
#define __WINE_DXGI_UNIXLIB_H

#include "wine/unixlib.h"

/* Desktop frames come from the host compositor through the
 * xdg-desktop-portal ScreenCast interface and PipeWire. */

/* 64-bit fields come first so that 32-bit and 64-bit layouts match. */

struct dupl_create_params
{
    UINT64 handle;      /* out */
    const char *key;    /* UTF-8 output name, used to remember the portal choice */
    UINT32 width;       /* desktop size of the output */
    UINT32 height;
};

struct dupl_acquire_params
{
    UINT64 handle;
    void *buffer;       /* B8G8R8A8, pitch * height bytes */
    UINT32 timeout_ms;
    UINT32 width;       /* destination size; frames are scaled to fit if needed */
    UINT32 height;
    UINT32 pitch;
};

struct dupl_destroy_params
{
    UINT64 handle;
};

enum dxgi_unix_funcs
{
    unix_dupl_create,
    unix_dupl_acquire,
    unix_dupl_destroy,
    unix_funcs_count,
};

#endif /* __WINE_DXGI_UNIXLIB_H */
