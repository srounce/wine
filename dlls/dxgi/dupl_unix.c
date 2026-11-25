/*
 * DXGI desktop duplication, Unix side
 *
 * Desktop frames are captured from the host compositor with the
 * xdg-desktop-portal ScreenCast interface (D-Bus) and PipeWire. This works on
 * Wayland compositors, where windows of other Wine instances or native
 * applications cannot be read back through X11 or GDI.
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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#if defined(HAVE_PIPEWIRE_PIPEWIRE_H) && defined(SONAME_LIBDBUS_1)
# define HAVE_DESKTOP_DUPLICATION 1
#endif

#ifdef HAVE_DESKTOP_DUPLICATION
#include <dbus/dbus.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#pragma GCC diagnostic pop
#endif

#define NOMINMAX
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"

#include "unixlib.h"

WINE_DEFAULT_DEBUG_CHANNEL(dxgi);

#ifdef HAVE_DESKTOP_DUPLICATION

#define PORTAL_BUS_NAME     "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH  "/org/freedesktop/portal/desktop"
#define PORTAL_SCREENCAST   "org.freedesktop.portal.ScreenCast"
#define PORTAL_REQUEST      "org.freedesktop.portal.Request"
#define PORTAL_SESSION      "org.freedesktop.portal.Session"

/* ScreenCast portal constants */
#define SOURCE_TYPE_MONITOR 1
#define CURSOR_MODE_HIDDEN  1
#define PERSIST_MODE_UNTIL_REVOKED 2

/* After the user cancels the portal dialog, fail immediately instead of
 * asking again on every retry of the application. */
#define CANCEL_COOLDOWN_SECONDS 300

struct dupl
{
    pthread_mutex_t lock;
    pthread_cond_t cond;

    DBusConnection *bus;
    char *session_handle;

    struct pw_thread_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_stream *stream;
    struct spa_hook stream_listener;

    /* Negotiated format; only touched on the PipeWire thread. */
    struct spa_video_info_raw format;
    BOOL swap_rb;

    /* Latest complete frame (front) and the one being filled (back). Both are
     * tightly packed 4 bytes per pixel in the stream's channel order. */
    unsigned char *front, *back;
    size_t front_size, back_size;
    UINT32 front_width, front_height;
    BOOL front_swap_rb;
    UINT64 frame_seq, read_seq;
    BOOL failed;
};

static pthread_mutex_t portal_mutex = PTHREAD_MUTEX_INITIALIZER;
static time_t last_cancel;
static unsigned int request_counter;

static void init_libraries_once(void)
{
    dbus_threads_init_default();
    pw_init(NULL, NULL);
}

static void init_libraries(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_libraries_once);
}

static void get_monotonic_time(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

/* ---------------------------------------------------------------------- */
/* Restore token storage                                                  */

static void sanitize(const char *src, char *dst, size_t size)
{
    size_t i;

    for (i = 0; src[i] && i + 1 < size; ++i)
    {
        char c = src[i];
        dst[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
    }
    dst[i] = 0;
}

static BOOL get_token_path(const char *key, char *path, size_t size)
{
    const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    const char *prefix = getenv("WINEPREFIX");
    char dir[PATH_MAX], sprefix[128], skey[64];

    if (state && *state)
        snprintf(dir, sizeof(dir), "%s", state);
    else if (home && *home)
        snprintf(dir, sizeof(dir), "%s/.local/state", home);
    else
        return FALSE;

    mkdir(dir, 0700);
    strncat(dir, "/wine", sizeof(dir) - strlen(dir) - 1);
    mkdir(dir, 0700);

    sanitize(prefix && *prefix ? prefix : "default", sprefix, sizeof(sprefix));
    sanitize(key, skey, sizeof(skey));
    snprintf(path, size, "%s/dxgi-duplication%s-%s.token", dir, sprefix, skey);
    return TRUE;
}

static char *load_restore_token(const char *key)
{
    char path[PATH_MAX], token[512];
    size_t len;
    FILE *file;

    if (!get_token_path(key, path, sizeof(path)) || !(file = fopen(path, "r")))
        return NULL;
    len = fread(token, 1, sizeof(token) - 1, file);
    fclose(file);
    while (len && (token[len - 1] == '\n' || token[len - 1] == ' ')) --len;
    token[len] = 0;
    return len ? strdup(token) : NULL;
}

static void save_restore_token(const char *key, const char *token)
{
    char path[PATH_MAX];
    int fd;

    if (!get_token_path(key, path, sizeof(path)))
        return;
    if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600)) < 0)
    {
        WARN("Failed to write restore token to %s, errno %d.\n", debugstr_a(path), errno);
        return;
    }
    if (write(fd, token, strlen(token)) < 0)
        WARN("Failed to write restore token, errno %d.\n", errno);
    close(fd);
}

/* ---------------------------------------------------------------------- */
/* D-Bus helpers                                                          */

static void dict_add(DBusMessageIter *dict, const char *key, int type, const char *signature, const void *value)
{
    DBusMessageIter entry, variant;

    dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, signature, &variant);
    dbus_message_iter_append_basic(&variant, type, value);
    dbus_message_iter_close_container(&entry, &variant);
    dbus_message_iter_close_container(dict, &entry);
}

static void dict_add_string(DBusMessageIter *dict, const char *key, const char *value)
{
    dict_add(dict, key, DBUS_TYPE_STRING, DBUS_TYPE_STRING_AS_STRING, &value);
}

static void dict_add_uint32(DBusMessageIter *dict, const char *key, dbus_uint32_t value)
{
    dict_add(dict, key, DBUS_TYPE_UINT32, DBUS_TYPE_UINT32_AS_STRING, &value);
}

static void dict_add_bool(DBusMessageIter *dict, const char *key, dbus_bool_t value)
{
    dict_add(dict, key, DBUS_TYPE_BOOLEAN, DBUS_TYPE_BOOLEAN_AS_STRING, &value);
}

/* Finds key in an a{sv} iterator and returns an iterator into its variant. */
static BOOL dict_find(DBusMessageIter *array, const char *key, DBusMessageIter *value)
{
    DBusMessageIter dict, entry;
    const char *name;

    if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_ARRAY)
        return FALSE;
    dbus_message_iter_recurse(array, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY)
    {
        dbus_message_iter_recurse(&dict, &entry);
        dbus_message_iter_get_basic(&entry, &name);
        if (!strcmp(name, key))
        {
            dbus_message_iter_next(&entry);
            dbus_message_iter_recurse(&entry, value);
            return TRUE;
        }
        dbus_message_iter_next(&dict);
    }
    return FALSE;
}

static char *dict_get_string(DBusMessageIter *array, const char *key)
{
    DBusMessageIter value;
    const char *str;
    int type;

    if (!dict_find(array, key, &value))
        return NULL;
    type = dbus_message_iter_get_arg_type(&value);
    if (type != DBUS_TYPE_STRING && type != DBUS_TYPE_OBJECT_PATH)
        return NULL;
    dbus_message_iter_get_basic(&value, &str);
    return strdup(str);
}

static dbus_uint32_t get_screencast_version(DBusConnection *bus)
{
    const char *iface = PORTAL_SCREENCAST, *prop = "version";
    DBusMessage *msg, *reply;
    DBusMessageIter iter, variant;
    dbus_uint32_t version = 0;
    DBusError err;

    msg = dbus_message_new_method_call(PORTAL_BUS_NAME, PORTAL_OBJECT_PATH,
            "org.freedesktop.DBus.Properties", "Get");
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop, DBUS_TYPE_INVALID);
    dbus_error_init(&err);
    reply = dbus_connection_send_with_reply_and_block(bus, msg, 5000, &err);
    dbus_message_unref(msg);
    if (!reply)
    {
        WARN("Failed to query ScreenCast portal version: %s.\n", debugstr_a(err.message));
        dbus_error_free(&err);
        return 0;
    }
    if (dbus_message_iter_init(reply, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_VARIANT)
    {
        dbus_message_iter_recurse(&iter, &variant);
        if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_UINT32)
            dbus_message_iter_get_basic(&variant, &version);
    }
    dbus_message_unref(reply);
    return version;
}

static void new_request_token(char *token, size_t size)
{
    snprintf(token, size, "wine_dxgi_%d_%u", (int)getpid(), __atomic_add_fetch(&request_counter, 1, __ATOMIC_SEQ_CST));
}

/* Sends a portal method call that returns a Request handle and waits for the
 * Request's Response signal. Returns the response code (0 = success, 1 = the
 * user cancelled, 2 = other error) or -1 on failure; *response holds the
 * Response message when the return value is >= 0. */
static int portal_request(DBusConnection *bus, DBusMessage *msg, const char *token,
        int timeout_ms, DBusMessage **response)
{
    char sender[256], path[512], match[768], *p;
    const char *handle = NULL;
    struct timespec start, now;
    DBusMessage *reply, *signal;
    dbus_uint32_t code;
    DBusError err;
    int ret = -1;

    *response = NULL;

    /* Subscribe before calling to avoid missing an early Response. */
    snprintf(sender, sizeof(sender), "%s", dbus_bus_get_unique_name(bus) + 1);
    for (p = sender; *p; ++p) if (*p == '.') *p = '_';
    snprintf(path, sizeof(path), PORTAL_OBJECT_PATH "/request/%s/%s", sender, token);
    snprintf(match, sizeof(match), "type='signal',interface='" PORTAL_REQUEST "',member='Response',path='%s'", path);
    dbus_error_init(&err);
    dbus_bus_add_match(bus, match, &err);
    if (dbus_error_is_set(&err))
    {
        WARN("Failed to add D-Bus match: %s.\n", debugstr_a(err.message));
        dbus_error_free(&err);
        return -1;
    }

    if (!(reply = dbus_connection_send_with_reply_and_block(bus, msg, 10000, &err)))
    {
        WARN("Portal call %s failed: %s.\n", debugstr_a(dbus_message_get_member(msg)), debugstr_a(err.message));
        dbus_error_free(&err);
        goto done;
    }
    if (dbus_message_get_args(reply, NULL, DBUS_TYPE_OBJECT_PATH, &handle, DBUS_TYPE_INVALID)
            && strcmp(handle, path))
    {
        /* Portals older than version 0.9 use their own handle path. */
        dbus_bus_remove_match(bus, match, NULL);
        snprintf(path, sizeof(path), "%s", handle);
        snprintf(match, sizeof(match), "type='signal',interface='" PORTAL_REQUEST "',member='Response',path='%s'", path);
        dbus_bus_add_match(bus, match, NULL);
    }
    dbus_message_unref(reply);

    get_monotonic_time(&start);
    for (;;)
    {
        while ((signal = dbus_connection_pop_message(bus)))
        {
            if (dbus_message_is_signal(signal, PORTAL_REQUEST, "Response")
                    && !strcmp(dbus_message_get_path(signal), path))
            {
                DBusMessageIter iter;

                if (dbus_message_iter_init(signal, &iter) && dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
                {
                    dbus_message_iter_get_basic(&iter, &code);
                    *response = signal;
                    ret = code;
                    goto done;
                }
            }
            dbus_message_unref(signal);
        }

        get_monotonic_time(&now);
        if ((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000 > timeout_ms)
        {
            WARN("Timed out waiting for portal response to %s.\n", debugstr_a(dbus_message_get_member(msg)));
            goto done;
        }
        if (!dbus_connection_read_write(bus, 200))
        {
            WARN("D-Bus connection closed.\n");
            goto done;
        }
    }

done:
    dbus_bus_remove_match(bus, match, NULL);
    return ret;
}

/* Returns an iterator to the results dictionary of a Response message. */
static BOOL response_results(DBusMessage *response, DBusMessageIter *results)
{
    if (!dbus_message_iter_init(response, results)) return FALSE;
    return dbus_message_iter_next(results) && dbus_message_iter_get_arg_type(results) == DBUS_TYPE_ARRAY;
}

static void close_portal_session(struct dupl *dupl)
{
    DBusMessage *msg;

    if (!dupl->bus) return;
    if (dupl->session_handle)
    {
        msg = dbus_message_new_method_call(PORTAL_BUS_NAME, dupl->session_handle, PORTAL_SESSION, "Close");
        dbus_connection_send(dupl->bus, msg, NULL);
        dbus_connection_flush(dupl->bus);
        dbus_message_unref(msg);
        free(dupl->session_handle);
        dupl->session_handle = NULL;
    }
    dbus_connection_close(dupl->bus);
    dbus_connection_unref(dupl->bus);
    dupl->bus = NULL;
}

/* Runs the ScreenCast portal handshake. On success returns the PipeWire node
 * of the selected monitor and a file descriptor to the PipeWire remote. */
static NTSTATUS start_portal_session(struct dupl *dupl, const char *key, UINT32 *node_id, int *pw_fd)
{
    char token[64], session_token[64];
    DBusMessageIter iter, dict, results, streams, stream;
    DBusMessage *msg, *response, *reply;
    dbus_uint32_t version;
    const char *str;
    char *restore_token;
    DBusError err;
    int code;

    dbus_error_init(&err);
    if (!(dupl->bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err)))
    {
        WARN("Failed to connect to the D-Bus session bus: %s.\n", debugstr_a(err.message));
        dbus_error_free(&err);
        return STATUS_NOT_SUPPORTED;
    }
    dbus_connection_set_exit_on_disconnect(dupl->bus, FALSE);

    if (!(version = get_screencast_version(dupl->bus)))
        return STATUS_NOT_SUPPORTED;
    TRACE("ScreenCast portal version %u.\n", version);

    /* CreateSession */
    new_request_token(token, sizeof(token));
    new_request_token(session_token, sizeof(session_token));
    msg = dbus_message_new_method_call(PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_SCREENCAST, "CreateSession");
    dbus_message_iter_init_append(msg, &iter);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dict_add_string(&dict, "handle_token", token);
    dict_add_string(&dict, "session_handle_token", session_token);
    dbus_message_iter_close_container(&iter, &dict);
    code = portal_request(dupl->bus, msg, token, 30000, &response);
    dbus_message_unref(msg);
    if (code != 0)
    {
        if (response) dbus_message_unref(response);
        WARN("CreateSession failed, response %d.\n", code);
        return STATUS_UNSUCCESSFUL;
    }
    if (response_results(response, &results))
        dupl->session_handle = dict_get_string(&results, "session_handle");
    dbus_message_unref(response);
    if (!dupl->session_handle)
    {
        WARN("No session handle in CreateSession response.\n");
        return STATUS_UNSUCCESSFUL;
    }

    /* SelectSources */
    restore_token = load_restore_token(key);
    new_request_token(token, sizeof(token));
    msg = dbus_message_new_method_call(PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_SCREENCAST, "SelectSources");
    dbus_message_iter_init_append(msg, &iter);
    str = dupl->session_handle;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &str);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dict_add_string(&dict, "handle_token", token);
    dict_add_uint32(&dict, "types", SOURCE_TYPE_MONITOR);
    dict_add_bool(&dict, "multiple", FALSE);
    if (version >= 2)
        dict_add_uint32(&dict, "cursor_mode", CURSOR_MODE_HIDDEN);
    if (version >= 4)
    {
        dict_add_uint32(&dict, "persist_mode", PERSIST_MODE_UNTIL_REVOKED);
        if (restore_token)
            dict_add_string(&dict, "restore_token", restore_token);
    }
    dbus_message_iter_close_container(&iter, &dict);
    code = portal_request(dupl->bus, msg, token, 30000, &response);
    dbus_message_unref(msg);
    free(restore_token);
    if (response) dbus_message_unref(response);
    if (code != 0)
    {
        WARN("SelectSources failed, response %d.\n", code);
        return STATUS_UNSUCCESSFUL;
    }

    /* Start; this is where the compositor may ask the user to pick a monitor. */
    new_request_token(token, sizeof(token));
    msg = dbus_message_new_method_call(PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_SCREENCAST, "Start");
    dbus_message_iter_init_append(msg, &iter);
    str = dupl->session_handle;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &str);
    str = "";
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &str);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dict_add_string(&dict, "handle_token", token);
    dbus_message_iter_close_container(&iter, &dict);
    code = portal_request(dupl->bus, msg, token, 600000, &response);
    dbus_message_unref(msg);
    if (code != 0)
    {
        if (response) dbus_message_unref(response);
        if (code == 1)
        {
            WARN("Screen capture was cancelled by the user.\n");
            last_cancel = time(NULL);
            return STATUS_ACCESS_DENIED;
        }
        WARN("Start failed, response %d.\n", code);
        return STATUS_UNSUCCESSFUL;
    }

    *node_id = 0;
    if (response_results(response, &results))
    {
        if ((restore_token = dict_get_string(&results, "restore_token")))
        {
            save_restore_token(key, restore_token);
            free(restore_token);
        }
        if (dict_find(&results, "streams", &streams) && dbus_message_iter_get_arg_type(&streams) == DBUS_TYPE_ARRAY)
        {
            dbus_message_iter_recurse(&streams, &stream);
            if (dbus_message_iter_get_arg_type(&stream) == DBUS_TYPE_STRUCT)
            {
                DBusMessageIter fields;

                dbus_message_iter_recurse(&stream, &fields);
                if (dbus_message_iter_get_arg_type(&fields) == DBUS_TYPE_UINT32)
                    dbus_message_iter_get_basic(&fields, node_id);
            }
        }
    }
    dbus_message_unref(response);
    if (!*node_id)
    {
        WARN("No stream in Start response.\n");
        return STATUS_UNSUCCESSFUL;
    }
    TRACE("Capturing PipeWire node %u.\n", *node_id);

    /* OpenPipeWireRemote */
    msg = dbus_message_new_method_call(PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_SCREENCAST, "OpenPipeWireRemote");
    dbus_message_iter_init_append(msg, &iter);
    str = dupl->session_handle;
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &str);
    dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
    dbus_message_iter_close_container(&iter, &dict);
    reply = dbus_connection_send_with_reply_and_block(dupl->bus, msg, 10000, &err);
    dbus_message_unref(msg);
    if (!reply || !dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, pw_fd, DBUS_TYPE_INVALID))
    {
        WARN("OpenPipeWireRemote failed: %s.\n", debugstr_a(err.message));
        dbus_error_free(&err);
        if (reply) dbus_message_unref(reply);
        return STATUS_UNSUCCESSFUL;
    }
    dbus_message_unref(reply);
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* PipeWire stream                                                        */

static void stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct dupl *dupl = data;
    uint32_t media_type, media_subtype;
    const struct spa_pod *buffers;
    struct spa_pod_builder builder;
    uint8_t buffer[256];

    if (!param || id != SPA_PARAM_Format)
        return;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0
            || media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &dupl->format) < 0)
        return;

    dupl->swap_rb = dupl->format.format == SPA_VIDEO_FORMAT_RGBx || dupl->format.format == SPA_VIDEO_FORMAT_RGBA;
    TRACE("Negotiated format %u, %ux%u, max framerate %u/%u.\n", dupl->format.format,
            dupl->format.size.width, dupl->format.size.height,
            dupl->format.max_framerate.num, dupl->format.max_framerate.denom);

    /* Ask for mappable memory; we don't negotiate DMA-BUF modifiers. */
    builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    buffers = spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr)));
    pw_stream_update_params(dupl->stream, &buffers, 1);
}

static void stream_state_changed(void *data, enum pw_stream_state old,
        enum pw_stream_state state, const char *error)
{
    struct dupl *dupl = data;

    TRACE("Stream state %s -> %s%s%s.\n", pw_stream_state_as_string(old), pw_stream_state_as_string(state),
            error ? ", error " : "", error ? error : "");
    if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && old != PW_STREAM_STATE_UNCONNECTED))
    {
        pthread_mutex_lock(&dupl->lock);
        dupl->failed = TRUE;
        pthread_cond_broadcast(&dupl->cond);
        pthread_mutex_unlock(&dupl->lock);
    }
}

static void stream_process(void *data)
{
    struct dupl *dupl = data;
    UINT32 width = dupl->format.size.width, height = dupl->format.size.height, stride, row_size, y;
    struct pw_buffer *buffer, *latest = NULL;
    const unsigned char *src;
    struct spa_data *plane;
    unsigned char *tmp;
    size_t size;

    /* Only the newest queued buffer matters. */
    while ((buffer = pw_stream_dequeue_buffer(dupl->stream)))
    {
        if (latest) pw_stream_queue_buffer(dupl->stream, latest);
        latest = buffer;
    }
    if (!latest)
        return;

    plane = &latest->buffer->datas[0];
    if (!plane->data || !plane->chunk || !plane->chunk->size || (plane->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)
            || !width || !height)
        goto done;

    row_size = width * 4;
    stride = plane->chunk->stride > 0 ? plane->chunk->stride : row_size;
    if (stride < row_size || plane->chunk->offset + (size_t)stride * (height - 1) + row_size > plane->maxsize)
    {
        WARN("Unexpected buffer layout, stride %u, size %u.\n", stride, plane->maxsize);
        goto done;
    }

    size = (size_t)row_size * height;
    if (dupl->back_size < size)
    {
        free(dupl->back);
        if (!(dupl->back = malloc(size)))
        {
            dupl->back_size = 0;
            goto done;
        }
        dupl->back_size = size;
    }

    src = (const unsigned char *)plane->data + plane->chunk->offset;
    if (stride == row_size)
        memcpy(dupl->back, src, size);
    else
        for (y = 0; y < height; ++y)
            memcpy(dupl->back + (size_t)y * row_size, src + (size_t)y * stride, row_size);

    pthread_mutex_lock(&dupl->lock);
    tmp = dupl->front;
    dupl->front = dupl->back;
    dupl->back = tmp;
    size = dupl->front_size;
    dupl->front_size = dupl->back_size;
    dupl->back_size = size;
    dupl->front_width = width;
    dupl->front_height = height;
    dupl->front_swap_rb = dupl->swap_rb;
    ++dupl->frame_seq;
    pthread_cond_broadcast(&dupl->cond);
    pthread_mutex_unlock(&dupl->lock);

done:
    pw_stream_queue_buffer(dupl->stream, latest);
}

static const struct pw_stream_events stream_events =
{
    PW_VERSION_STREAM_EVENTS,
    .state_changed = stream_state_changed,
    .param_changed = stream_param_changed,
    .process = stream_process,
};

static unsigned int get_max_fps(void)
{
    const char *env = getenv("WINE_DXGI_DUPLICATION_MAX_FPS");
    int fps = env ? atoi(env) : 0;

    return fps > 0 && fps <= 1000 ? fps : 30;
}

static NTSTATUS start_stream(struct dupl *dupl, UINT32 node_id, int pw_fd, UINT32 width, UINT32 height)
{
    struct spa_rectangle size = SPA_RECTANGLE(width, height), min_size = SPA_RECTANGLE(1, 1);
    struct spa_rectangle max_size = SPA_RECTANGLE(16384, 16384);
    struct spa_fraction rate = SPA_FRACTION(0, 1), max_rate = SPA_FRACTION(get_max_fps(), 1);
    struct spa_fraction min_rate = SPA_FRACTION(0, 1);
    struct spa_pod_builder builder;
    const struct spa_pod *params[1];
    uint8_t buffer[1024];
    int ret;

    if (!(dupl->loop = pw_thread_loop_new("wine-dxgi-duplication", NULL)))
    {
        close(pw_fd);
        return STATUS_UNSUCCESSFUL;
    }
    if (!(dupl->context = pw_context_new(pw_thread_loop_get_loop(dupl->loop), NULL, 0)))
    {
        close(pw_fd);
        return STATUS_UNSUCCESSFUL;
    }
    if (pw_thread_loop_start(dupl->loop) < 0)
    {
        close(pw_fd);
        return STATUS_UNSUCCESSFUL;
    }

    pw_thread_loop_lock(dupl->loop);

    if (!(dupl->core = pw_context_connect_fd(dupl->context, pw_fd, NULL, 0)))
    {
        WARN("Failed to connect to PipeWire.\n");
        pw_thread_loop_unlock(dupl->loop);
        return STATUS_UNSUCCESSFUL;
    }

    dupl->stream = pw_stream_new(dupl->core, "wine-dxgi-duplication",
            pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
                    PW_KEY_MEDIA_ROLE, "Screen", NULL));
    if (!dupl->stream)
    {
        pw_thread_loop_unlock(dupl->loop);
        return STATUS_UNSUCCESSFUL;
    }
    pw_stream_add_listener(dupl->stream, &dupl->stream_listener, &stream_events, dupl);

    builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    params[0] = spa_pod_builder_add_object(&builder,
            SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx,
                    SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&size, &min_size, &max_size),
            SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate),
            SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&max_rate, &min_rate, &max_rate));

    ret = pw_stream_connect(dupl->stream, PW_DIRECTION_INPUT, node_id,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    pw_thread_loop_unlock(dupl->loop);
    if (ret < 0)
    {
        WARN("Failed to connect PipeWire stream, error %d.\n", ret);
        return STATUS_UNSUCCESSFUL;
    }
    return STATUS_SUCCESS;
}

static void dupl_free(struct dupl *dupl)
{
    if (dupl->loop)
    {
        pw_thread_loop_lock(dupl->loop);
        if (dupl->stream) pw_stream_destroy(dupl->stream);
        if (dupl->core) pw_core_disconnect(dupl->core);
        pw_thread_loop_unlock(dupl->loop);
        pw_thread_loop_stop(dupl->loop);
        if (dupl->context) pw_context_destroy(dupl->context);
        pw_thread_loop_destroy(dupl->loop);
    }
    close_portal_session(dupl);
    pthread_cond_destroy(&dupl->cond);
    pthread_mutex_destroy(&dupl->lock);
    free(dupl->front);
    free(dupl->back);
    free(dupl);
}

static NTSTATUS dupl_create(void *args)
{
    struct dupl_create_params *params = args;
    pthread_condattr_t attr;
    struct dupl *dupl;
    UINT32 node_id;
    NTSTATUS status;
    int pw_fd = -1;

    init_libraries();

    pthread_mutex_lock(&portal_mutex);
    if (last_cancel && time(NULL) - last_cancel < CANCEL_COOLDOWN_SECONDS)
    {
        pthread_mutex_unlock(&portal_mutex);
        TRACE("Screen capture was cancelled recently, not asking again.\n");
        return STATUS_ACCESS_DENIED;
    }

    if (!(dupl = calloc(1, sizeof(*dupl))))
    {
        pthread_mutex_unlock(&portal_mutex);
        return STATUS_NO_MEMORY;
    }
    pthread_mutex_init(&dupl->lock, NULL);
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&dupl->cond, &attr);
    pthread_condattr_destroy(&attr);

    status = start_portal_session(dupl, params->key, &node_id, &pw_fd);
    pthread_mutex_unlock(&portal_mutex);
    if (!status)
        status = start_stream(dupl, node_id, pw_fd, params->width, params->height);

    if (status)
    {
        dupl_free(dupl);
        return status;
    }

    params->handle = (UINT_PTR)dupl;
    return STATUS_SUCCESS;
}

/* The fourth byte of BGRx and RGBx frames is undefined, so the alpha of
 * every copied pixel is set to opaque. */
static inline UINT32 convert_pixel(UINT32 pixel, BOOL swap_rb)
{
    if (swap_rb)
        pixel = (pixel & 0x0000ff00) | ((pixel & 0xff) << 16) | ((pixel >> 16) & 0xff);
    return pixel | 0xff000000;
}

static void copy_frame(const struct dupl *dupl, const struct dupl_acquire_params *params)
{
    UINT32 src_width = dupl->front_width, src_height = dupl->front_height;
    UINT32 dst_width = params->width, dst_height = params->height, x, y;
    BOOL transpose = params->rotation == DUPL_ROTATION_90 || params->rotation == DUPL_ROTATION_270;
    /* Size of the destination as the user sees it, i.e. with the rotation undone. */
    UINT32 view_width = transpose ? dst_height : dst_width;
    UINT32 view_height = transpose ? dst_width : dst_height;
    BOOL swap_rb = dupl->front_swap_rb, scale = src_width != view_width || src_height != view_height;
    const UINT32 *src = (const UINT32 *)dupl->front, *src_row;
    unsigned char *dst = params->buffer;
    UINT32 *dst_row;

    if (!scale && params->rotation == DUPL_ROTATION_0)
    {
        for (y = 0; y < dst_height; ++y)
        {
            dst_row = (UINT32 *)(dst + (size_t)y * params->pitch);
            src_row = src + (size_t)y * src_width;
            /* Separate loops keep the common case free of the swap test. */
            if (swap_rb)
                for (x = 0; x < dst_width; ++x)
                    dst_row[x] = convert_pixel(src_row[x], TRUE);
            else
                for (x = 0; x < dst_width; ++x)
                    dst_row[x] = convert_pixel(src_row[x], FALSE);
        }
        return;
    }

    if (!scale && params->rotation == DUPL_ROTATION_180)
    {
        for (y = 0; y < dst_height; ++y)
        {
            dst_row = (UINT32 *)(dst + (size_t)y * params->pitch);
            src_row = src + (size_t)(src_height - 1 - y) * src_width + src_width - 1;
            for (x = 0; x < dst_width; ++x)
                dst_row[x] = convert_pixel(src_row[-(ptrdiff_t)x], swap_rb);
        }
        return;
    }

    /* General case: rotate, scale with nearest neighbour and convert to BGRA.
     * (vx, vy) is the destination pixel's position on the desktop as the user
     * sees it, using the same mapping as the DXGI desktop duplication sample. */
    for (y = 0; y < dst_height; ++y)
    {
        dst_row = (UINT32 *)(dst + (size_t)y * params->pitch);
        for (x = 0; x < dst_width; ++x)
        {
            UINT32 vx, vy, sx, sy;

            switch (params->rotation)
            {
                case DUPL_ROTATION_90:  vx = view_width - 1 - y; vy = x; break;
                case DUPL_ROTATION_180: vx = view_width - 1 - x; vy = view_height - 1 - y; break;
                case DUPL_ROTATION_270: vx = y; vy = view_height - 1 - x; break;
                default:                vx = x; vy = y; break;
            }
            sx = vx * (UINT64)src_width / view_width;
            sy = vy * (UINT64)src_height / view_height;
            dst_row[x] = convert_pixel(src[(size_t)sy * src_width + sx], swap_rb);
        }
    }
}

static NTSTATUS dupl_acquire(void *args)
{
    const struct dupl_acquire_params *params = args;
    struct dupl *dupl = (struct dupl *)(UINT_PTR)params->handle;
    struct timespec deadline;
    NTSTATUS status = STATUS_SUCCESS;

    get_monotonic_time(&deadline);
    deadline.tv_sec += params->timeout_ms / 1000;
    deadline.tv_nsec += (params->timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_nsec -= 1000000000L;
        ++deadline.tv_sec;
    }

    pthread_mutex_lock(&dupl->lock);
    while (!dupl->failed && dupl->frame_seq == dupl->read_seq)
    {
        if (pthread_cond_timedwait(&dupl->cond, &dupl->lock, &deadline) == ETIMEDOUT)
        {
            status = STATUS_TIMEOUT;
            break;
        }
    }
    if (!status && dupl->failed)
        status = STATUS_CONNECTION_DISCONNECTED;
    if (!status)
    {
        copy_frame(dupl, params);
        dupl->read_seq = dupl->frame_seq;
    }
    pthread_mutex_unlock(&dupl->lock);
    return status;
}

static NTSTATUS dupl_destroy(void *args)
{
    const struct dupl_destroy_params *params = args;

    dupl_free((struct dupl *)(UINT_PTR)params->handle);
    return STATUS_SUCCESS;
}

#else  /* HAVE_DESKTOP_DUPLICATION */

static NTSTATUS dupl_create(void *args)
{
    FIXME("Desktop duplication needs PipeWire and D-Bus support at build time.\n");
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS dupl_acquire(void *args)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS dupl_destroy(void *args)
{
    return STATUS_SUCCESS;
}

#endif  /* HAVE_DESKTOP_DUPLICATION */

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    dupl_create,
    dupl_acquire,
    dupl_destroy,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count);

#ifdef _WIN64

typedef ULONG PTR32;

static NTSTATUS wow64_dupl_create(void *args)
{
    struct
    {
        UINT64 handle;
        PTR32 key;
        UINT32 width;
        UINT32 height;
    } *params32 = args;
    struct dupl_create_params params =
    {
        .key = ULongToPtr(params32->key),
        .width = params32->width,
        .height = params32->height,
    };
    NTSTATUS status;

    status = dupl_create(&params);
    params32->handle = params.handle;
    return status;
}

static NTSTATUS wow64_dupl_acquire(void *args)
{
    struct
    {
        UINT64 handle;
        PTR32 buffer;
        UINT32 timeout_ms;
        UINT32 width;
        UINT32 height;
        UINT32 pitch;
        UINT32 rotation;
    } const *params32 = args;
    struct dupl_acquire_params params =
    {
        .handle = params32->handle,
        .buffer = ULongToPtr(params32->buffer),
        .timeout_ms = params32->timeout_ms,
        .width = params32->width,
        .height = params32->height,
        .pitch = params32->pitch,
        .rotation = params32->rotation,
    };

    return dupl_acquire(&params);
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    wow64_dupl_create,
    wow64_dupl_acquire,
    dupl_destroy,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count);

#endif  /* _WIN64 */
