/*
 * probe.c
 * Copyright 2009-2013 John Lindgren
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

#include "probe.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "audstrings.h"
#include "internal.h"
#include "playlist.h"
#include "plugin.h"
#include "plugins-internal.h"
#include "runtime.h"

struct ProbeState {
    const char * filename;
    VFSFile * handle;
    bool failed;
    PluginHandle * plugin;
};

static bool check_opened (ProbeState * state)
{
    if (state->handle != nullptr)
        return true;
    if (state->failed)
        return false;

    AUDDBG ("Opening %s.\n", state->filename);
    state->handle = probe_buffer_new (state->filename);

    if (state->handle != nullptr)
        return true;

    AUDDBG ("FAILED.\n");
    state->failed = true;
    return false;
}

static bool probe_func (PluginHandle * plugin, ProbeState * state)
{
    AUDDBG ("Trying %s.\n", aud_plugin_get_name (plugin));
    InputPlugin * decoder = (InputPlugin *) aud_plugin_get_header (plugin);
    if (decoder == nullptr)
        return true;

    if (decoder->is_our_file_from_vfs != nullptr)
    {
        if (! check_opened (state))
            return false;

        if (decoder->is_our_file_from_vfs (state->filename, state->handle))
        {
            state->plugin = plugin;
            return false;
        }

        if (vfs_fseek (state->handle, 0, SEEK_SET) < 0)
            return false;
    }

    return true;
}

/* Optimization: If we have found plugins with a key match, assume that at least
 * one of them will succeed.  This means that we need not check the very last
 * plugin.  (If there is only one, we do not need to check it at all.)  This is
 * implemented as follows:
 *
 * 1. On the first call, assume until further notice the plugin passed is the
 *    last one and will therefore succeed.
 * 2. On a subsequent call, think twice and probe the plugin we assumed would
 *    succeed.  If it does in fact succeed, then we are done.  If not, assume
 *    similarly that the plugin passed in this call is the last one.
 */

static bool probe_func_fast (PluginHandle * plugin, ProbeState * state)
{
    if (state->plugin != nullptr)
    {
        PluginHandle * prev = state->plugin;
        state->plugin = nullptr;

        if (! probe_func (prev, state))
            return false;
    }

    AUDDBG ("Guessing %s.\n", aud_plugin_get_name (plugin));
    state->plugin = plugin;
    return true;
}

static void probe_by_scheme (ProbeState * state)
{
    const char * s = strstr (state->filename, "://");
    if (s == nullptr)
        return;

    AUDDBG ("Probing by scheme.\n");
    StringBuf buf = str_copy (state->filename, s - state->filename);
    input_plugin_for_key (INPUT_KEY_SCHEME, buf, (PluginForEachFunc) probe_func_fast, state);
}

static void probe_by_extension (ProbeState * state)
{
    StringBuf buf = uri_get_extension (state->filename);
    if (! buf)
        return;

    AUDDBG ("Probing by extension.\n");
    input_plugin_for_key (INPUT_KEY_EXTENSION, buf, (PluginForEachFunc) probe_func_fast, state);
}

static void probe_by_mime (ProbeState * state)
{
    if (! check_opened (state))
        return;

    String mime = vfs_get_metadata (state->handle, "content-type");
    if (! mime)
        return;

    AUDDBG ("Probing by MIME type.\n");
    input_plugin_for_key (INPUT_KEY_MIME, mime, (PluginForEachFunc)
     probe_func_fast, state);
}

static void probe_by_content (ProbeState * state)
{
    AUDDBG ("Probing by content.\n");
    aud_plugin_for_enabled (PLUGIN_TYPE_INPUT, (PluginForEachFunc) probe_func, state);
}

EXPORT PluginHandle * aud_file_find_decoder (const char * filename, bool fast)
{
    ProbeState state;

    AUDDBG ("Probing %s.\n", filename);
    state.plugin = nullptr;
    state.filename = filename;
    state.handle = nullptr;
    state.failed = false;

    probe_by_scheme (& state);

    if (state.plugin != nullptr)
        goto DONE;

    probe_by_extension (& state);

    if (state.plugin != nullptr || fast)
        goto DONE;

    probe_by_mime (& state);

    if (state.plugin != nullptr)
        goto DONE;

    probe_by_content (& state);

DONE:
    if (state.handle != nullptr)
        vfs_fclose (state.handle);

    if (state.plugin != nullptr)
        AUDDBG ("Probe succeeded: %s\n", aud_plugin_get_name (state.plugin));
    else
        AUDDBG ("Probe failed.\n");

    return state.plugin;
}

static bool open_file (const char * filename, InputPlugin * ip,
 const char * mode, VFSFile * * handle)
{
    /* no need to open a handle for custom URI schemes */
    if (ip->schemes && ip->schemes[0])
        return true;

    * handle = vfs_fopen (filename, mode);
    return (* handle != nullptr);
}

EXPORT Tuple aud_file_read_tuple (const char * filename, PluginHandle * decoder)
{
    InputPlugin * ip = (InputPlugin *) aud_plugin_get_header (decoder);
    g_return_val_if_fail (ip, Tuple ());
    g_return_val_if_fail (ip->probe_for_tuple, Tuple ());

    VFSFile * handle = nullptr;
    if (! open_file (filename, ip, "r", & handle))
        return Tuple ();

    Tuple tuple = ip->probe_for_tuple (filename, handle);

    if (handle)
        vfs_fclose (handle);

    return tuple;
}

EXPORT bool aud_file_read_image (const char * filename,
 PluginHandle * decoder, void * * data, int64_t * size)
{
    * data = nullptr;
    * size = 0;

    if (! input_plugin_has_images (decoder))
        return false;

    InputPlugin * ip = (InputPlugin *) aud_plugin_get_header (decoder);
    g_return_val_if_fail (ip, false);
    g_return_val_if_fail (ip->get_song_image, false);

    VFSFile * handle = nullptr;
    if (! open_file (filename, ip, "r", & handle))
        return false;

    bool success = ip->get_song_image (filename, handle, data, size);

    if (handle)
        vfs_fclose (handle);

    return success;
}

EXPORT bool aud_file_can_write_tuple (const char * filename, PluginHandle * decoder)
{
    return input_plugin_can_write_tuple (decoder);
}

EXPORT bool aud_file_write_tuple (const char * filename,
 PluginHandle * decoder, const Tuple & tuple)
{
    InputPlugin * ip = (InputPlugin *) aud_plugin_get_header (decoder);
    g_return_val_if_fail (ip, false);
    g_return_val_if_fail (ip->update_song_tuple, false);

    VFSFile * handle = nullptr;
    if (! open_file (filename, ip, "r+", & handle))
        return false;

    bool success = ip->update_song_tuple (filename, handle, tuple);

    if (handle)
        vfs_fclose (handle);

    if (success)
        aud_playlist_rescan_file (filename);

    return success;
}

EXPORT bool aud_custom_infowin (const char * filename, PluginHandle * decoder)
{
    if (! input_plugin_has_infowin (decoder))
        return false;

    InputPlugin * ip = (InputPlugin *) aud_plugin_get_header (decoder);
    g_return_val_if_fail (ip, false);
    g_return_val_if_fail (ip->file_info_box, false);

    ip->file_info_box (filename);
    return true;
}
