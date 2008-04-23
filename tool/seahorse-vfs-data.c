/*
 * Seahorse
 *
 * Copyright (C) 2004 Stefan Walter
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#include <gtk/gtk.h>
#include <libgnomevfs/gnome-vfs.h>

#include "seahorse-gpgmex.h"
#include "seahorse-vfs-data.h"
#include "seahorse-util.h"

#define PROGRESS_BLOCK  16 * 1024

/* The current/last VFS operation */
typedef enum _VfsAsyncOp
{
    VFS_OP_NONE,
    VFS_OP_OPENING,
    VFS_OP_READING,
    VFS_OP_WRITING,
    VFS_OP_SEEKING
}
VfsAsyncOp;

/* The state of the VFS system */
typedef enum _VfsAsyncState
{
    VFS_ASYNC_PROCESSING,
    VFS_ASYNC_CANCELLED,
    VFS_ASYNC_READY
}
VfsAsyncState;

/* A handle for VFS work */
typedef struct _VfsAsyncHandle {

    gpgme_data_t gdata;             /* A pointer to the outside gpgme_data_t handle */

    gchar *uri;                     /* The URI we're operating on */
    GnomeVFSAsyncHandle* handle;    /* Handle for operations */
    GtkWidget* widget;              /* Widget for progress and cancel */

    VfsAsyncOp    operation;        /* The last/current operation */
    VfsAsyncState state;            /* State of the last/current operation */

    GnomeVFSResult result;          /* Result of the current operation */
    gpointer buffer;                /* Current operation's buffer */
    GnomeVFSFileSize processed;     /* Number of bytes processed in current op */

    GnomeVFSFileSize last;          /* Last update sent about number of bytes */
    GnomeVFSFileSize total;         /* Total number of bytes read or written */

    SeahorseVfsProgressCb progcb;   /* Progress callback */
    gpointer userdata;              /* User data for progress callback */
} VfsAsyncHandle;

static void vfs_data_cancel(VfsAsyncHandle *ah);

/* Waits for a given VFS operation to complete without halting the UI */
static gboolean
vfs_data_wait_results(VfsAsyncHandle* ah, gboolean errors)
{
    VfsAsyncOp op;

    seahorse_util_wait_until (ah->state != VFS_ASYNC_PROCESSING);

    op = ah->operation;
    ah->operation = VFS_OP_NONE;

    /* Cancelling looks like an error to our caller */
    if(ah->state == VFS_ASYNC_CANCELLED)
    {
        errno = 0;
        return FALSE;
    }

    g_assert(ah->state == VFS_ASYNC_READY);

    /* There was no operation going on, result codes are not relevant */
    if(op == VFS_OP_NONE)
        return TRUE;

    if(ah->result == GNOME_VFS_ERROR_EOF)
    {
        ah->processed = 0;
        ah->result = GNOME_VFS_OK;
    }

    else if(ah->result == GNOME_VFS_ERROR_CANCELLED)
    {
        vfs_data_cancel(ah);
        errno = 0;
        return FALSE;
    }

    /* Check for error codes */
    if(ah->result != GNOME_VFS_OK)
    {
        if(errors)
        {
            switch(ah->result)
            {
            #define VFS_TO_SYS_ERR(v, s)    \
                case v: errno = s; break;
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NOT_FOUND, ENOENT);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_GENERIC, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_INTERNAL, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_BAD_PARAMETERS, EINVAL);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NOT_SUPPORTED, EOPNOTSUPP);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_IO, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_CORRUPTED_DATA, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_WRONG_FORMAT, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_BAD_FILE, EBADF);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_TOO_BIG, EFBIG);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NO_SPACE, ENOSPC);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_READ_ONLY, EPERM);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_INVALID_URI, EINVAL);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NOT_OPEN, EBADF);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_INVALID_OPEN_MODE, EPERM);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_ACCESS_DENIED, EACCES);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_TOO_MANY_OPEN_FILES, EMFILE);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NOT_A_DIRECTORY, ENOTDIR);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_IN_PROGRESS, EALREADY);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_INTERRUPTED, EINTR);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_FILE_EXISTS, EEXIST);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_LOOP, ELOOP);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NOT_PERMITTED, EPERM);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_IS_DIRECTORY, EISDIR);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NO_MEMORY, ENOMEM);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_HOST_NOT_FOUND, ENOENT);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_INVALID_HOST_NAME, EHOSTDOWN);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_HOST_HAS_NO_ADDRESS, EHOSTUNREACH);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_LOGIN_FAILED, EACCES);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_DIRECTORY_BUSY, EBUSY);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_DIRECTORY_NOT_EMPTY, ENOTEMPTY);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_TOO_MANY_LINKS, EMLINK);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_READ_ONLY_FILE_SYSTEM, EROFS);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NOT_SAME_FILE_SYSTEM, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NAME_TOO_LONG, ENAMETOOLONG);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_SERVICE_NOT_AVAILABLE, ENOPROTOOPT);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_SERVICE_OBSOLETE, ENOPROTOOPT);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_PROTOCOL_ERROR, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NO_MASTER_BROWSER, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NO_DEFAULT, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_NO_HANDLER, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_PARSE, EIO);
            VFS_TO_SYS_ERR(GNOME_VFS_ERROR_LAUNCH, EIO);

            default:
                errno = EIO;
                break;
            }

            /* When errors on open we look cancelled */
            if(op == VFS_OP_OPENING)
                ah->state = VFS_ASYNC_CANCELLED;
        }

        return FALSE;
    }

    return TRUE;
}

/* Called when a file open completes */
static void
vfs_data_open_done(GnomeVFSAsyncHandle *handle, GnomeVFSResult result,
                        gpointer callback_data)
{
    VfsAsyncHandle* ah = (VfsAsyncHandle*)callback_data;

    if(ah->state == VFS_ASYNC_PROCESSING)
    {
        g_assert(handle == ah->handle);
        g_assert(ah->operation == VFS_OP_OPENING);

        ah->result = result;
        ah->state = VFS_ASYNC_READY;
        ah->total = 0;
        ah->last = 0;
    }
}

/* Open the given handle */
static void
vfs_data_open_helper (VfsAsyncHandle *ah, gboolean write)
{
    g_assert (ah->handle == NULL);
    g_assert (ah->uri != NULL);
    g_assert (ah->state == VFS_ASYNC_READY);

    if (write) {
        /* Note that we always overwrite the file */
        gnome_vfs_async_create (&(ah->handle), ah->uri, GNOME_VFS_OPEN_WRITE | GNOME_VFS_OPEN_RANDOM,
                    FALSE, 0600, GNOME_VFS_PRIORITY_DEFAULT, vfs_data_open_done, ah);
    } else {
        gnome_vfs_async_open (&(ah->handle), ah->uri, GNOME_VFS_OPEN_READ | GNOME_VFS_OPEN_RANDOM,
                    GNOME_VFS_PRIORITY_DEFAULT, vfs_data_open_done, ah);
    }

    ah->state = VFS_ASYNC_PROCESSING;
    ah->operation = VFS_OP_OPENING;
}

/* Open the given URI */
static VfsAsyncHandle*
vfs_data_open(const gchar* uri, gboolean write, gboolean delayed)
{
    VfsAsyncHandle* ah;

    /* We only support delayed opening of write files */
    g_assert (write || !delayed);

    ah = g_new0(VfsAsyncHandle, 1);
    ah->state = VFS_ASYNC_READY;
    ah->operation = VFS_OP_NONE;
    ah->uri = g_strdup (uri);

    /* Open the file right here and now if requested */
    if (!delayed)
        vfs_data_open_helper (ah, write);

    return ah;
}

/* Called when a read completes */
static void
vfs_data_read_done(GnomeVFSAsyncHandle *handle, GnomeVFSResult result, gpointer buffer,
        GnomeVFSFileSize bytes_requested, GnomeVFSFileSize bytes_read, gpointer callback_data)
{
    VfsAsyncHandle* ah = (VfsAsyncHandle*)callback_data;

    if(ah->state == VFS_ASYNC_PROCESSING)
    {
        g_assert(handle == ah->handle);
        g_assert(buffer == ah->buffer);
        g_assert(ah->operation == VFS_OP_READING);

        ah->result = result;
        ah->processed = bytes_read;
        ah->state = VFS_ASYNC_READY;
        ah->total += bytes_read;

        /* Call progress callback if setup */
        if (ah->progcb && ah->total >= ah->last + PROGRESS_BLOCK)
            (ah->progcb) (ah->gdata, ah->total, ah->userdata);
    }
}

/* Called by gpgme to read data */
static ssize_t
vfs_data_read(void *handle, void *buffer, size_t size)
{
    VfsAsyncHandle* ah = (VfsAsyncHandle*)handle;
    ssize_t sz = 0;

    g_assert (ah->handle != NULL);

    /* Just in case we have an operation, like open */
    if(!vfs_data_wait_results(ah, TRUE))
        return -1;

    g_assert(ah->state == VFS_ASYNC_READY);

    /* Start async operation */
    ah->buffer = buffer;
    ah->state = VFS_ASYNC_PROCESSING;
    ah->operation = VFS_OP_READING;
    gnome_vfs_async_read(ah->handle, buffer, size, vfs_data_read_done, ah);

    /* Wait for it */
    if(!vfs_data_wait_results(ah, TRUE))
        return -1;

    /* Return results */
    sz = (ssize_t)ah->processed;
    ah->state = VFS_ASYNC_READY;

    ah->buffer = NULL;
    ah->processed = 0;

    return sz;
}

/* Called when a write completes */
static void
vfs_data_write_done(GnomeVFSAsyncHandle *handle, GnomeVFSResult result, gconstpointer buffer,
        GnomeVFSFileSize bytes_requested, GnomeVFSFileSize bytes_written, gpointer callback_data)
{
    VfsAsyncHandle* ah = (VfsAsyncHandle*)callback_data;

    if(ah->state == VFS_ASYNC_PROCESSING)
    {
        g_assert(handle == ah->handle);
        g_assert(buffer == ah->buffer);
        g_assert(ah->operation == VFS_OP_WRITING);

        ah->result = result;
        ah->processed = bytes_written;
        ah->state = VFS_ASYNC_READY;
        ah->total += bytes_written;

        /* Call progress callback if setup */
        if (ah->progcb && ah->total >= ah->last + PROGRESS_BLOCK)
            (ah->progcb) (ah->gdata, ah->total, ah->userdata);
    }
}

/* Called by gpgme to write data */
static ssize_t
vfs_data_write(void *handle, const void *buffer, size_t size)
{
    VfsAsyncHandle* ah = (VfsAsyncHandle*)handle;
    ssize_t sz = 0;

    /* If the file isn't open yet, then do that now */
    if (!ah->handle && ah->state == VFS_ASYNC_READY)
        vfs_data_open_helper (ah, TRUE);

    /* Just in case we have an operation, like open */
    if(!vfs_data_wait_results(ah, TRUE))
        return -1;

    g_assert(ah->state == VFS_ASYNC_READY);

    /* Start async operation */
    ah->buffer = (gpointer)buffer;
    ah->state = VFS_ASYNC_PROCESSING;
    ah->operation = VFS_OP_WRITING;
    gnome_vfs_async_write(ah->handle, buffer, size, vfs_data_write_done, ah);

    /* Wait for it */
    if(!vfs_data_wait_results(ah, TRUE))
        return -1;

    /* Return results */
    sz = (ssize_t)ah->processed;
    ah->state = VFS_ASYNC_READY;

    ah->buffer = NULL;
    ah->processed = 0;

    return sz;
}

/* Called when a VFS seek completes */
static void
vfs_data_seek_done(GnomeVFSAsyncHandle *handle, GnomeVFSResult result,
                        gpointer callback_data)
{
    VfsAsyncHandle* ah = (VfsAsyncHandle*)callback_data;

    if(ah->state == VFS_ASYNC_PROCESSING)
    {
        g_assert(handle == ah->handle);
        g_assert(ah->operation == VFS_OP_SEEKING);

        ah->result = result;
        ah->state = VFS_ASYNC_READY;
    }
}

/* Called from gpgme to seek a file */
static off_t
vfs_data_seek(void *handle, off_t offset, int whence)
{
    VfsAsyncHandle* ah = (VfsAsyncHandle*)handle;
    GnomeVFSSeekPosition wh;

    /* If the file isn't open yet, then do that now */
    if (!ah->handle && ah->state == VFS_ASYNC_READY)
        vfs_data_open_helper (ah, TRUE);

    /* Just in case we have an operation, like open */
    if(!vfs_data_wait_results(ah, TRUE))
        return (off_t)-1;

    g_assert(ah->state == VFS_ASYNC_READY);

    switch(whence)
    {
    case SEEK_SET:
        wh = GNOME_VFS_SEEK_START;
        break;
    case SEEK_CUR:
        wh = GNOME_VFS_SEEK_CURRENT;
        break;
    case SEEK_END:
        wh = GNOME_VFS_SEEK_END;
        break;
    default:
        g_assert_not_reached();
        break;
    }

    /* Start async operation */
    ah->state = VFS_ASYNC_PROCESSING;
    ah->operation = VFS_OP_SEEKING;
    gnome_vfs_async_seek(ah->handle, wh, (GnomeVFSFileOffset)offset,
                            vfs_data_seek_done, ah);

    /* Wait for it */
    if(!vfs_data_wait_results(ah, TRUE))
        return -1;

    /* Return results */
    ah->state = VFS_ASYNC_READY;
    return offset;
}

/* Dummy callback for closing a file */
static void
vfs_data_close_done(GnomeVFSAsyncHandle *handle, GnomeVFSResult result,
                        gpointer callback_data)
{
    /* A noop, we just let this go */
}

/* Called to cancel the current operation.
 * Puts the async handle into a cancelled state. */
static void
vfs_data_cancel(VfsAsyncHandle *ah)
{
    gboolean close = FALSE;

    if (ah->handle) {
        switch (ah->state) {
        case VFS_ASYNC_CANCELLED:
            break;

        case VFS_ASYNC_PROCESSING:
            close = (ah->operation != VFS_OP_OPENING);
            gnome_vfs_async_cancel (ah->handle);
            break;

        case VFS_ASYNC_READY:
            close = TRUE;
            break;
        };
    }

    if (close) {
        gnome_vfs_async_close (ah->handle, vfs_data_close_done, NULL);
        ah->handle = NULL;
    }

    ah->state = VFS_ASYNC_CANCELLED;
}

/* Called by gpgme to close a file */
static void
vfs_data_release(void *handle)
{
    VfsAsyncHandle* ah = (VfsAsyncHandle*)handle;

    vfs_data_cancel (ah);
    g_free (ah->uri);
    g_free (ah);
}

/* GPGME vfs file operations */
static struct gpgme_data_cbs vfs_data_cbs =
{
    vfs_data_read,
    vfs_data_write,
    vfs_data_seek,
    vfs_data_release
};

/* -----------------------------------------------------------------------------
 */

/* Create a data on the given uri, remote uris get gnome-vfs backends,
 * local uris get normal file access. */
static gpgme_data_t
create_vfs_data (const gchar *uri, guint mode, SeahorseVfsProgressCb progcb,
                 gpointer userdata, gpg_error_t* err)
{
    gpgme_error_t gerr;
    gpgme_data_t ret = NULL;
    VfsAsyncHandle* handle = NULL;
    char* ruri;

    if (!err)
        err = &gerr;

    ruri = gnome_vfs_make_uri_canonical (uri);

    handle = vfs_data_open (ruri, mode & SEAHORSE_VFS_WRITE,
                                  mode & SEAHORSE_VFS_DELAY);
    if (handle) {

        *err = gpgme_data_new_from_cbs (&ret, &vfs_data_cbs, handle);
        if (!GPG_IS_OK (*err)) {
            vfs_data_cbs.release (handle);
            ret = NULL;
        }

        handle->progcb = progcb;
        handle->userdata = userdata;
        handle->gdata = ret;
    }

    g_free (ruri);

    return ret;
}


gpgme_data_t
seahorse_vfs_data_create (const gchar *uri, guint mode, GError **err)
{
    return seahorse_vfs_data_create_full (uri, mode, NULL, NULL, err);
}

gpgme_data_t
seahorse_vfs_data_create_full (const gchar *uri, guint mode, SeahorseVfsProgressCb progcb,
                               gpointer userdata, GError **err)
{
    gpgme_data_t data;
    gpgme_error_t gerr;

    g_return_val_if_fail (!err || !*err, NULL);
    data = create_vfs_data (uri, mode, progcb, userdata, &gerr);
    if (!data)
        seahorse_util_gpgme_to_error (gerr, err);
    return data;
}

gpgme_data_t
seahorse_vfs_data_create_gerr (const gchar *uri, guint mode, gpg_error_t* err)
{
    return create_vfs_data (uri, mode, NULL, NULL, err);
}

gboolean
seahorse_vfs_set_file_contents (const gchar *uri, const gchar *text, gint len,
                                GError **err)
{
    gpgme_data_t data;
    gboolean ret;

    data = seahorse_vfs_data_create (uri, SEAHORSE_VFS_WRITE, err);
    if (!data)
        return FALSE;

    ret = seahorse_vfs_data_write_all (data, (void*)text, len, err);

    gpgme_data_release (data);
    return ret;
}

gboolean
seahorse_vfs_data_write_all (gpgme_data_t data, const void* buffer, gint len, GError **err)
{
    guchar *text = (guchar*)buffer;
    gint written;

    if (len < 0)
        len = strlen ((gchar*)text);

    while (len > 0) {
        written = gpgme_data_write (data, (void*)text, len);
        if (written < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            g_set_error (err, G_FILE_ERROR, g_file_error_from_errno (errno),
                         "%s", strerror (errno));
            return FALSE;
        }

        len -= written;
        text += written;
    }

    return TRUE;
}

/* -----------------------------------------------------------------------------
 * MULTIPLE CONCATED FILES
 */

/* Called by gpgme to read data */
static ssize_t
multi_data_read (void *handle, void *buffer, size_t size)
{
    gpgme_data_t data = NULL;
    ssize_t sz = 0;
    GList *datas, *l;
    guchar *buf;

    datas = (GList*)handle;
    buf = (guchar*)buffer;

    /* Find first non-null data thingy */
    for (l = datas; l; l = g_list_next (l)) {
        data = (gpgme_data_t)l->data;
        if (data)
            break;
    }

    while (size > 0 && data) {
        sz = gpgme_data_read (data, buf, size);

        /* An error */
        if (sz < 0)
            return sz;

        if (sz > size)
            sz = size;

        /* Free current data, get next one */
        if (sz < size) {
            l->data = NULL;
            gpgmex_data_release (data);
            l = g_list_next (l);
            data = (gpgme_data_t)(l ? l->data : NULL);
        }

        /* Read later in buffer */
        size -= sz;
        buf += sz;
    }

    return sz;
}

/* Called by gpgme to write data */
static ssize_t
multi_data_write (void *handle, const void *buffer, size_t size)
{
    /* No writing */
    errno = EBADF;
    return -1;
}

/* Called from gpgme to seek a file */
static off_t
multi_data_seek(void *handle, off_t offset, int whence)
{
    /* No real seeking */
    return offset;
}

/* Called by gpgme to close a file */
static void
multi_data_release(void *handle)
{
    GList *datas = (GList*)handle;
    GList *l;

    for (l = datas; l; l = g_list_next (l))
        gpgmex_data_release ((gpgme_data_t)l->data);
    g_list_free (datas);
}

static struct gpgme_data_cbs multi_data_cbs =
{
    multi_data_read,
    multi_data_write,
    multi_data_seek,
    multi_data_release
};

gpgme_data_t
seahorse_vfs_data_read_multi (const gchar **uris, GError **err)
{
    gpgme_error_t gerr;
    gpgme_data_t data;
    GList *datas = NULL;
    const gchar **u;

    for (u = uris; *u; u++) {
        if(!(*u)[0])
            continue;
        data = seahorse_vfs_data_create (*u, SEAHORSE_VFS_READ, err);
        if (!data)
            break;
        datas = g_list_prepend (datas, data);
    }

    /* Failed somewhere */
    if (!data) {
        multi_data_release (datas);
        return NULL;
    }

    datas = g_list_reverse (datas);
    gerr = gpgme_data_new_from_cbs (&data, &multi_data_cbs, datas);
    if (!GPG_IS_OK (gerr)) {
        seahorse_util_gpgme_to_error (gerr, err);
        multi_data_release (datas);
        return NULL;
    }

    return data;
}
