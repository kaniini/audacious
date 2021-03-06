/*
 * plugin-load.cc
 * Copyright 2007-2013 William Pitcock and John Lindgren
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

#include "plugins-internal.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gmodule.h>

#include "audstrings.h"
#include "internal.h"
#include "plugin.h"
#include "runtime.h"

static const char * plugin_dir_list[] = {
    "Transport",
    "Container",
    "Input",
    "Output",
    "Effect",
    "General",
    "Visualization"
};

struct LoadedModule {
    Plugin * header;
    GModule * module;
};

static Index<LoadedModule> loaded_modules;

Plugin * plugin_load (const char * filename)
{
    AUDDBG ("Loading plugin: %s.\n", filename);

    GModule * module = g_module_open (filename,
     (GModuleFlags) (G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL));

    if (! module)
    {
        fprintf (stderr, " *** ERROR: %s could not be loaded: %s\n", filename,
         g_module_error ());
        return nullptr;
    }

    Plugin * header;
    if (! g_module_symbol (module, "_aud_plugin_self", (void * *) & header))
        header = nullptr;

    if (! header || header->magic != _AUD_PLUGIN_MAGIC)
    {
        fprintf (stderr, " *** ERROR: %s is not a valid Audacious plugin.\n", filename);
        g_module_close (module);
        return nullptr;
    }

    if (header->version < _AUD_PLUGIN_VERSION_MIN ||
        header->version > _AUD_PLUGIN_VERSION)
    {
        fprintf (stderr, " *** ERROR: %s is not compatible with this version "
         "of Audacious.\n", filename);
        g_module_close (module);
        return nullptr;
    }

    if (header->type == PLUGIN_TYPE_TRANSPORT ||
        header->type == PLUGIN_TYPE_PLAYLIST ||
        header->type == PLUGIN_TYPE_INPUT ||
        header->type == PLUGIN_TYPE_EFFECT)
    {
        if (PLUGIN_HAS_FUNC (header, init) && ! header->init ())
        {
            fprintf (stderr, " *** ERROR: %s failed to initialize.\n", filename);
            g_module_close (module);
            return nullptr;
        }
    }

    loaded_modules.append ({header, module});

    return header;
}

static void plugin_unload (LoadedModule & loaded)
{
    switch (loaded.header->type)
    {
    case PLUGIN_TYPE_TRANSPORT:
    case PLUGIN_TYPE_PLAYLIST:
    case PLUGIN_TYPE_INPUT:
    case PLUGIN_TYPE_EFFECT:
        if (PLUGIN_HAS_FUNC (loaded.header, cleanup))
            loaded.header->cleanup ();
        break;
    }

#ifndef VALGRIND_FRIENDLY
    g_module_close (loaded.module);
#endif
}

/******************************************************************/

static bool scan_plugin_func (const char * path, const char * basename, void * data)
{
    if (! str_has_suffix_nocase (basename, PLUGIN_SUFFIX))
        return false;

    GStatBuf st;
    if (g_stat (path, & st) < 0)
    {
        fprintf (stderr, "Unable to stat %s: %s\n", path, strerror (errno));
        return false;
    }

    if (S_ISREG (st.st_mode))
        plugin_register (path, st.st_mtime);

    return false;
}

static void scan_plugins (const char * path)
{
    dir_foreach (path, scan_plugin_func, nullptr);
}

void plugin_system_init (void)
{
    assert (g_module_supported ());

    plugin_registry_load ();

    const char * path = aud_get_path (AudPath::PluginDir);
    for (const char * dir : plugin_dir_list)
        scan_plugins (filename_build ({path, dir}));

    plugin_registry_prune ();
}

void plugin_system_cleanup (void)
{
    plugin_registry_save ();

    for (LoadedModule & loaded : loaded_modules)
        plugin_unload (loaded);

    loaded_modules.clear ();
}
