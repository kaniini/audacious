/*
 * plugin.h
 * Copyright 2005-2013 William Pitcock, Yoshiki Yazawa, Eugene Zagidullin, and
 *                     John Lindgren
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

#ifndef LIBAUDCORE_PLUGIN_H
#define LIBAUDCORE_PLUGIN_H

#include <libaudcore/audio.h>
#include <libaudcore/index.h>
#include <libaudcore/plugins.h>
#include <libaudcore/tuple.h>
#include <libaudcore/vfs.h>

struct PluginPreferences;

/* "Magic" bytes identifying an Audacious plugin header. */
#define _AUD_PLUGIN_MAGIC ((int) 0x8EAC8DE2)

/* API version.  Plugins are marked with this number at compile time.
 *
 * _AUD_PLUGIN_VERSION is the current version; _AUD_PLUGIN_VERSION_MIN is
 * the oldest one we are backward compatible with.  Plugins marked older than
 * _AUD_PLUGIN_VERSION_MIN or newer than _AUD_PLUGIN_VERSION are not loaded.
 *
 * Before releases that add new pointers to the end of the API tables, increment
 * _AUD_PLUGIN_VERSION but leave _AUD_PLUGIN_VERSION_MIN the same.
 *
 * Before releases that break backward compatibility (e.g. remove pointers from
 * the API tables), increment _AUD_PLUGIN_VERSION *and* set
 * _AUD_PLUGIN_VERSION_MIN to the same value. */

#define _AUD_PLUGIN_VERSION_MIN 46 /* 3.6-devel */
#define _AUD_PLUGIN_VERSION     46 /* 3.6-devel */

/* A NOTE ON THREADS
 *
 * How thread-safe a plugin must be depends on the type of plugin.  Note that
 * some parts of the Audacious API are *not* thread-safe and therefore cannot be
 * used in some parts of some plugins; for example, input plugins cannot use
 * GUI-related calls or access the playlist except in about() and configure().
 *
 * Thread-safe plugins: transport, playlist, input, effect, and output.  These
 * must be mostly thread-safe.  init() and cleanup() may be called from
 * secondary threads; however, no other functions provided by the plugin will be
 * called at the same time.  about() and configure() will be called only from
 * the main thread.  All other functions provided by the plugin may be called
 * from any thread and from multiple threads simultaneously.
 *
 * Exceptions:
 * - Because many existing input plugins are not coded to handle simultaneous
 *   calls to play(), play() will only be called from one thread at a time.  New
 *   plugins should not rely on this exception, though.
 * - Some combinations of calls, especially for output and effect plugins, make
 *   no sense; for example, flush() in an output plugin will only be called
 *   after open_audio() and before close_audio().
 *
 * Single-thread plugins: visualization, general, and interface.  Functions
 * provided by these plugins will only be called from the main thread. */

/* CROSS-PLUGIN MESSAGES
 *
 * Since 3.2, Audacious implements a basic messaging system between plugins.
 * Messages are sent using aud_plugin_send_message() and received through the
 * take_message() method specified in the header of the receiving plugin.
 * Plugins that do not need to receive messages can set take_message() to nullptr.
 *
 * Each message includes a code indicating the type of message, a pointer to
 * some data, and a value indicating the size of that data. What the message
 * data contains is entirely up to the two plugins involved. For this reason, it
 * is crucial that both plugins agree on the meaning of the message codes used.
 *
 * Once the message is sent, an integer error code is returned. If the receiving
 * plugin does not provide the take_message() method, ENOSYS is returned. If
 * take_message() does not recognize the message code, it should ignore the
 * message and return EINVAL. An error code of zero represents success. Other
 * error codes may be used with more specific meanings.
 *
 * For the time being, aud_plugin_send_message() should only be called from the
 * program's main thread. */

#define PLUGIN_COMMON_FIELDS \
    int magic; /* checked against _AUD_PLUGIN_MAGIC */ \
    int version; /* checked against _AUD_PLUGIN_VERSION */ \
    int type; /* PLUGIN_TYPE_XXX */ \
    int size; /* size in bytes of the struct */ \
    const char * name; \
    const char * domain; /* for gettext */ \
    const char * about_text; \
    const PluginPreferences * prefs; \
    bool (* init) (void); \
    void (* cleanup) (void); \
    int (* take_message) (const char * code, const void * data, int size); \
    void (* about) (void); /* use about_text instead if possible */ \
    void (* configure) (void); /* use prefs instead if possible */ \
    void * reserved1; \
    void * reserved2; \
    void * reserved3; \
    void * reserved4;

struct Plugin
{
    PLUGIN_COMMON_FIELDS
};

struct TransportPlugin
{
    PLUGIN_COMMON_FIELDS

    /* supported URI schemes (without "://")
     *     (array terminated with null pointer) */
    const char * const * schemes;

    /* file operation implementations
     *     (struct of function pointers, may contain null pointers) */
    const VFSConstructor * vtable;
};

struct PlaylistPlugin
{
    PLUGIN_COMMON_FIELDS

    /* supported file extensions (without periods)
     *     (array terminated with null pointer) */
    const char * const * extensions;

    /* path: URI of playlist file (in)
     * file: VFS handle of playlist file (in, read-only file, not seekable)
     * title: title of playlist (out)
     * items: playlist entries (out) */
    bool (* load) (const char * path, VFSFile * file, String & title,
     Index<PlaylistAddItem> & items);

    /* path: URI of playlist file (in)
     * file: VFS handle of playlist file (in, write-only file, not seekable)
     * title: title of playlist (in)
     * items: playlist entries (in) */
    bool (* save) (const char * path, VFSFile * file, const char * title,
     const Index<PlaylistAddItem> & items);
};

struct OutputPlugin
{
    PLUGIN_COMMON_FIELDS

    /* During probing, plugins with higher priority (10 to 0) are tried first. */
    int probe_priority;

    /* Returns current volume for left and right channels (0 to 100). */
    void (* get_volume) (int * l, int * r);

    /* Changes volume for left and right channels (0 to 100). */
    void (* set_volume) (int l, int r);

    /* Begins playback of a PCM stream.  <format> is one of the FMT_*
     * enumeration values defined in libaudcore/audio.h.  Returns nonzero on
     * success. */
    bool (* open_audio) (int format, int rate, int chans);

    /* Ends playback.  Any buffered audio data is discarded. */
    void (* close_audio) (void);

    /* Returns how many bytes of data may be passed to a following write_audio()
     * call.  nullptr if the plugin supports only blocking writes (not recommended). */
    int (* buffer_free) (void);

    /* Waits until buffer_free() will return a size greater than zero.
     * output_time(), pause(), and flush() may be called meanwhile; if flush()
     * is called, period_wait() should return immediately.  nullptr if the plugin
     * supports only blocking writes (not recommended). */
    void (* period_wait) (void);

    /* Buffers <size> bytes of data, in the format given to open_audio(). */
    void (* write_audio) (void * data, int size);

    /* Waits until all buffered data has been heard by the user. */
    void (* drain) (void);

    /* Returns time count (in milliseconds) of how much data has been heard by
     * the user. */
    int (* output_time) (void);

    /* Pauses the stream if <p> is nonzero; otherwise unpauses it.
     * write_audio() will not be called while the stream is paused. */
    void (* pause) (bool p);

    /* Discards any buffered audio data and sets the time counter (in
     * milliseconds) of data written. */
    void (* flush) (int time);

    /* Whether close_audio() and open_audio() must always be called between
     * songs, even if the audio format is the same.  Note that this defeats
     * gapless playback. */
    bool force_reopen;
};

struct EffectPlugin
{
    PLUGIN_COMMON_FIELDS

    /* All processing is done in floating point.  If the effect plugin wants to
     * change the channel count or sample rate, it can change the parameters
     * passed to start().  They cannot be changed in the middle of a song. */
    void (* start) (int * channels, int * rate);

    /* process() has two options: modify the samples in place and leave the data
     * pointer unchanged or copy them into a buffer of its own.  If it sets the
     * pointer to dynamically allocated memory, it is the plugin's job to free
     * that memory.  process() may return different lengths of audio than it is
     * passed, even a zero length. */
    void (* process) (float * * data, int * samples);

    /* Optional.  A seek is taking place; any buffers should be discarded. */
    void (* flush) (void);

    /* Exactly like process() except that any buffers should be drained (i.e.
     * the data processed and returned).  finish() will be called a second time
     * at the end of the last song in the playlist. */
    void (* finish) (float * * data, int * samples);

    /* Required only for plugins that change the time domain (e.g. a time
     * stretch) or use read-ahead buffering.  translate_delay() must do two
     * things: first, translate <delay> (which is in milliseconds) from the
     * output time domain back to the input time domain; second, increase
     * <delay> by the size of the read-ahead buffer.  It should return the
     * adjusted delay. */
    int (* adjust_delay) (int delay);

    /* Effects with lowest order (0 to 9) are applied first. */
    int order;

    /* If the effect does not change the number of channels or the sampling
     * rate, it can be enabled and disabled more smoothly. */
    bool preserves_format;
};

struct InputPlugin
{
    PLUGIN_COMMON_FIELDS

    /* Nonzero if the files handled by the plugin may contain more than one
     * song.  When reading the tuple for such a file, the plugin should set the
     * FIELD_SUBSONG_NUM field to the number of songs in the file.  For all
     * other files, the field should be left unset.
     *
     * Example:
     * 1. User adds a file named "somefile.xxx" to the playlist.  Having
     * determined that this plugin can handle the file, Audacious opens the file
     * and calls probe_for_tuple().  probe_for_tuple() sees that there are 3
     * songs in the file and sets FIELD_SUBSONG_NUM to 3.
     * 2. For each song in the file, Audacious opens the file and calls
     * probe_for_tuple() -- this time, however, a question mark and song number
     * are appended to the file name passed: "somefile.sid?2" refers to the
     * second song in the file "somefile.sid".
     * 3. When one of the songs is played, Audacious opens the file and calls
     * play() with a file name modified in this way.
     */
    bool have_subtune;

    /* Pointer to an array (terminated with nullptr) of file extensions associated
     * with file types the plugin can handle. */
    const char * const * extensions;
    /* Pointer to an array (terminated with nullptr) of MIME types the plugin can
     * handle. */
    const char * const * mimes;

    /* Pointer to an array (terminated with nullptr) of custom URI schemes the
     * plugin supports.  Plugins using custom URI schemes are expected to
     * handle their own I/O.  Hence, any VFSFile pointers passed to play(),
     * probe_for_tuple(), etc. will be nullptr. */
    const char * const * schemes;

    /* How quickly the plugin should be tried in searching for a plugin to
     * handle a file which could not be identified from its extension.  Plugins
     * with priority 0 are tried first, 10 last. */
    int priority;

    /* Returns true if the plugin can handle the file. */
    bool (* is_our_file_from_vfs) (const char * filename, VFSFile * file);

    /* Reads metadata from the file. */
    Tuple (* probe_for_tuple) (const char * filename, VFSFile * file);

    /* Plays the file.  Returns false on error.  Also see input-api.h. */
    bool (* play) (const char * filename, VFSFile * file);

    /* Optional.  Writes metadata to the file, returning false on error. */
    bool (* update_song_tuple) (const char * filename, VFSFile * file, const Tuple & tuple);

    /* Optional.  Reads an album art image (JPEG or PNG data) from the file.
     * Returns a pointer to the data along with its size in bytes.  The returned
     * data will be freed when no longer needed.  Returns false on error. */
    bool (* get_song_image) (const char * filename, VFSFile * file,
     void * * data, int64_t * size);

    /* Optional.  Displays a window showing info about the file.  In general,
     * this function should be avoided since Audacious already provides a file
     * info window. */
    void (* file_info_box) (const char * filename);
};

struct GeneralPlugin
{
    PLUGIN_COMMON_FIELDS

    bool enabled_by_default;

    /* GtkWidget * (* get_widget) (void); */
    void * (* get_widget) (void);
};

struct VisPlugin
{
    PLUGIN_COMMON_FIELDS

    /* reset internal state and clear display */
    void (* clear) (void);

    /* 512 frames of a single-channel PCM signal */
    void (* render_mono_pcm) (const float * pcm);

    /* 512 frames of an interleaved multi-channel PCM signal */
    void (* render_multi_pcm) (const float * pcm, int channels);

    /* intensity of frequencies 1/512, 2/512, ..., 256/512 of sample rate */
    void (* render_freq) (const float * freq);

    /* GtkWidget * (* get_widget) (void); */
    void * (* get_widget) (void);
};

struct IfacePlugin
{
    PLUGIN_COMMON_FIELDS

    void (* show) (bool show);
    void (* run) (void);
    void (* quit) (void);

    void (* show_about_window) (void);
    void (* hide_about_window) (void);
    void (* show_filebrowser) (bool open);
    void (* hide_filebrowser) (void);
    void (* show_jump_to_song) (void);
    void (* hide_jump_to_song) (void);
    void (* show_prefs_window) (void);
    void (* hide_prefs_window) (void);
    void (* plugin_menu_add) (int id, void (* func) (void), const char * name, const char * icon);
    void (* plugin_menu_remove) (int id, void (* func) (void));
};

#undef PLUGIN_COMMON_FIELDS

#define AUD_PLUGIN(stype, itype, ...) \
extern "C" const stype _aud_plugin_self = { \
    _AUD_PLUGIN_MAGIC, \
    _AUD_PLUGIN_VERSION, \
    itype, \
    sizeof (stype), \
    __VA_ARGS__ \
};

#define AUD_TRANSPORT_PLUGIN(...) AUD_PLUGIN (TransportPlugin, PLUGIN_TYPE_TRANSPORT, __VA_ARGS__)
#define AUD_PLAYLIST_PLUGIN(...) AUD_PLUGIN (PlaylistPlugin, PLUGIN_TYPE_PLAYLIST, __VA_ARGS__)
#define AUD_INPUT_PLUGIN(...) AUD_PLUGIN (InputPlugin, PLUGIN_TYPE_INPUT, __VA_ARGS__)
#define AUD_EFFECT_PLUGIN(...) AUD_PLUGIN (EffectPlugin, PLUGIN_TYPE_EFFECT, __VA_ARGS__)
#define AUD_OUTPUT_PLUGIN(...) AUD_PLUGIN (OutputPlugin, PLUGIN_TYPE_OUTPUT, __VA_ARGS__)
#define AUD_VIS_PLUGIN(...) AUD_PLUGIN (VisPlugin, PLUGIN_TYPE_VIS, __VA_ARGS__)
#define AUD_GENERAL_PLUGIN(...) AUD_PLUGIN (GeneralPlugin, PLUGIN_TYPE_GENERAL, __VA_ARGS__)
#define AUD_IFACE_PLUGIN(...) AUD_PLUGIN (IfacePlugin, PLUGIN_TYPE_IFACE, __VA_ARGS__)

#define PLUGIN_HAS_FUNC(p, func) \
 ((p)->size > (char *) & (p)->func - (char *) (p) && (p)->func)

#endif
