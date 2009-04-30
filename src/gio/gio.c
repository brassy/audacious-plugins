/*  Audacious
 *  Copyright (c) 2009 William Pitcock
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include <audacious/plugin.h>
#include <stdio.h>
#include <gio/gio.h>

#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <string.h>

typedef struct {
    GFile *file;
    GFileInputStream *istream;
    GFileOutputStream *ostream;
    GSeekable *seekable;
    GSList *stream_stack;
} VFSGIOHandle;

static GVfs *gvfs = NULL;

VFSFile *
gio_aud_vfs_fopen_impl(const gchar *path, const gchar *mode)
{
    VFSFile *file;
    VFSGIOHandle *handle;
    GError *error = NULL;

    if (path == NULL || mode == NULL)
	    return NULL;

    handle = g_slice_new0(VFSGIOHandle);
    handle->file = g_vfs_get_file_for_uri(gvfs, path);

    if (*mode == 'r')
    {
        handle->istream = g_file_read(handle->file, NULL, &error);
        handle->seekable = G_SEEKABLE(handle->istream);
    }
    else if (*mode == 'w')
    {
        handle->ostream = g_file_replace(handle->file, NULL, FALSE, G_FILE_CREATE_NONE, NULL, &error);
        handle->seekable = G_SEEKABLE(handle->ostream);
    }
    else
    {
        g_warning("UNSUPPORTED ACCESS MODE: %s", mode);
        g_object_unref(handle->file);
        g_slice_free(VFSGIOHandle, handle);
        return NULL;
    }

    if (handle->istream == NULL && handle->ostream == NULL)
    {
        g_warning("Could not open %s for reading or writing: %s", path, error->message);
        g_object_unref(handle->file);
        g_slice_free(VFSGIOHandle, handle);
        g_error_free(error);
        return NULL;
    }

    file = g_new(VFSFile, 1);
    file->handle = handle;

    return file;
}

gint
gio_aud_vfs_fclose_impl(VFSFile * file)
{
    gint ret = 0;

    g_return_val_if_fail(file != NULL, -1);

    if (file->handle)
    {
        VFSGIOHandle *handle = (VFSGIOHandle *) file->handle;

        g_object_unref(handle->file);
        g_slice_free(VFSGIOHandle, handle);

        file->handle = NULL;
    }

    return ret;
}

size_t
gio_aud_vfs_fread_impl(gpointer ptr,
          size_t size,
          size_t nmemb,
          VFSFile * file)
{
    VFSGIOHandle *handle;
    goffset count = 0;
    gsize realsize = (size * nmemb);
    gsize ret;

    g_return_val_if_fail(file != NULL, EOF);
    g_return_val_if_fail(file->handle != NULL, EOF);

    handle = (VFSGIOHandle *) file->handle;

    /* handle ungetc() *grumble* --nenolod */
    if (handle->stream_stack != NULL)
    {
        guchar uc;
        while ((count < realsize) && (handle->stream_stack != NULL))
        {
            uc = GPOINTER_TO_INT(handle->stream_stack->data);
            handle->stream_stack = g_slist_delete_link(handle->stream_stack, handle->stream_stack);
            memcpy(ptr + count, &uc, 1);
            count++;
        }
    }

    ret = (g_input_stream_read(G_INPUT_STREAM(handle->istream), (ptr + count), (realsize - count), NULL, NULL) + count);
    return (ret / size);
}

size_t
gio_aud_vfs_fwrite_impl(gconstpointer ptr,
           size_t size,
           size_t nmemb,
           VFSFile * file)
{
    VFSGIOHandle *handle;
    gsize ret;

    g_return_val_if_fail(file != NULL, EOF);
    g_return_val_if_fail(file->handle != NULL, EOF);

    handle = (VFSGIOHandle *) file->handle;

    ret = g_output_stream_write(G_OUTPUT_STREAM(handle->ostream), ptr, size * nmemb, NULL, NULL);
    return (ret / size);
}

gint
gio_aud_vfs_getc_impl(VFSFile *file)
{
    guchar buf;
    VFSGIOHandle *handle;

    g_return_val_if_fail(file != NULL, EOF);
    g_return_val_if_fail(file->handle != NULL, EOF);

    handle = (VFSGIOHandle *) file->handle;

    if (handle->stream_stack != NULL)
    {
        buf = GPOINTER_TO_INT(handle->stream_stack->data);
        handle->stream_stack = g_slist_delete_link(handle->stream_stack, handle->stream_stack);
        return buf;
    }
    else if (g_input_stream_read(G_INPUT_STREAM(handle->istream), &buf, 1, NULL, NULL) != 1)
        return EOF;

    return buf;
}

gint
gio_aud_vfs_ungetc_impl(gint c, VFSFile * file)
{
    VFSGIOHandle *handle;

    g_return_val_if_fail(file != NULL, EOF);
    g_return_val_if_fail(file->handle != NULL, EOF);

    handle = (VFSGIOHandle *) file->handle;
    handle->stream_stack = g_slist_prepend(handle->stream_stack, GINT_TO_POINTER(c));
    if (handle->stream_stack != NULL)
        return c;

    return EOF;
}

gint
gio_aud_vfs_fseek_impl(VFSFile * file,
          glong offset,
          gint whence)
{
    VFSGIOHandle *handle;
    GSeekType seektype;

    g_return_val_if_fail(file != NULL, -1);
    g_return_val_if_fail(file->handle != NULL, -1);

    handle = (VFSGIOHandle *) file->handle;

    if (!g_seekable_can_seek(handle->seekable))
        return -1;

    if (handle->stream_stack != NULL)
    {
        g_slist_free(handle->stream_stack);
        handle->stream_stack = NULL;
    }

    switch (whence)
    {
    case SEEK_CUR:
        seektype = G_SEEK_CUR;
        break;
    case SEEK_END:
        seektype = G_SEEK_END;
        break;
    default:
        seektype = G_SEEK_SET;
        break;
    }

    return (g_seekable_seek(handle->seekable, offset, seektype, NULL, NULL) ? 0 : -1);
}

void
gio_aud_vfs_rewind_impl(VFSFile * file)
{
    g_return_if_fail(file != NULL);

    file->base->vfs_fseek_impl(file, 0, SEEK_SET);
}

glong
gio_aud_vfs_ftell_impl(VFSFile * file)
{
    VFSGIOHandle *handle;

    g_return_val_if_fail(file != NULL, -1);
    g_return_val_if_fail(file->handle != NULL, -1);

    handle = (VFSGIOHandle *) file->handle;

    return (glong) (g_seekable_tell(handle->seekable) - g_slist_length(handle->stream_stack));
}

gboolean
gio_aud_vfs_feof_impl(VFSFile * file)
{
    g_return_val_if_fail(file != NULL, TRUE);

    return (file->base->vfs_ftell_impl(file) == file->base->vfs_fsize_impl(file));
}

gint
gio_aud_vfs_truncate_impl(VFSFile * file, glong size)
{
    VFSGIOHandle *handle;

    g_return_val_if_fail(file != NULL, -1);

    handle = (VFSGIOHandle *) file->handle;

    return g_seekable_truncate(handle->seekable, size, NULL, NULL);
}

off_t
gio_aud_vfs_fsize_impl(VFSFile * file)
{
    GFileInfo *info;
    VFSGIOHandle *handle;
    GError *error = NULL;
    goffset size;

    g_return_val_if_fail(file != NULL, -1);
    g_return_val_if_fail(file->handle != NULL, -1);

    handle = (VFSGIOHandle *) file->handle;
    info = g_file_query_info(handle->file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, &error);

    if (info == NULL)
    {
        g_warning("gio fsize(): error: %s", error->message);
        g_error_free(error);
        return -1;
    }

    size = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_STANDARD_SIZE);
    g_object_unref(info);

    return size;
}

VFSConstructor file_const = {
    .uri_id = "file://",
    .vfs_fopen_impl = gio_aud_vfs_fopen_impl,
    .vfs_fclose_impl = gio_aud_vfs_fclose_impl,
    .vfs_fread_impl = gio_aud_vfs_fread_impl,
    .vfs_fwrite_impl = gio_aud_vfs_fwrite_impl,
    .vfs_getc_impl = gio_aud_vfs_getc_impl,
    .vfs_ungetc_impl = gio_aud_vfs_ungetc_impl,
    .vfs_fseek_impl = gio_aud_vfs_fseek_impl,
    .vfs_rewind_impl = gio_aud_vfs_rewind_impl,
    .vfs_ftell_impl = gio_aud_vfs_ftell_impl,
    .vfs_feof_impl = gio_aud_vfs_feof_impl,
    .vfs_truncate_impl = gio_aud_vfs_truncate_impl,
    .vfs_fsize_impl = gio_aud_vfs_fsize_impl
};

static void init(void)
{
    gint i;
    const gchar * const *schemes;

    gvfs = g_vfs_get_default();
    schemes = g_vfs_get_supported_uri_schemes(gvfs);

    aud_vfs_register_transport(&file_const);

    for (i = 0; schemes[i] != NULL; i++) {
         VFSConstructor *c;
         if (!g_ascii_strcasecmp(schemes[i], "http") || !g_ascii_strcasecmp(schemes[i], "file"))
             continue;

         g_print("GVfs supports %s - registering it\n", schemes[i]);

         c = g_slice_new0(VFSConstructor);
         c->uri_id = g_strdup_printf("%s://", schemes[i]);
         c->vfs_fopen_impl = gio_aud_vfs_fopen_impl;
         c->vfs_fclose_impl = gio_aud_vfs_fclose_impl;
         c->vfs_fread_impl = gio_aud_vfs_fread_impl;
         c->vfs_fwrite_impl = gio_aud_vfs_fwrite_impl;
         c->vfs_getc_impl = gio_aud_vfs_getc_impl;
         c->vfs_ungetc_impl = gio_aud_vfs_ungetc_impl;
         c->vfs_fseek_impl = gio_aud_vfs_fseek_impl;
         c->vfs_rewind_impl = gio_aud_vfs_rewind_impl;
         c->vfs_ftell_impl = gio_aud_vfs_ftell_impl;
         c->vfs_feof_impl = gio_aud_vfs_feof_impl;
         c->vfs_truncate_impl = gio_aud_vfs_truncate_impl;
         c->vfs_fsize_impl = gio_aud_vfs_fsize_impl;
         aud_vfs_register_transport(c);
    }
}

static void cleanup(void)
{
    g_object_unref(gvfs);
#if 0
    aud_vfs_unregister_transport(&file_const);
#endif
}

DECLARE_PLUGIN(gio, init, cleanup, NULL, NULL, NULL, NULL, NULL, NULL);
