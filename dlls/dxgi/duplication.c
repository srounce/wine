/*
 * DXGI desktop duplication
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "dxgi_private.h"
#include "d3d11.h"
#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);

struct dxgi_output_duplication
{
    IDXGIOutputDuplication IDXGIOutputDuplication_iface;
    LONG refcount;
    struct wined3d_private_store private_store;

    IDXGIOutput *output;
    struct wined3d_output *wined3d_output;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11Texture2D *texture;
    DXGI_OUTDUPL_DESC desc;

    /* Display mode at creation time; any change invalidates the duplication. */
    UINT mode_width, mode_height;
    enum wined3d_display_rotation mode_rotation;

    UINT64 handle;
    BYTE *buffer;
    UINT pitch;
    BOOL frame_acquired;
    BOOL lost;
};

static inline struct dxgi_output_duplication *impl_from_IDXGIOutputDuplication(IDXGIOutputDuplication *iface)
{
    return CONTAINING_RECORD(iface, struct dxgi_output_duplication, IDXGIOutputDuplication_iface);
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_QueryInterface(IDXGIOutputDuplication *iface,
        REFIID iid, void **out)
{
    TRACE("iface %p, iid %s, out %p.\n", iface, debugstr_guid(iid), out);

    if (IsEqualGUID(iid, &IID_IDXGIOutputDuplication)
            || IsEqualGUID(iid, &IID_IDXGIObject)
            || IsEqualGUID(iid, &IID_IUnknown))
    {
        IDXGIOutputDuplication_AddRef(iface);
        *out = iface;
        return S_OK;
    }

    WARN("%s not implemented, returning E_NOINTERFACE.\n", debugstr_guid(iid));
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE dxgi_output_duplication_AddRef(IDXGIOutputDuplication *iface)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);
    ULONG refcount = InterlockedIncrement(&dupl->refcount);

    TRACE("%p increasing refcount to %lu.\n", iface, refcount);

    return refcount;
}

static void dxgi_output_duplication_destroy(struct dxgi_output_duplication *dupl)
{
    if (dupl->handle)
    {
        struct dupl_destroy_params params = {.handle = dupl->handle};

        WINE_UNIX_CALL(unix_dupl_destroy, &params);
    }
    if (dupl->texture)
        ID3D11Texture2D_Release(dupl->texture);
    if (dupl->context)
        ID3D11DeviceContext_Release(dupl->context);
    if (dupl->device)
        ID3D11Device_Release(dupl->device);
    if (dupl->output)
        IDXGIOutput_Release(dupl->output);
    free(dupl->buffer);
    wined3d_private_store_cleanup(&dupl->private_store);
    free(dupl);
}

static ULONG STDMETHODCALLTYPE dxgi_output_duplication_Release(IDXGIOutputDuplication *iface)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);
    ULONG refcount = InterlockedDecrement(&dupl->refcount);

    TRACE("%p decreasing refcount to %lu.\n", iface, refcount);

    if (!refcount)
        dxgi_output_duplication_destroy(dupl);

    return refcount;
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_SetPrivateData(IDXGIOutputDuplication *iface,
        REFGUID guid, UINT data_size, const void *data)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);

    TRACE("iface %p, guid %s, data_size %u, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_set_private_data(&dupl->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_SetPrivateDataInterface(IDXGIOutputDuplication *iface,
        REFGUID guid, const IUnknown *object)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);

    TRACE("iface %p, guid %s, object %p.\n", iface, debugstr_guid(guid), object);

    return dxgi_set_private_data_interface(&dupl->private_store, guid, object);
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_GetPrivateData(IDXGIOutputDuplication *iface,
        REFGUID guid, UINT *data_size, void *data)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);

    TRACE("iface %p, guid %s, data_size %p, data %p.\n", iface, debugstr_guid(guid), data_size, data);

    return dxgi_get_private_data(&dupl->private_store, guid, data_size, data);
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_GetParent(IDXGIOutputDuplication *iface,
        REFIID iid, void **parent)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);

    TRACE("iface %p, iid %s, parent %p.\n", iface, debugstr_guid(iid), parent);

    return IDXGIOutput_QueryInterface(dupl->output, iid, parent);
}

static void STDMETHODCALLTYPE dxgi_output_duplication_GetDesc(IDXGIOutputDuplication *iface,
        DXGI_OUTDUPL_DESC *desc)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);

    TRACE("iface %p, desc %p.\n", iface, desc);

    if (desc)
        *desc = dupl->desc;
}

/* As on Windows, the application is expected to create a new duplication
 * after a mode change; the description and texture size no longer match. */
static BOOL dxgi_output_duplication_mode_changed(struct dxgi_output_duplication *dupl)
{
    enum wined3d_display_rotation rotation;
    struct wined3d_display_mode mode;
    HRESULT hr;

    wined3d_mutex_lock();
    hr = wined3d_output_get_display_mode(dupl->wined3d_output, &mode, &rotation);
    wined3d_mutex_unlock();

    return FAILED(hr) || mode.width != dupl->mode_width || mode.height != dupl->mode_height
            || rotation != dupl->mode_rotation;
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_AcquireNextFrame(IDXGIOutputDuplication *iface,
        UINT timeout_ms, DXGI_OUTDUPL_FRAME_INFO *info, IDXGIResource **resource)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);
    struct dupl_acquire_params params;
    LARGE_INTEGER now;
    NTSTATUS status;
    HRESULT hr;

    TRACE("iface %p, timeout_ms %u, info %p, resource %p.\n", iface, timeout_ms, info, resource);

    if (!info || !resource)
        return E_INVALIDARG;
    *resource = NULL;
    if (dupl->frame_acquired)
        return DXGI_ERROR_INVALID_CALL;
    if (dupl->lost)
        return DXGI_ERROR_ACCESS_LOST;

    params.handle = dupl->handle;
    params.buffer = dupl->buffer;
    params.timeout_ms = timeout_ms;
    params.width = dupl->desc.ModeDesc.Width;
    params.height = dupl->desc.ModeDesc.Height;
    params.pitch = dupl->pitch;
    status = WINE_UNIX_CALL(unix_dupl_acquire, &params);
    if (status && status != STATUS_TIMEOUT)
    {
        WARN("Failed to acquire frame, status %#lx.\n", status);
        dupl->lost = TRUE;
        return DXGI_ERROR_ACCESS_LOST;
    }
    /* Checked after the wait, so that neither a frame captured in the new
     * mode nor a timeout on a desktop that stopped changing hides it. */
    if (dxgi_output_duplication_mode_changed(dupl))
    {
        WARN("The display mode of the output changed.\n");
        dupl->lost = TRUE;
        return DXGI_ERROR_ACCESS_LOST;
    }
    if (status == STATUS_TIMEOUT)
        return DXGI_ERROR_WAIT_TIMEOUT;

    ID3D11DeviceContext_UpdateSubresource(dupl->context, (ID3D11Resource *)dupl->texture, 0, NULL,
            dupl->buffer, dupl->pitch, 0);

    if (FAILED(hr = ID3D11Texture2D_QueryInterface(dupl->texture, &IID_IDXGIResource, (void **)resource)))
    {
        ERR("Failed to get IDXGIResource from texture, hr %#lx.\n", hr);
        return DXGI_ERROR_ACCESS_LOST;
    }

    QueryPerformanceCounter(&now);
    memset(info, 0, sizeof(*info));
    info->LastPresentTime = now;
    info->AccumulatedFrames = 1;
    info->TotalMetadataBufferSize = sizeof(RECT);
    dupl->frame_acquired = TRUE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_GetFrameDirtyRects(IDXGIOutputDuplication *iface,
        UINT rects_size, RECT *rects, UINT *required_size)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);

    TRACE("iface %p, rects_size %u, rects %p, required_size %p.\n", iface, rects_size, rects, required_size);

    if (!dupl->frame_acquired)
        return DXGI_ERROR_INVALID_CALL;
    if (!required_size)
        return E_INVALIDARG;

    /* The whole output is reported as dirty on every frame. */
    *required_size = sizeof(RECT);
    if (rects_size < sizeof(RECT) || !rects)
        return DXGI_ERROR_MORE_DATA;
    SetRect(rects, 0, 0, dupl->desc.ModeDesc.Width, dupl->desc.ModeDesc.Height);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_GetFrameMoveRects(IDXGIOutputDuplication *iface,
        UINT rects_size, DXGI_OUTDUPL_MOVE_RECT *rects, UINT *required_size)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);

    TRACE("iface %p, rects_size %u, rects %p, required_size %p.\n", iface, rects_size, rects, required_size);

    if (!dupl->frame_acquired)
        return DXGI_ERROR_INVALID_CALL;
    if (!required_size)
        return E_INVALIDARG;
    *required_size = 0;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_GetFramePointerShape(IDXGIOutputDuplication *iface,
        UINT buffer_size, void *buffer, UINT *required_size, DXGI_OUTDUPL_POINTER_SHAPE_INFO *info)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);

    TRACE("iface %p, buffer_size %u, buffer %p, required_size %p, info %p.\n",
            iface, buffer_size, buffer, required_size, info);

    if (!dupl->frame_acquired)
        return DXGI_ERROR_INVALID_CALL;
    if (!required_size || !info)
        return E_INVALIDARG;

    /* The cursor is not captured; there is never a new pointer shape. */
    *required_size = 0;
    memset(info, 0, sizeof(*info));
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_MapDesktopSurface(IDXGIOutputDuplication *iface,
        DXGI_MAPPED_RECT *rect)
{
    TRACE("iface %p, rect %p.\n", iface, rect);

    /* DesktopImageInSystemMemory is FALSE. */
    return DXGI_ERROR_UNSUPPORTED;
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_UnMapDesktopSurface(IDXGIOutputDuplication *iface)
{
    TRACE("iface %p.\n", iface);

    return DXGI_ERROR_INVALID_CALL;
}

static HRESULT STDMETHODCALLTYPE dxgi_output_duplication_ReleaseFrame(IDXGIOutputDuplication *iface)
{
    struct dxgi_output_duplication *dupl = impl_from_IDXGIOutputDuplication(iface);

    TRACE("iface %p.\n", iface);

    if (!dupl->frame_acquired)
        return DXGI_ERROR_INVALID_CALL;
    dupl->frame_acquired = FALSE;
    return S_OK;
}

static const struct IDXGIOutputDuplicationVtbl dxgi_output_duplication_vtbl =
{
    dxgi_output_duplication_QueryInterface,
    dxgi_output_duplication_AddRef,
    dxgi_output_duplication_Release,
    /* IDXGIObject methods */
    dxgi_output_duplication_SetPrivateData,
    dxgi_output_duplication_SetPrivateDataInterface,
    dxgi_output_duplication_GetPrivateData,
    dxgi_output_duplication_GetParent,
    /* IDXGIOutputDuplication methods */
    dxgi_output_duplication_GetDesc,
    dxgi_output_duplication_AcquireNextFrame,
    dxgi_output_duplication_GetFrameDirtyRects,
    dxgi_output_duplication_GetFrameMoveRects,
    dxgi_output_duplication_GetFramePointerShape,
    dxgi_output_duplication_MapDesktopSurface,
    dxgi_output_duplication_UnMapDesktopSurface,
    dxgi_output_duplication_ReleaseFrame,
};

static BOOL WINAPI init_unixlib(INIT_ONCE *once, void *param, void **context)
{
    if (__wine_init_unix_call())
        WARN("Failed to load the dxgi Unix library; desktop duplication is unavailable.\n");
    return TRUE;
}

HRESULT dxgi_output_duplication_create(struct dxgi_output *output, IUnknown *device,
        IDXGIOutputDuplication **out)
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;
    struct dxgi_output_duplication *dupl;
    struct dupl_create_params params;
    struct wined3d_output_desc output_desc;
    enum wined3d_display_rotation rotation;
    struct wined3d_display_mode mode;
    D3D11_TEXTURE2D_DESC texture_desc;
    char key[CCHDEVICENAME * 3];
    UINT width, height;
    NTSTATUS status;
    HRESULT hr;

    *out = NULL;

    InitOnceExecuteOnce(&init_once, init_unixlib, NULL, NULL);
    if (!__wine_unixlib_handle)
        return E_NOTIMPL;

    wined3d_mutex_lock();
    hr = wined3d_output_get_desc(output->wined3d_output, &output_desc);
    if (SUCCEEDED(hr))
        hr = wined3d_output_get_display_mode(output->wined3d_output, &mode, &rotation);
    wined3d_mutex_unlock();
    if (FAILED(hr))
        return hr;

    width = output_desc.desktop_rect.right - output_desc.desktop_rect.left;
    height = output_desc.desktop_rect.bottom - output_desc.desktop_rect.top;
    if (!width || !height)
        return DXGI_ERROR_UNSUPPORTED;

    if (!(dupl = calloc(1, sizeof(*dupl))))
        return E_OUTOFMEMORY;
    dupl->IDXGIOutputDuplication_iface.lpVtbl = &dxgi_output_duplication_vtbl;
    dupl->refcount = 1;
    wined3d_private_store_init(&dupl->private_store);
    dupl->output = (IDXGIOutput *)&output->IDXGIOutput6_iface;
    IDXGIOutput_AddRef(dupl->output);
    dupl->wined3d_output = output->wined3d_output;
    dupl->mode_width = mode.width;
    dupl->mode_height = mode.height;
    dupl->mode_rotation = rotation;

    if (FAILED(hr = IUnknown_QueryInterface(device, &IID_ID3D11Device, (void **)&dupl->device)))
    {
        WARN("Device %p is not a Direct3D 11 device.\n", device);
        hr = E_INVALIDARG;
        goto fail;
    }
    ID3D11Device_GetImmediateContext(dupl->device, &dupl->context);

    dupl->desc.ModeDesc.Width = width;
    dupl->desc.ModeDesc.Height = height;
    dupl->desc.ModeDesc.RefreshRate.Numerator = mode.refresh_rate ? mode.refresh_rate : 60;
    dupl->desc.ModeDesc.RefreshRate.Denominator = 1;
    dupl->desc.ModeDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    dupl->desc.ModeDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE;
    dupl->desc.ModeDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    /* Frames show the desktop the way the user sees it. */
    dupl->desc.Rotation = DXGI_MODE_ROTATION_IDENTITY;
    dupl->desc.DesktopImageInSystemMemory = FALSE;

    texture_desc.Width = width;
    texture_desc.Height = height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.SampleDesc.Quality = 0;
    texture_desc.Usage = D3D11_USAGE_DEFAULT;
    texture_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texture_desc.CPUAccessFlags = 0;
    texture_desc.MiscFlags = 0;
    if (FAILED(hr = ID3D11Device_CreateTexture2D(dupl->device, &texture_desc, NULL, &dupl->texture)))
    {
        WARN("Failed to create desktop texture, hr %#lx.\n", hr);
        goto fail;
    }

    dupl->pitch = width * 4;
    if (!(dupl->buffer = malloc((size_t)dupl->pitch * height)))
    {
        hr = E_OUTOFMEMORY;
        goto fail;
    }

    WideCharToMultiByte(CP_UTF8, 0, output_desc.device_name, -1, key, sizeof(key), NULL, NULL);
    params.handle = 0;
    params.key = key;
    params.width = width;
    params.height = height;
    if ((status = WINE_UNIX_CALL(unix_dupl_create, &params)))
    {
        WARN("Failed to start desktop capture for %s, status %#lx.\n", debugstr_a(key), status);
        hr = status == STATUS_ACCESS_DENIED ? E_ACCESSDENIED : DXGI_ERROR_UNSUPPORTED;
        goto fail;
    }
    dupl->handle = params.handle;

    TRACE("Created output duplication %p for %s, %ux%u.\n", dupl, debugstr_a(key), width, height);
    *out = &dupl->IDXGIOutputDuplication_iface;
    return S_OK;

fail:
    dxgi_output_duplication_destroy(dupl);
    return hr;
}
