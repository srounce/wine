/*
 * Server-side serial port communications management
 *
 * Copyright (C) 1998 Alexandre Julliard
 * Copyright (C) 2000,2001 Mike McCormack
 *
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

#include "config.h"

#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#if defined(HAVE_SYS_EPOLL_H) && defined(HAVE_EPOLL_CREATE)
# include <sys/epoll.h>
# define USE_EPOLL
#endif
#ifdef HAVE_UTIME_H
#include <utime.h>
#endif

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/ntddser.h"

#include "file.h"
#include "handle.h"
#include "thread.h"
#include "request.h"

struct wait_req
{
    struct serial *serial;
    struct timeout_user *timeout;
};

static void serial_dump( struct object *obj, int verbose );
static struct fd *serial_get_fd( struct object *obj );
static int serial_close_handle( struct object *obj, struct process *process, obj_handle_t handle );
static void serial_destroy(struct object *obj);

static enum server_fd_type serial_get_fd_type( struct fd *fd );
static void serial_ioctl( struct fd *fd, ioctl_code_t code, struct async *async );
static void serial_queue_async( struct fd *fd, struct async *async, int type, int count );
static void serial_reselect_async( struct fd *fd, struct async_queue *queue );

static void serial_rx_poll_event( struct fd *fd, int event );

struct serial
{
    struct object       obj;
    struct fd          *fd;

    struct async_queue  wait_q; /* queue for asynchronous WAIT_ON_MASK */

    struct timeout_user *read_timer;
    SERIAL_TIMEOUTS     timeouts;
    unsigned int        eventmask;
    unsigned int        history;
    unsigned int        pending_write : 1;
    struct fd          *rx_fd;

    struct termios      original;

    /* FIXME: add dcb, comm status, handler module, sharing */
};

static const struct object_ops serial_ops =
{
    .size     = sizeof(struct serial),
    .type     = &file_type,
    .dump     = serial_dump,
    .get_fd   = serial_get_fd,
    .get_sync = default_fd_get_sync,
    .close_handle = serial_close_handle,
    .destroy  = serial_destroy,
};

static const struct fd_ops serial_fd_ops =
{
    .get_fd_type    = serial_get_fd_type,
    .get_file_info  = default_fd_get_file_info,
    .ioctl          = serial_ioctl,
    .queue_async    = serial_queue_async,
    .reselect_async = serial_reselect_async,
};

static const struct fd_ops serial_rx_fd_ops =
{
    .poll_event     = serial_rx_poll_event,
};

/* check if the given fd is a serial port */
int is_serial_fd( struct fd *fd )
{
    struct termios tios;

    return !tcgetattr( get_unix_fd(fd), &tios );
}

#ifdef USE_EPOLL
/* EV_RXCHAR reports the arrival of characters: EPOLLET signals each batch once */
static void serial_rx_init( struct serial *serial )
{
    struct epoll_event event = { .events = EPOLLIN | EPOLLET };
    int unix_fd;

    if ((unix_fd = epoll_create( 1 )) == -1) return;
    if (epoll_ctl( unix_fd, EPOLL_CTL_ADD, get_unix_fd( serial->fd ), &event ) == -1)
    {
        close( unix_fd );
        return;
    }
    serial->rx_fd = create_anonymous_fd( &serial_rx_fd_ops, unix_fd, &serial->obj, 0 );
}

static int serial_rx_update( struct serial *serial )
{
    struct epoll_event event;

    if (epoll_wait( get_unix_fd( serial->rx_fd ), &event, 1, 0 ) != 1) return 0;
    if (!(serial->eventmask & SERIAL_EV_RXCHAR)) return 0;
    serial->history |= SERIAL_EV_RXCHAR;
    return 1;
}
#else
static void serial_rx_init( struct serial *serial ) { }
static int serial_rx_update( struct serial *serial ) { return 0; }
#endif

static void serial_rx_poll_event( struct fd *fd, int event )
{
    struct serial *serial = get_fd_user( fd );

    if (serial_rx_update( serial )) async_wake_up( &serial->wait_q, STATUS_ALERTED );
}

static unsigned int serial_get_history( struct serial *serial )
{
    unsigned int history;

    if (serial->rx_fd)
    {
        serial_rx_update( serial );
        history = serial->history;
        serial->history = 0;
        return history;
    }

#ifdef TIOCINQ
    if (serial->eventmask & SERIAL_EV_RXCHAR)
    {
        int queue = 0;
        if (!ioctl( get_unix_fd( serial->fd ), TIOCINQ, &queue ) && queue) return SERIAL_EV_RXCHAR;
    }
#endif
    return 0;
}

/* create a serial object for a given fd */
struct object *create_serial( struct fd *fd, unsigned int access, unsigned int sharing )
{
    struct serial *serial;

    if (!(serial = alloc_object( &serial_ops ))) return NULL;

    serial->read_timer   = NULL;
    serial->eventmask    = 0;
    serial->history      = 0;
    serial->pending_write = 0;
    serial->rx_fd        = NULL;
    memset( &serial->timeouts, 0, sizeof(serial->timeouts) );
    init_async_queue( &serial->wait_q );
    serial->fd = (struct fd *)grab_object( fd );
    set_fd_user( fd, &serial_fd_ops, &serial->obj );
    serial_rx_init( serial );

#ifdef TIOCEXCL
    if ((access & (FILE_READ_DATA | FILE_WRITE_DATA | FILE_APPEND_DATA)) &&
        !(sharing & (FILE_SHARE_READ | FILE_SHARE_WRITE)))
        ioctl( get_unix_fd( fd ), TIOCEXCL );
#endif
    return &serial->obj;
}

static struct fd *serial_get_fd( struct object *obj )
{
    struct serial *serial = (struct serial *)obj;
    return (struct fd *)grab_object( serial->fd );
}

static int serial_close_handle( struct object *obj, struct process *process, obj_handle_t handle )
{
    struct serial *serial = (struct serial *)obj;

    /* closing the last handle cancels a pending wait */
    if (obj->handle_count == 1) async_wake_up( &serial->wait_q, STATUS_CANCELLED );
    return 1;
}

static void serial_destroy( struct object *obj)
{
    struct serial *serial = (struct serial *)obj;
    if (serial->read_timer) remove_timeout_user( serial->read_timer );
    free_async_queue( &serial->wait_q );
    if (serial->rx_fd) release_object( serial->rx_fd );
    release_object( serial->fd );
}

static void serial_dump( struct object *obj, int verbose )
{
    struct serial *serial = (struct serial *)obj;
    assert( obj->ops == &serial_ops );
    fprintf( stderr, "Port fd=%p mask=%x\n", serial->fd, serial->eventmask );
}

static struct serial *get_serial_obj( struct process *process, obj_handle_t handle, unsigned int access )
{
    return (struct serial *)get_handle_obj( process, handle, access, &serial_ops );
}

static enum server_fd_type serial_get_fd_type( struct fd *fd )
{
    return FD_TYPE_SERIAL;
}

#define WAIT_ON_MASK_POLL_INTERVAL -10000

static void free_wait_req( void *private )
{
    struct wait_req *req = private;

    if (req->timeout) remove_timeout_user( req->timeout );
    release_object( req->serial );
    free( req );
}

static void async_wait_timeout( void *private )
{
    struct wait_req *req = private;

    async_wake_up( &req->serial->wait_q, STATUS_ALERTED );
    req->timeout = add_timeout_user( WAIT_ON_MASK_POLL_INTERVAL, async_wait_timeout, req );
}

static void serial_ioctl( struct fd *fd, ioctl_code_t code, struct async *async )
{
    struct serial *serial = get_fd_user( fd );

    switch (code)
    {
    case IOCTL_SERIAL_GET_TIMEOUTS:
        if (get_reply_max_size() < sizeof(serial->timeouts))
        {
            set_error( STATUS_BUFFER_TOO_SMALL );
            return;
        }
        set_reply_data( &serial->timeouts, sizeof(serial->timeouts ));
        return;

    case IOCTL_SERIAL_SET_TIMEOUTS:
        if (get_req_data_size() < sizeof(serial->timeouts))
        {
            set_error( STATUS_BUFFER_TOO_SMALL );
            return;
        }
        memcpy( &serial->timeouts, get_req_data(), sizeof(serial->timeouts) );
        return;

    case IOCTL_SERIAL_GET_WAIT_MASK:
        if (get_reply_max_size() < sizeof(serial->eventmask))
        {
            set_error( STATUS_BUFFER_TOO_SMALL );
            return;
        }
        set_reply_data( &serial->eventmask, sizeof(serial->eventmask) );
        return;

    case IOCTL_SERIAL_SET_WAIT_MASK:
        if (get_req_data_size() < sizeof(serial->eventmask))
        {
            set_error( STATUS_BUFFER_TOO_SMALL );
            return;
        }
        serial->eventmask = *(unsigned int *)get_req_data();
        if (serial->rx_fd)
        {
            /* setting the mask clears the event history, even for the events kept */
            serial_rx_update( serial );
            set_fd_events( serial->rx_fd, (serial->eventmask & SERIAL_EV_RXCHAR) ? POLLIN : 0 );
        }
        serial->history = 0;
        /* a pending wait completes successfully, with no events */
        async_wake_up( &serial->wait_q, STATUS_SUCCESS );
        return;

    case IOCTL_SERIAL_WAIT_ON_MASK:
    {
        struct wait_req *req;

        if (async_queued( &serial->wait_q ))
        {
            set_error( STATUS_INVALID_PARAMETER );
            return;
        }

        if (!(req = mem_alloc(sizeof(*req))))
            return;

        req->serial = (struct serial *)grab_object( serial );
        if (!(req->timeout = add_timeout_user( WAIT_ON_MASK_POLL_INTERVAL, async_wait_timeout, req )))
        {
            free( req );
            return;
        }

        async_set_completion_callback( async, free_wait_req, req );
        queue_async( &serial->wait_q, async );
        set_error( STATUS_ALERTED );
        return;
    }

    default:
        set_error( STATUS_NOT_SUPPORTED );
    }
}

static void serial_queue_async( struct fd *fd, struct async *async, int type, int count )
{
    struct serial *serial = get_fd_user( fd );
    timeout_t timeout = 0;

    assert(serial->obj.ops == &serial_ops);

    switch (type)
    {
    case ASYNC_TYPE_READ:
        timeout = serial->timeouts.ReadTotalTimeoutConstant +
            (timeout_t)serial->timeouts.ReadTotalTimeoutMultiplier * count;
        break;
    case ASYNC_TYPE_WRITE:
        timeout = serial->timeouts.WriteTotalTimeoutConstant +
            (timeout_t)serial->timeouts.WriteTotalTimeoutMultiplier * count;
        break;
    }

    fd_queue_async( fd, async, type );
    if (timeout) async_set_timeout( async, timeout * -10000, STATUS_TIMEOUT );
    set_error( STATUS_PENDING );
}

static void serial_read_timeout( void *arg )
{
    struct serial *serial = arg;

    serial->read_timer = NULL;
    fd_async_wake_up( serial->fd, ASYNC_TYPE_READ, STATUS_TIMEOUT );
}

static void serial_reselect_async( struct fd *fd, struct async_queue *queue )
{
    struct serial *serial = get_fd_user( fd );

    if (serial->read_timer)
    {
        if (!(default_fd_get_poll_events( fd ) & POLLIN))
        {
            remove_timeout_user( serial->read_timer );
            serial->read_timer = NULL;
        }
    }
    else if (serial->timeouts.ReadIntervalTimeout && (default_fd_get_poll_events( fd ) & POLLIN))
    {
        serial->read_timer = add_timeout_user( (timeout_t)serial->timeouts.ReadIntervalTimeout * -10000,
                                               serial_read_timeout, serial );
    }
    default_fd_reselect_async( fd, queue );
}

DECL_HANDLER(get_serial_info)
{
    struct serial *serial;

    if ((serial = get_serial_obj( current->process, req->handle, 0 )))
    {
        /* event mask */
        reply->eventmask    = serial->eventmask;

        /* event history */
        if (req->flags & SERIALINFO_HISTORY)
            reply->history = serial_get_history( serial );

        /* pending write */
        reply->pending_write = serial->pending_write;
        if (req->flags & SERIALINFO_PENDING_WRITE)
            serial->pending_write = 0;

        release_object( serial );
    }
}

DECL_HANDLER(set_serial_info)
{
    struct serial *serial;

    if ((serial = get_serial_obj( current->process, req->handle, 0 )))
    {
        if (req->flags & SERIALINFO_PENDING_WRITE)
            serial->pending_write = 1;

        release_object( serial );
    }
}
