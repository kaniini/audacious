/*
 * vfs_local.c
 * Copyright 2009-2014 John Lindgren
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the documentation
 *    provided with the distribution.
 *
 * This software is provided "as is" and without any warranty, express or
 * implied. In no event shall the authors be liable for any damages arising from
 * the use of this software.
 */

#include "vfs_local.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "audstrings.h"

#ifdef _WIN32
#define fseeko fseeko64
#define ftello ftello64
#endif

enum LocalOp {
    OP_NONE,
    OP_READ,
    OP_WRITE
};

struct LocalFile {
    String path;
    FILE * stream;
    int64_t cached_size;
    LocalOp last_op;
};

static void * local_fopen (const char * uri, const char * mode)
{
    StringBuf path = uri_to_filename (uri);
    g_return_val_if_fail (path, nullptr);

    const char * suffix = "";

#ifdef _WIN32
    if (! strchr (mode, 'b'))  /* binary mode (Windows) */
        suffix = "b";
#else
    if (! strchr (mode, 'e'))  /* close on exec (POSIX) */
        suffix = "e";
#endif

    StringBuf mode2 = str_concat ({mode, suffix});

    FILE * stream = g_fopen (path, mode2);

    if (! stream)
    {
        perror (path);
        return nullptr;
    }

    LocalFile * local = new LocalFile ();

    local->path = String (path);
    local->stream = stream;
    local->cached_size = -1;
    local->last_op = OP_NONE;

    return local;
}

static int local_fclose (VFSFile * file)
{
    LocalFile * local = (LocalFile *) vfs_get_handle (file);

    int result = fclose (local->stream);
    if (result < 0)
        perror (local->path);

    delete local;

    return result;
}

static int64_t local_fread (void * ptr, int64_t size, int64_t nitems, VFSFile * file)
{
    LocalFile * local = (LocalFile *) vfs_get_handle (file);

    if (local->last_op == OP_WRITE)
    {
        if (fseeko (local->stream, 0, SEEK_CUR) < 0)  /* flush buffers */
        {
            perror (local->path);
            return 0;
        }
    }

    local->last_op = OP_READ;

    clearerr (local->stream);

    int64_t result = fread (ptr, size, nitems, local->stream);
    if (result < nitems && ferror (local->stream))
        perror (local->path);

    return result;
}

static int64_t local_fwrite (const void * ptr, int64_t size, int64_t nitems, VFSFile * file)
{
    LocalFile * local = (LocalFile *) vfs_get_handle (file);

    if (local->last_op == OP_READ)
    {
        if (fseeko (local->stream, 0, SEEK_CUR) < 0)  /* flush buffers */
        {
            perror (local->path);
            return 0;
        }
    }

    local->last_op = OP_WRITE;
    local->cached_size = -1;

    clearerr (local->stream);

    int64_t result = fwrite (ptr, size, nitems, local->stream);
    if (result < nitems && ferror (local->stream))
        perror (local->path);

    return result;
}

static int local_fseek (VFSFile * file, int64_t offset, int whence)
{
    LocalFile * local = (LocalFile *) vfs_get_handle (file);

    int result = fseeko (local->stream, offset, whence);
    if (result < 0)
        perror (local->path);

    if (result == 0)
        local->last_op = OP_NONE;

    return result;
}

static int64_t local_ftell (VFSFile * file)
{
    LocalFile * local = (LocalFile *) vfs_get_handle (file);
    return ftello (local->stream);
}

static bool local_feof (VFSFile * file)
{
    LocalFile * local = (LocalFile *) vfs_get_handle (file);
    return feof (local->stream);
}

static int local_ftruncate (VFSFile * file, int64_t length)
{
    LocalFile * local = (LocalFile *) vfs_get_handle (file);

    if (local->last_op != OP_NONE)
    {
        if (fseeko (local->stream, 0, SEEK_CUR) < 0)  /* flush buffers */
        {
            perror (local->path);
            return 0;
        }
    }

    int result = ftruncate (fileno (local->stream), length);
    if (result < 0)
        perror (local->path);

    if (result == 0)
    {
        local->last_op = OP_NONE;
        local->cached_size = length;
    }

    return result;
}

static int64_t local_fsize (VFSFile * file)
{
    LocalFile * local = (LocalFile *) vfs_get_handle (file);

    if (local->cached_size < 0)
    {
        int64_t saved_pos = ftello (local->stream);
        if (ftello < 0)
            goto ERR;

        if (local_fseek (file, 0, SEEK_END) < 0)
            goto ERR;

        int64_t length = ftello (local->stream);
        if (length < 0)
            goto ERR;

        if (local_fseek (file, saved_pos, SEEK_SET) < 0)
            goto ERR;

        local->cached_size = length;
    }

    return local->cached_size;

ERR:
    perror (local->path);
    return -1;
}

const VFSConstructor vfs_local_vtable = {
    local_fopen,
    local_fclose,
    local_fread,
    local_fwrite,
    local_fseek,
    local_ftell,
    local_feof,
    local_ftruncate,
    local_fsize
};
