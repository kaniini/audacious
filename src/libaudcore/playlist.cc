/*
 * playlist.cc
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

#include "playlist-internal.h"
#include "runtime.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "audstrings.h"
#include "drct.h"
#include "hook.h"
#include "i18n.h"
#include "interface.h"
#include "internal.h"
#include "list.h"
#include "mainloop.h"
#include "multihash.h"
#include "objects.h"
#include "plugins.h"
#include "scanner.h"
#include "tuple.h"
#include "tuple-compiler.h"

enum {RESUME_STOP, RESUME_PLAY, RESUME_PAUSE};

#define STATE_FILE "playlist-state"

#define ENTER pthread_mutex_lock (& mutex)
#define LEAVE pthread_mutex_unlock (& mutex)

#define RETURN(...) do { \
    pthread_mutex_unlock (& mutex); \
    return __VA_ARGS__; \
} while (0)

#define ENTER_GET_PLAYLIST(...) ENTER; \
    Playlist * playlist = lookup_playlist (playlist_num); \
    if (! playlist) \
        RETURN (__VA_ARGS__);

#define ENTER_GET_ENTRY(...) ENTER_GET_PLAYLIST (__VA_ARGS__); \
    Entry * entry = lookup_entry (playlist, entry_num); \
    if (! entry) \
        RETURN (__VA_ARGS__);

struct UniqueID
{
    constexpr UniqueID (int val) :
        val (val) {}

    operator int () const
        { return val; }

    unsigned hash () const
        { return int32_hash (val); }

private:
    int val;
};

struct Update {
    int level, before, after;
};

struct Entry {
    Entry (PlaylistAddItem && item);
    ~Entry ();

    int number;
    String filename;
    PluginHandle * decoder;
    Tuple tuple;
    String formatted, title, artist, album;
    int length;
    bool failed;
    bool selected;
    int shuffle_num;
    bool queued;
};

struct Playlist {
    Playlist (int id);
    ~Playlist ();

    int number, unique_id;
    String filename, title;
    bool modified;
    Index<SmartPtr<Entry>> entries;
    Entry * position, * focus;
    int selected_count;
    int last_shuffle_num;
    Index<Entry *> queued;
    int64_t total_length, selected_length;
    bool scanning, scan_ending;
    Update next_update, last_update;
    bool resume_paused;
    int resume_time;
};

static const char * const default_title = N_("New Playlist");
static const char * const temp_title = N_("Now Playing");

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

/* The unique ID table contains pointers to Playlist for ID's in use and nullptr
 * for "dead" (previously used and therefore unavailable) ID's. */
static SimpleHash<UniqueID, Playlist *> unique_id_table;
static int next_unique_id = 1000;

static Index<SmartPtr<Playlist>> playlists;
static Playlist * active_playlist = nullptr;
static Playlist * playing_playlist = nullptr;
static int resume_playlist = -1;

static QueuedFunc queued_update;
static int update_level;

struct ScanItem : public ListNode
{
    Playlist * playlist;
    Entry * entry;
    ScanRequest * request;
};

static int scan_playlist, scan_row;
static List<ScanItem> scan_list;

static void scan_finish (ScanRequest * request);
static void scan_cancel (Entry * entry);
static void scan_restart (void);

static bool next_song_locked (Playlist * playlist, bool repeat, int hint);

static void playlist_reformat_titles (void);
static void playlist_trigger_scan (void);

static SmartPtr<TupleCompiler> title_formatter;

static void entry_set_tuple_real (Entry * entry, Tuple && tuple)
{
    /* Hack: We cannot refresh segmented entries (since their info is read from
     * the cue sheet when it is first loaded), so leave them alone. -jlindgren */
    if (entry->tuple.get_value_type (FIELD_SEGMENT_START) == TUPLE_INT)
        return;

    entry->tuple = std::move (tuple);
    entry->failed = false;

    describe_song (entry->filename, entry->tuple, entry->title, entry->artist, entry->album);

    if (! entry->tuple)
    {
        entry->formatted = String ();
        entry->length = 0;
    }
    else
    {
        entry->formatted = title_formatter->evaluate (entry->tuple);
        entry->length = entry->tuple.get_int (FIELD_LENGTH);
        if (entry->length < 0)
            entry->length = 0;
    }
}

static void entry_set_tuple (Playlist * playlist, Entry * entry, Tuple && tuple)
{
    scan_cancel (entry);

    playlist->total_length -= entry->length;
    if (entry->selected)
        playlist->selected_length -= entry->length;

    entry_set_tuple_real (entry, std::move (tuple));

    playlist->total_length += entry->length;
    if (entry->selected)
        playlist->selected_length += entry->length;
}

static void entry_set_failed (Playlist * playlist, Entry * entry)
{
    Tuple tuple;
    tuple.set_filename (entry->filename);
    entry_set_tuple (playlist, entry, std::move (tuple));
    entry->failed = true;
}

Entry::Entry (PlaylistAddItem && item) :
    number (-1),
    filename (item.filename),
    decoder (item.decoder),
    length (0),
    failed (false),
    selected (false),
    shuffle_num (0),
    queued (false)
{
    entry_set_tuple_real (this, std::move (item.tuple));
}

Entry::~Entry ()
{
    scan_cancel (this);
}

static int new_unique_id (int preferred)
{
    if (preferred >= 0 && ! unique_id_table.lookup (preferred))
        return preferred;

    while (unique_id_table.lookup (next_unique_id))
        next_unique_id ++;

    return next_unique_id ++;
}

Playlist::Playlist (int id) :
    number (-1),
    unique_id (new_unique_id (id)),
    title (_(default_title)),
    modified (true),
    position (nullptr),
    focus (nullptr),
    selected_count (0),
    last_shuffle_num (0),
    total_length (0),
    selected_length (0),
    scanning (false),
    scan_ending (false),
    next_update (),
    last_update (),
    resume_paused (false),
    resume_time (0)
{
    unique_id_table.add (unique_id, (Playlist *) this);
}

Playlist::~Playlist ()
{
    unique_id_table.add (unique_id, nullptr);
}

static void number_playlists (int at, int length)
{
    for (int i = at; i < at + length; i ++)
        playlists[i]->number = i;
}

static Playlist * lookup_playlist (int i)
{
    return (i >= 0 && i < playlists.len ()) ? playlists[i].get () : nullptr;
}

static void number_entries (Playlist * p, int at, int length)
{
    for (int i = at; i < at + length; i ++)
        p->entries[i]->number = i;
}

static Entry * lookup_entry (Playlist * p, int i)
{
    return (i >= 0 && i < p->entries.len ()) ? p->entries[i].get () : nullptr;
}

static void update (void * unused)
{
    ENTER;

    for (auto & p : playlists)
    {
        p->last_update = p->next_update;
        p->next_update = Update ();
    }

    int level = update_level;
    update_level = 0;

    LEAVE;

    hook_call ("playlist update", GINT_TO_POINTER (level));
}

static void queue_update (int level, Playlist * p, int at, int count)
{
    if (p)
    {
        if (level >= PLAYLIST_UPDATE_METADATA)
        {
            p->modified = true;

            if (! aud_get_bool (nullptr, "metadata_on_play"))
            {
                p->scanning = true;
                p->scan_ending = false;
                scan_restart ();
            }
        }

        if (p->next_update.level)
        {
            p->next_update.level = aud::max (p->next_update.level, level);
            p->next_update.before = aud::min (p->next_update.before, at);
            p->next_update.after = aud::min (p->next_update.after, p->entries.len () - at - count);
        }
        else
        {
            p->next_update.level = level;
            p->next_update.before = at;
            p->next_update.after = p->entries.len () - at - count;
        }
    }

    if (! update_level)
        queued_update.queue (update, nullptr);

    update_level = aud::max (update_level, level);
}

EXPORT bool aud_playlist_update_pending (void)
{
    ENTER;
    bool pending = update_level ? true : false;
    RETURN (pending);
}

EXPORT int aud_playlist_updated_range (int playlist_num, int * at, int * count)
{
    ENTER_GET_PLAYLIST (0);

    Update * u = & playlist->last_update;

    int level = u->level;
    * at = u->before;
    * count = playlist->entries.len () - u->before - u->after;

    RETURN (level);
}

EXPORT bool aud_playlist_scan_in_progress (int playlist_num)
{
    if (playlist_num >= 0)
    {
        ENTER_GET_PLAYLIST (false);
        bool scanning = playlist->scanning || playlist->scan_ending;
        RETURN (scanning);
    }
    else
    {
        ENTER;

        bool scanning = false;
        for (auto & p : playlists)
        {
            if (p->scanning || p->scan_ending)
                scanning = true;
        }

        RETURN (scanning);
    }
}

static ScanItem * scan_list_find_playlist (Playlist * playlist)
{
    for (ScanItem * item = scan_list.head (); item; item = scan_list.next (item))
    {
        if (item->playlist == playlist)
            return item;
    }

    return nullptr;
}

static ScanItem * scan_list_find_entry (Entry * entry)
{
    for (ScanItem * item = scan_list.head (); item; item = scan_list.next (item))
    {
        if (item->entry == entry)
            return item;
    }

    return nullptr;
}

static ScanItem * scan_list_find_request (ScanRequest * request)
{
    for (ScanItem * item = scan_list.head (); item; item = scan_list.next (item))
    {
        if (item->request == request)
            return item;
    }

    return nullptr;
}

static void scan_queue_entry (Playlist * playlist, Entry * entry)
{
    int flags = 0;
    if (! entry->tuple)
        flags |= SCAN_TUPLE;

    ScanItem * item = new ScanItem;
    item->playlist = playlist;
    item->entry = entry;
    item->request = scan_request (entry->filename, flags, entry->decoder, scan_finish);
    scan_list.append (item);
}

static void scan_check_complete (Playlist * playlist)
{
    if (! playlist->scan_ending || scan_list_find_playlist (playlist))
        return;

    playlist->scan_ending = false;
    event_queue_cancel ("playlist scan complete", nullptr);
    event_queue ("playlist scan complete", nullptr);
}

static bool scan_queue_next_entry (void)
{
    while (scan_playlist < playlists.len ())
    {
        Playlist * playlist = playlists[scan_playlist].get ();

        if (playlist->scanning)
        {
            while (scan_row < playlist->entries.len ())
            {
                Entry * entry = playlist->entries[scan_row ++].get ();

                if (! entry->tuple && ! scan_list_find_entry (entry))
                {
                    scan_queue_entry (playlist, entry);
                    return true;
                }
            }

            playlist->scanning = false;
            playlist->scan_ending = true;
            scan_check_complete (playlist);
        }

        scan_playlist ++;
        scan_row = 0;
    }

    return false;
}

static void scan_schedule (void)
{
    int scheduled = 0;

    for (ScanItem * item = scan_list.head (); item; item = scan_list.next (item))
    {
        if (++ scheduled >= SCAN_THREADS)
            return;
    }

    while (scan_queue_next_entry ())
    {
        if (++ scheduled >= SCAN_THREADS)
            return;
    }
}

static void scan_finish (ScanRequest * request)
{
    ENTER;

    ScanItem * item = scan_list_find_request (request);
    if (! item)
        RETURN ();

    Playlist * playlist = item->playlist;
    Entry * entry = item->entry;

    scan_list.remove (item);
    delete item;

    if (! entry->decoder)
        entry->decoder = scan_request_get_decoder (request);

    if (! entry->tuple)
    {
        Tuple tuple = scan_request_get_tuple (request);
        if (tuple)
        {
            entry_set_tuple (playlist, entry, std::move (tuple));
            queue_update (PLAYLIST_UPDATE_METADATA, playlist, entry->number, 1);
        }
    }

    if (! entry->decoder || ! entry->tuple)
        entry_set_failed (playlist, entry);

    scan_check_complete (playlist);
    scan_schedule ();

    pthread_cond_broadcast (& cond);

    LEAVE;
}

static void scan_cancel (Entry * entry)
{
    ScanItem * item = scan_list_find_entry (entry);
    if (! item)
        return;

    scan_list.remove (item);
    delete (item);
}

static void scan_restart (void)
{
    scan_playlist = 0;
    scan_row = 0;
    scan_schedule ();
}

/* mutex may be unlocked during the call */
static Entry * get_entry (int playlist_num, int entry_num,
 bool need_decoder, bool need_tuple)
{
    while (1)
    {
        Playlist * playlist = lookup_playlist (playlist_num);
        Entry * entry = playlist ? lookup_entry (playlist, entry_num) : nullptr;

        if (! entry || entry->failed)
            return entry;

        if ((need_decoder && ! entry->decoder) || (need_tuple && ! entry->tuple))
        {
            if (! scan_list_find_entry (entry))
                scan_queue_entry (playlist, entry);

            pthread_cond_wait (& cond, & mutex);
            continue;
        }

        return entry;
    }
}

/* mutex may be unlocked during the call */
static Entry * get_playback_entry (bool need_decoder, bool need_tuple)
{
    while (1)
    {
        Entry * entry = playing_playlist ? playing_playlist->position : nullptr;

        if (! entry || entry->failed)
            return entry;

        if ((need_decoder && ! entry->decoder) || (need_tuple && ! entry->tuple))
        {
            if (! scan_list_find_entry (entry))
                scan_queue_entry (playing_playlist, entry);

            pthread_cond_wait (& cond, & mutex);
            continue;
        }

        return entry;
    }
}

void playlist_init (void)
{
    srand (time (nullptr));

    ENTER;

    update_level = 0;
    scan_playlist = scan_row = 0;

    title_formatter = new TupleCompiler;

    LEAVE;

    /* initialize title formatter */
    playlist_reformat_titles ();

    hook_associate ("set metadata_on_play", (HookFunction) playlist_trigger_scan, nullptr);
    hook_associate ("set generic_title_format", (HookFunction) playlist_reformat_titles, nullptr);
    hook_associate ("set show_numbers_in_pl", (HookFunction) playlist_reformat_titles, nullptr);
    hook_associate ("set leading_zero", (HookFunction) playlist_reformat_titles, nullptr);
}

void playlist_end (void)
{
    hook_dissociate ("set metadata_on_play", (HookFunction) playlist_trigger_scan);
    hook_dissociate ("set generic_title_format", (HookFunction) playlist_reformat_titles);
    hook_dissociate ("set show_numbers_in_pl", (HookFunction) playlist_reformat_titles);
    hook_dissociate ("set leading_zero", (HookFunction) playlist_reformat_titles);

    ENTER;

    queued_update.stop ();

    active_playlist = playing_playlist = nullptr;
    resume_playlist = -1;

    playlists.clear ();
    unique_id_table.clear ();

    title_formatter = nullptr;

    LEAVE;
}

EXPORT int aud_playlist_count (void)
{
    ENTER;
    int count = playlists.len ();
    RETURN (count);
}

void playlist_insert_with_id (int at, int id)
{
    ENTER;

    if (at < 0 || at > playlists.len ())
        at = playlists.len ();

    playlists.insert (at, 1);
    playlists[at] = SmartPtr<Playlist> (new Playlist (id));

    number_playlists (at, playlists.len () - at);

    queue_update (PLAYLIST_UPDATE_STRUCTURE, nullptr, 0, 0);
    LEAVE;
}

EXPORT void aud_playlist_insert (int at)
{
    playlist_insert_with_id (at, -1);
}

EXPORT void aud_playlist_reorder (int from, int to, int count)
{
    ENTER;

    if (from < 0 || from + count > playlists.len () || to < 0 || to +
     count > playlists.len () || count < 0)
        RETURN ();

    Index<SmartPtr<Playlist>> displaced;

    if (to < from)
        displaced.move_from (playlists, to, -1, from - to, true, false);
    else
        displaced.move_from (playlists, from + count, -1, to - from, true, false);

    playlists.shift (from, to, count);

    if (to < from)
    {
        playlists.move_from (displaced, 0, to + count, from - to, false, true);
        number_playlists (to, from + count - to);
    }
    else
    {
        playlists.move_from (displaced, 0, from, to - from, false, true);
        number_playlists (from, to + count - from);
    }

    queue_update (PLAYLIST_UPDATE_STRUCTURE, nullptr, 0, 0);
    LEAVE;
}

EXPORT void aud_playlist_delete (int playlist_num)
{
    ENTER_GET_PLAYLIST ();

    bool was_playing = (playlist == playing_playlist);

    playlists.remove (playlist_num, 1);

    if (! playlists.len ())
        playlists.append (SmartPtr<Playlist> (new Playlist (-1)));

    number_playlists (playlist_num, playlists.len () - playlist_num);

    if (playlist == active_playlist)
    {
        int active_num = aud::min (playlist_num, playlists.len () - 1);
        active_playlist = playlists[active_num].get ();
    }

    if (playlist == playing_playlist)
        playing_playlist = nullptr;

    queue_update (PLAYLIST_UPDATE_STRUCTURE, nullptr, 0, 0);
    LEAVE;

    if (was_playing)
        playback_stop ();
}

EXPORT int aud_playlist_get_unique_id (int playlist_num)
{
    ENTER_GET_PLAYLIST (-1);
    int unique_id = playlist->unique_id;
    RETURN (unique_id);
}

EXPORT int aud_playlist_by_unique_id (int id)
{
    ENTER;

    Playlist * * ptr = unique_id_table.lookup (id);
    int num = (ptr && * ptr) ? (* ptr)->number : -1;

    RETURN (num);
}

EXPORT void aud_playlist_set_filename (int playlist_num, const char * filename)
{
    ENTER_GET_PLAYLIST ();

    playlist->filename = String (filename);
    playlist->modified = true;

    queue_update (PLAYLIST_UPDATE_METADATA, nullptr, 0, 0);
    LEAVE;
}

EXPORT String aud_playlist_get_filename (int playlist_num)
{
    ENTER_GET_PLAYLIST (String ());
    String filename = playlist->filename;
    RETURN (filename);
}

EXPORT void aud_playlist_set_title (int playlist_num, const char * title)
{
    ENTER_GET_PLAYLIST ();

    playlist->title = String (title);
    playlist->modified = true;

    queue_update (PLAYLIST_UPDATE_METADATA, nullptr, 0, 0);
    LEAVE;
}

EXPORT String aud_playlist_get_title (int playlist_num)
{
    ENTER_GET_PLAYLIST (String ());
    String title = playlist->title;
    RETURN (title);
}

void playlist_set_modified (int playlist_num, bool modified)
{
    ENTER_GET_PLAYLIST ();
    playlist->modified = modified;
    LEAVE;
}

bool playlist_get_modified (int playlist_num)
{
    ENTER_GET_PLAYLIST (false);
    bool modified = playlist->modified;
    RETURN (modified);
}

EXPORT void aud_playlist_set_active (int playlist_num)
{
    ENTER_GET_PLAYLIST ();

    bool changed = false;

    if (playlist != active_playlist)
    {
        changed = true;
        active_playlist = playlist;
    }

    LEAVE;

    if (changed)
        hook_call ("playlist activate", nullptr);
}

EXPORT int aud_playlist_get_active (void)
{
    ENTER;
    int list = active_playlist ? active_playlist->number : -1;
    RETURN (list);
}

EXPORT void aud_playlist_set_playing (int playlist_num)
{
    /* get playback state before locking playlists */
    bool paused = aud_drct_get_paused ();
    int time = aud_drct_get_time ();

    ENTER;

    Playlist * playlist = lookup_playlist (playlist_num);
    bool can_play = false;
    bool position_changed = false;

    if (playlist == playing_playlist)
        RETURN ();

    if (playing_playlist)
    {
        playing_playlist->resume_paused = paused;
        playing_playlist->resume_time = time;
    }

    /* is there anything to play? */
    if (playlist && ! playlist->position)
    {
        if (next_song_locked (playlist, true, 0))
            position_changed = true;
        else
            playlist = nullptr;
    }

    if (playlist)
    {
        can_play = true;
        paused = playlist->resume_paused;
        time = playlist->resume_time;
    }

    playing_playlist = playlist;

    LEAVE;

    if (position_changed)
        hook_call ("playlist position", GINT_TO_POINTER (playlist_num));

    hook_call ("playlist set playing", nullptr);

    /* start playback after unlocking playlists */
    if (can_play)
        playback_play (time, paused);
    else
        playback_stop ();
}

EXPORT int aud_playlist_get_playing (void)
{
    ENTER;
    int list = playing_playlist ? playing_playlist->number: -1;
    RETURN (list);
}

EXPORT int aud_playlist_get_blank (void)
{
    int list = aud_playlist_get_active ();
    String title = aud_playlist_get_title (list);

    if (strcmp (title, _(default_title)) || aud_playlist_entry_count (list) > 0)
    {
        list = aud_playlist_count ();
        aud_playlist_insert (list);
    }

    return list;
}

EXPORT int aud_playlist_get_temporary (void)
{
    int count = aud_playlist_count ();

    for (int list = 0; list < count; list ++)
    {
        String title = aud_playlist_get_title (list);
        if (! strcmp (title, _(temp_title)))
            return list;
    }

    int list = aud_playlist_get_blank ();
    aud_playlist_set_title (list, _(temp_title));
    return list;
}

static void set_position (Playlist * playlist, Entry * entry, bool update_shuffle)
{
    playlist->position = entry;
    playlist->resume_time = 0;

    /* move entry to top of shuffle list */
    if (entry && update_shuffle)
        entry->shuffle_num = ++ playlist->last_shuffle_num;
}

/* unlocked */
static void change_playback (bool can_play)
{
    if (can_play && aud_drct_get_playing ())
        playback_play (0, aud_drct_get_paused ());
    else
        aud_playlist_set_playing (-1);
}

EXPORT int aud_playlist_entry_count (int playlist_num)
{
    ENTER_GET_PLAYLIST (0);
    int count = playlist->entries.len ();
    RETURN (count);
}

void playlist_entry_insert_batch_raw (int playlist_num, int at, Index<PlaylistAddItem> && items)
{
    ENTER_GET_PLAYLIST ();

    int entries = playlist->entries.len ();

    if (at < 0 || at > entries)
        at = entries;

    int number = items.len ();

    playlist->entries.insert (at, number);

    int i = at;
    for (auto & item : items)
    {
        Entry * entry = new Entry (std::move (item));
        playlist->entries[i ++] = SmartPtr<Entry> (entry);
        playlist->total_length += entry->length;
    }

    items.clear ();

    number_entries (playlist, at, entries + number - at);

    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, at, number);
    LEAVE;
}

EXPORT void aud_playlist_entry_delete (int playlist_num, int at, int number)
{
    ENTER_GET_PLAYLIST ();

    int entries = playlist->entries.len ();
    bool position_changed = false;
    bool was_playing = false;
    bool can_play = false;

    if (at < 0 || at > entries)
        at = entries;
    if (number < 0 || number > entries - at)
        number = entries - at;

    if (playlist->position && playlist->position->number >= at &&
     playlist->position->number < at + number)
    {
        position_changed = true;
        was_playing = (playlist == playing_playlist);

        set_position (playlist, nullptr, false);
    }

    if (playlist->focus && playlist->focus->number >= at &&
     playlist->focus->number < at + number)
    {
        if (at + number < entries)
            playlist->focus = playlist->entries[at + number].get ();
        else if (at > 0)
            playlist->focus = playlist->entries[at - 1].get ();
        else
            playlist->focus = nullptr;
    }

    for (int count = 0; count < number; count ++)
    {
        Entry * entry = playlist->entries [at + count].get ();

        if (entry->queued)
            playlist->queued.remove (playlist->queued.find (entry), 1);

        if (entry->selected)
        {
            playlist->selected_count --;
            playlist->selected_length -= entry->length;
        }

        playlist->total_length -= entry->length;
    }

    playlist->entries.remove (at, number);
    number_entries (playlist, at, entries - at - number);

    if (position_changed && aud_get_bool (nullptr, "advance_on_delete"))
        can_play = next_song_locked (playlist, aud_get_bool (nullptr, "repeat"), at);

    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, at, 0);
    LEAVE;

    if (position_changed)
        hook_call ("playlist position", GINT_TO_POINTER (playlist_num));
    if (was_playing)
        change_playback (can_play);
}

EXPORT String aud_playlist_entry_get_filename (int playlist_num, int entry_num)
{
    ENTER_GET_ENTRY (String ());
    String filename = entry->filename;
    RETURN (filename);
}

EXPORT PluginHandle * aud_playlist_entry_get_decoder (int playlist_num, int entry_num, bool fast)
{
    ENTER;

    Entry * entry = get_entry (playlist_num, entry_num, ! fast, false);
    PluginHandle * decoder = entry ? entry->decoder : nullptr;

    RETURN (decoder);
}

EXPORT Tuple aud_playlist_entry_get_tuple (int playlist_num, int entry_num, bool fast)
{
    ENTER;

    Entry * entry = get_entry (playlist_num, entry_num, false, ! fast);
    Tuple tuple = entry->tuple.ref ();

    RETURN (tuple);
}

EXPORT String aud_playlist_entry_get_title (int playlist_num, int entry_num, bool fast)
{
    ENTER;

    Entry * entry = get_entry (playlist_num, entry_num, false, ! fast);
    String title = entry ? (entry->formatted ? entry->formatted : entry->title) : String ();

    RETURN (title);
}

EXPORT void aud_playlist_entry_describe (int playlist_num, int entry_num,
 String & title, String & artist, String & album, bool fast)
{
    ENTER;

    Entry * entry = get_entry (playlist_num, entry_num, false, ! fast);

    title = entry ? entry->title : String ();
    artist = entry ? entry->artist : String ();
    album = entry ? entry->album : String ();

    LEAVE;
}

EXPORT int aud_playlist_entry_get_length (int playlist_num, int entry_num, bool fast)
{
    ENTER;

    Entry * entry = get_entry (playlist_num, entry_num, false, ! fast);
    int length = entry ? entry->length : 0;

    RETURN (length);
}

EXPORT void aud_playlist_set_position (int playlist_num, int entry_num)
{
    ENTER_GET_PLAYLIST ();

    Entry * entry = lookup_entry (playlist, entry_num);
    bool was_playing = (playlist == playing_playlist);
    bool can_play = !! entry;

    set_position (playlist, entry, true);

    LEAVE;

    hook_call ("playlist position", GINT_TO_POINTER (playlist_num));
    if (was_playing)
        change_playback (can_play);
}

EXPORT int aud_playlist_get_position (int playlist_num)
{
    ENTER_GET_PLAYLIST (-1);
    int position = playlist->position ? playlist->position->number : -1;
    RETURN (position);
}

EXPORT void aud_playlist_set_focus (int playlist_num, int entry_num)
{
    ENTER_GET_PLAYLIST ();

    int first = INT_MAX;
    int last = -1;

    if (playlist->focus)
    {
        first = aud::min (first, playlist->focus->number);
        last = aud::max (last, playlist->focus->number);
    }

    playlist->focus = lookup_entry (playlist, entry_num);

    if (playlist->focus)
    {
        first = aud::min (first, playlist->focus->number);
        last = aud::max (last, playlist->focus->number);
    }

    if (first <= last)
        queue_update (PLAYLIST_UPDATE_SELECTION, playlist, first, last + 1 - first);

    LEAVE;
}

EXPORT int aud_playlist_get_focus (int playlist_num)
{
    ENTER_GET_PLAYLIST (-1);
    int focus = playlist->focus ? playlist->focus->number : -1;
    RETURN (focus);
}

EXPORT void aud_playlist_entry_set_selected (int playlist_num, int entry_num,
 bool selected)
{
    ENTER_GET_ENTRY ();

    if (entry->selected == selected)
        RETURN ();

    entry->selected = selected;

    if (selected)
    {
        playlist->selected_count++;
        playlist->selected_length += entry->length;
    }
    else
    {
        playlist->selected_count--;
        playlist->selected_length -= entry->length;
    }

    queue_update (PLAYLIST_UPDATE_SELECTION, playlist, entry_num, 1);
    LEAVE;
}

EXPORT bool aud_playlist_entry_get_selected (int playlist_num, int entry_num)
{
    ENTER_GET_ENTRY (false);
    bool selected = entry->selected;
    RETURN (selected);
}

EXPORT int aud_playlist_selected_count (int playlist_num)
{
    ENTER_GET_PLAYLIST (0);
    int selected_count = playlist->selected_count;
    RETURN (selected_count);
}

EXPORT void aud_playlist_select_all (int playlist_num, bool selected)
{
    ENTER_GET_PLAYLIST ();

    int entries = playlist->entries.len ();
    int first = entries, last = 0;

    for (auto & entry : playlist->entries)
    {
        if ((selected && ! entry->selected) || (entry->selected && ! selected))
        {
            entry->selected = selected;
            first = aud::min (first, entry->number);
            last = entry->number;
        }
    }

    if (selected)
    {
        playlist->selected_count = entries;
        playlist->selected_length = playlist->total_length;
    }
    else
    {
        playlist->selected_count = 0;
        playlist->selected_length = 0;
    }

    if (first < entries)
        queue_update (PLAYLIST_UPDATE_SELECTION, playlist, first, last + 1 - first);

    LEAVE;
}

EXPORT int aud_playlist_shift (int playlist_num, int entry_num, int distance)
{
    ENTER_GET_ENTRY (0);

    if (! entry->selected || ! distance)
        RETURN (0);

    int entries = playlist->entries.len ();
    int shift = 0, center, top, bottom;

    if (distance < 0)
    {
        for (center = entry_num; center > 0 && shift > distance; )
        {
            if (! playlist->entries[-- center]->selected)
                shift --;
        }
    }
    else
    {
        for (center = entry_num + 1; center < entries && shift < distance; )
        {
            if (! playlist->entries[center ++]->selected)
                shift ++;
        }
    }

    top = bottom = center;

    for (int i = 0; i < top; i ++)
    {
        if (playlist->entries[i]->selected)
            top = i;
    }

    for (int i = entries; i > bottom; i --)
    {
        if (playlist->entries[i - 1]->selected)
            bottom = i;
    }

    Index<SmartPtr<Entry>> temp;

    for (int i = top; i < center; i ++)
    {
        if (! playlist->entries[i]->selected)
            temp.append (std::move (playlist->entries[i]));
    }

    for (int i = top; i < bottom; i ++)
    {
        if (playlist->entries[i] && playlist->entries[i]->selected)
            temp.append (std::move (playlist->entries[i]));
    }

    for (int i = center; i < bottom; i ++)
    {
        if (playlist->entries[i] && ! playlist->entries[i]->selected)
            temp.append (std::move (playlist->entries[i]));
    }

    playlist->entries.move_from (temp, 0, top, bottom - top, false, true);

    number_entries (playlist, top, bottom - top);
    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, top, bottom - top);

    RETURN (shift);
}

static Entry * find_unselected_focus (Playlist * playlist)
{
    if (! playlist->focus || ! playlist->focus->selected)
        return playlist->focus;

    int entries = playlist->entries.len ();

    for (int search = playlist->focus->number + 1; search < entries; search ++)
    {
        Entry * entry = playlist->entries[search].get ();
        if (! entry->selected)
            return entry;
    }

    for (int search = playlist->focus->number; search --;)
    {
        Entry * entry = playlist->entries[search].get ();
        if (! entry->selected)
            return entry;
    }

    return nullptr;
}

EXPORT void aud_playlist_delete_selected (int playlist_num)
{
    ENTER_GET_PLAYLIST ();

    if (! playlist->selected_count)
        RETURN ();

    int entries = playlist->entries.len ();
    bool position_changed = false;
    bool was_playing = false;
    bool can_play = false;

    if (playlist->position && playlist->position->selected)
    {
        position_changed = true;
        was_playing = (playlist == playing_playlist);
        set_position (playlist, nullptr, false);
    }

    playlist->focus = find_unselected_focus (playlist);

    int before = 0;  // number of entries before first selected
    int after = 0;   // number of entries after last selected

    while (before < entries && ! playlist->entries[before]->selected)
        before ++;

    int to = before;

    for (int from = before; from < entries; from ++)
    {
        Entry * entry = playlist->entries[from].get ();

        if (entry->selected)
        {
            if (entry->queued)
                playlist->queued.remove (playlist->queued.find (entry), 1);

            playlist->total_length -= entry->length;
            after = 0;
        }
        else
        {
            playlist->entries[to ++] = std::move (playlist->entries[from]);
            after ++;
        }
    }

    entries = to;
    playlist->entries.remove (entries, -1);
    number_entries (playlist, before, entries - before);

    playlist->selected_count = 0;
    playlist->selected_length = 0;

    if (position_changed && aud_get_bool (nullptr, "advance_on_delete"))
        can_play = next_song_locked (playlist, aud_get_bool (nullptr, "repeat"), entries - after);

    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, before, entries - after - before);
    LEAVE;

    if (position_changed)
        hook_call ("playlist position", GINT_TO_POINTER (playlist_num));
    if (was_playing)
        change_playback (can_play);
}

EXPORT void aud_playlist_reverse (int playlist_num)
{
    ENTER_GET_PLAYLIST ();

    int entries = playlist->entries.len ();

    for (int i = 0; i < entries / 2; i ++)
        playlist->entries[i].swap (playlist->entries[entries - 1 - i]);

    number_entries (playlist, 0, entries);
    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, 0, entries);
    LEAVE;
}

EXPORT void aud_playlist_reverse_selected (int playlist_num)
{
    ENTER_GET_PLAYLIST ();

    int entries = playlist->entries.len ();

    int top = 0;
    int bottom = entries - 1;

    while (1)
    {
        while (top < bottom && ! playlist->entries[top]->selected)
            top ++;
        while (top < bottom && ! playlist->entries[bottom]->selected)
            bottom --;

        if (top >= bottom)
            break;

        playlist->entries[top ++].swap (playlist->entries[bottom --]);
    }

    number_entries (playlist, 0, entries);
    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, 0, entries);
    LEAVE;
}

EXPORT void aud_playlist_randomize (int playlist_num)
{
    ENTER_GET_PLAYLIST ();

    int entries = playlist->entries.len ();

    for (int i = 0; i < entries; i ++)
        playlist->entries[i].swap (playlist->entries[rand () % entries]);

    number_entries (playlist, 0, entries);
    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, 0, entries);
    LEAVE;
}

EXPORT void aud_playlist_randomize_selected (int playlist_num)
{
    ENTER_GET_PLAYLIST ();

    int entries = playlist->entries.len ();

    Index<Entry *> selected;

    for (auto & entry : playlist->entries)
    {
        if (entry->selected)
            selected.append (entry.get ());
    }

    int n_selected = selected.len ();

    for (int i = 0; i < n_selected; i ++)
    {
        int a = selected[i]->number;
        int b = selected[rand () % n_selected]->number;
        playlist->entries[a].swap (playlist->entries[b]);
    }

    number_entries (playlist, 0, entries);
    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, 0, entries);
    LEAVE;
}

enum {COMPARE_TYPE_FILENAME, COMPARE_TYPE_TUPLE, COMPARE_TYPE_TITLE};

struct CompareData {
    PlaylistStringCompareFunc filename_compare;
    PlaylistTupleCompareFunc tuple_compare;
    PlaylistStringCompareFunc title_compare;
};

static int compare_cb (const SmartPtr<Entry> & a, const SmartPtr<Entry> & b, void * _data)
{
    CompareData * data = (CompareData *) _data;

    int diff = 0;

    if (data->filename_compare)
        diff = data->filename_compare (a->filename, b->filename);
    else if (data->tuple_compare)
        diff = data->tuple_compare (a->tuple, b->tuple);
    else if (data->title_compare)
        diff = data->title_compare (a->formatted ? a->formatted : a->filename,
         b->formatted ? b->formatted : b->filename);

    if (diff)
        return diff;

    /* preserve order of "equal" entries */
    return a->number - b->number;
}

static void sort (Playlist * playlist, CompareData * data)
{
    playlist->entries.sort (compare_cb, data);
    number_entries (playlist, 0, playlist->entries.len ());

    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, 0, playlist->entries.len ());
}

static void sort_selected (Playlist * playlist, CompareData * data)
{
    int entries = playlist->entries.len ();

    Index<SmartPtr<Entry>> selected;

    for (auto & entry : playlist->entries)
    {
        if (entry->selected)
            selected.append (std::move (entry));
    }

    selected.sort (compare_cb, data);

    int i = 0;
    for (auto & entry : playlist->entries)
    {
        if (! entry)
            entry = std::move (selected[i ++]);
    }

    number_entries (playlist, 0, entries);
    queue_update (PLAYLIST_UPDATE_STRUCTURE, playlist, 0, entries);
}

static bool entries_are_scanned (Playlist * playlist, bool selected)
{
    for (auto & entry : playlist->entries)
    {
        if (selected && ! entry->selected)
            continue;

        if (! entry->tuple)
        {
            aud_ui_show_error (_("The playlist cannot be sorted because "
             "metadata scanning is still in progress (or has been disabled)."));
            return false;
        }
    }

    return true;
}

EXPORT void aud_playlist_sort_by_filename (int playlist_num, PlaylistStringCompareFunc compare)
{
    ENTER_GET_PLAYLIST ();

    CompareData data = {compare};
    sort (playlist, & data);

    LEAVE;
}

EXPORT void aud_playlist_sort_by_tuple (int playlist_num, PlaylistTupleCompareFunc compare)
{
    ENTER_GET_PLAYLIST ();

    CompareData data = {nullptr, compare};
    if (entries_are_scanned (playlist, false))
        sort (playlist, & data);

    LEAVE;
}

EXPORT void aud_playlist_sort_by_title (int playlist_num, PlaylistStringCompareFunc compare)
{
    ENTER_GET_PLAYLIST ();

    CompareData data = {nullptr, nullptr, compare};
    if (entries_are_scanned (playlist, false))
        sort (playlist, & data);

    LEAVE;
}

EXPORT void aud_playlist_sort_selected_by_filename (int playlist_num,
 PlaylistStringCompareFunc compare)
{
    ENTER_GET_PLAYLIST ();

    CompareData data = {compare};
    sort_selected (playlist, & data);

    LEAVE;
}

EXPORT void aud_playlist_sort_selected_by_tuple (int playlist_num,
 PlaylistTupleCompareFunc compare)
{
    ENTER_GET_PLAYLIST ();

    CompareData data = {nullptr, compare};
    if (entries_are_scanned (playlist, true))
        sort_selected (playlist, & data);

    LEAVE;
}

EXPORT void aud_playlist_sort_selected_by_title (int playlist_num,
 PlaylistStringCompareFunc compare)
{
    ENTER_GET_PLAYLIST ();

    CompareData data = {nullptr, nullptr, compare};
    if (entries_are_scanned (playlist, true))
        sort_selected (playlist, & data);

    LEAVE;
}

static void playlist_reformat_titles (void)
{
    ENTER;

    String format = aud_get_str (nullptr, "generic_title_format");
    title_formatter->compile (format);

    for (auto & playlist : playlists)
    {
        for (auto & entry : playlist->entries)
        {
            if (entry->tuple)
                entry->formatted = title_formatter->evaluate (entry->tuple);
            else
                entry->formatted = String ();
        }

        queue_update (PLAYLIST_UPDATE_METADATA, playlist.get (), 0, playlist->entries.len ());
    }

    LEAVE;
}

static void playlist_trigger_scan (void)
{
    ENTER;

    for (auto & playlist : playlists)
        playlist->scanning = true;

    scan_restart ();

    LEAVE;
}

static void playlist_rescan_real (int playlist_num, bool selected)
{
    ENTER_GET_PLAYLIST ();

    for (auto & entry : playlist->entries)
    {
        if (! selected || entry->selected)
            entry_set_tuple (playlist, entry.get (), Tuple ());
    }

    queue_update (PLAYLIST_UPDATE_METADATA, playlist, 0, playlist->entries.len ());
    LEAVE;
}

EXPORT void aud_playlist_rescan (int playlist_num)
{
    playlist_rescan_real (playlist_num, false);
}

EXPORT void aud_playlist_rescan_selected (int playlist_num)
{
    playlist_rescan_real (playlist_num, true);
}

EXPORT void aud_playlist_rescan_file (const char * filename)
{
    ENTER;

    for (auto & playlist : playlists)
    {
        for (auto & entry : playlist->entries)
        {
            if (! strcmp (entry->filename, filename))
            {
                entry_set_tuple (playlist.get (), entry.get (), Tuple ());
                queue_update (PLAYLIST_UPDATE_METADATA, playlist.get (), entry->number, 1);
            }
        }
    }

    LEAVE;
}

EXPORT int64_t aud_playlist_get_total_length (int playlist_num)
{
    ENTER_GET_PLAYLIST (0);
    int64_t length = playlist->total_length;
    RETURN (length);
}

EXPORT int64_t aud_playlist_get_selected_length (int playlist_num)
{
    ENTER_GET_PLAYLIST (0);
    int64_t length = playlist->selected_length;
    RETURN (length);
}

EXPORT int aud_playlist_queue_count (int playlist_num)
{
    ENTER_GET_PLAYLIST (0);
    int count = playlist->queued.len ();
    RETURN (count);
}

EXPORT void aud_playlist_queue_insert (int playlist_num, int at, int entry_num)
{
    ENTER_GET_ENTRY ();

    if (entry->queued || at > playlist->queued.len ())
        RETURN ();

    if (at < 0)
        playlist->queued.append (entry);
    else
    {
        playlist->queued.insert (at, 1);
        playlist->queued[at] = entry;
    }

    entry->queued = true;

    queue_update (PLAYLIST_UPDATE_SELECTION, playlist, entry_num, 1);
    LEAVE;
}

EXPORT void aud_playlist_queue_insert_selected (int playlist_num, int at)
{
    ENTER_GET_PLAYLIST ();

    if (at > playlist->queued.len ())
        RETURN ();

    Index<Entry *> add;
    int first = playlist->entries.len ();
    int last = 0;

    for (auto & entry : playlist->entries)
    {
        if (! entry->selected || entry->queued)
            continue;

        add.append (entry.get ());
        entry->queued = true;
        first = aud::min (first, entry->number);
        last = entry->number;
    }

    playlist->queued.move_from (add, 0, at, -1, true, true);

    if (first < playlist->entries.len ())
        queue_update (PLAYLIST_UPDATE_SELECTION, playlist, first, last + 1 - first);

    LEAVE;
}

EXPORT int aud_playlist_queue_get_entry (int playlist_num, int at)
{
    ENTER_GET_PLAYLIST (-1);

    int entry_num = -1;
    if (at >= 0 && at < playlist->queued.len ())
        entry_num = playlist->queued[at]->number;

    RETURN (entry_num);
}

EXPORT int aud_playlist_queue_find_entry (int playlist_num, int entry_num)
{
    ENTER_GET_ENTRY (-1);
    int pos = entry->queued ? playlist->queued.find (entry) : -1;
    RETURN (pos);
}

EXPORT void aud_playlist_queue_delete (int playlist_num, int at, int number)
{
    ENTER_GET_PLAYLIST ();

    if (at < 0 || number < 0 || at + number > playlist->queued.len ())
        RETURN ();

    int entries = playlist->entries.len ();
    int first = entries, last = 0;

    for (int i = at; i < at + number; i ++)
    {
        Entry * entry = playlist->queued[i];
        entry->queued = false;
        first = aud::min (first, entry->number);
        last = entry->number;
    }

    playlist->queued.remove (at, number);

    if (first < entries)
        queue_update (PLAYLIST_UPDATE_SELECTION, playlist, first, last + 1 - first);

    LEAVE;
}

EXPORT void aud_playlist_queue_delete_selected (int playlist_num)
{
    ENTER_GET_PLAYLIST ();

    int entries = playlist->entries.len ();
    int first = entries, last = 0;

    for (int i = 0; i < playlist->queued.len ();)
    {
        Entry * entry = playlist->queued[i];

        if (entry->selected)
        {
            playlist->queued.remove (i, 1);
            entry->queued = false;
            first = aud::min (first, entry->number);
            last = entry->number;
        }
        else
            i ++;
    }

    if (first < entries)
        queue_update (PLAYLIST_UPDATE_SELECTION, playlist, first, last + 1 - first);

    LEAVE;
}

static bool shuffle_prev (Playlist * playlist)
{
    Entry * found = nullptr;

    for (auto & entry : playlist->entries)
    {
        if (entry->shuffle_num && (! playlist->position ||
         entry->shuffle_num < playlist->position->shuffle_num) && (! found
         || entry->shuffle_num > found->shuffle_num))
            found = entry.get ();
    }

    if (! found)
        return false;

    set_position (playlist, found, false);
    return true;
}

bool playlist_prev_song (int playlist_num)
{
    ENTER_GET_PLAYLIST (false);

    bool was_playing = (playlist == playing_playlist);

    if (aud_get_bool (nullptr, "shuffle"))
    {
        if (! shuffle_prev (playlist))
            RETURN (false);
    }
    else
    {
        if (! playlist->position || playlist->position->number == 0)
            RETURN (false);

        set_position (playlist, playlist->entries[playlist->position->number - 1].get (), true);
    }

    LEAVE;

    hook_call ("playlist position", GINT_TO_POINTER (playlist_num));
    if (was_playing)
        change_playback (true);

    return true;
}

static bool shuffle_next (Playlist * playlist)
{
    int choice = 0;
    Entry * found = nullptr;

    for (auto & entry : playlist->entries)
    {
        if (! entry->shuffle_num)
            choice ++;
        else if (playlist->position && entry->shuffle_num >
         playlist->position->shuffle_num && (! found || entry->shuffle_num
         < found->shuffle_num))
            found = entry.get ();
    }

    if (found)
    {
        set_position (playlist, found, false);
        return true;
    }

    if (! choice)
        return false;

    choice = rand () % choice;

    for (auto & entry : playlist->entries)
    {
        if (! entry->shuffle_num)
        {
            if (! choice)
            {
                set_position (playlist, entry.get (), true);
                break;
            }

            choice --;
        }
    }

    return true;
}

static void shuffle_reset (Playlist * playlist)
{
    playlist->last_shuffle_num = 0;

    for (auto & entry : playlist->entries)
        entry->shuffle_num = 0;
}

static bool next_song_locked (Playlist * playlist, bool repeat, int hint)
{
    int entries = playlist->entries.len ();
    if (! entries)
        return false;

    if (playlist->queued.len ())
    {
        set_position (playlist, playlist->queued[0], true);
        playlist->queued.remove (0, 1);
        playlist->position->queued = false;
    }
    else if (aud_get_bool (nullptr, "shuffle"))
    {
        if (! shuffle_next (playlist))
        {
            if (! repeat)
                return false;

            shuffle_reset (playlist);

            if (! shuffle_next (playlist))
                return false;
        }
    }
    else
    {
        if (hint >= entries)
        {
            if (! repeat)
                return false;

            hint = 0;
        }

        set_position (playlist, playlist->entries[hint].get (), true);
    }

    return true;
}

bool playlist_next_song (int playlist_num, bool repeat)
{
    ENTER_GET_PLAYLIST (false);

    int hint = playlist->position ? playlist->position->number + 1 : 0;
    bool was_playing = (playlist == playing_playlist);

    if (! next_song_locked (playlist, repeat, hint))
        RETURN (false);

    LEAVE;

    hook_call ("playlist position", GINT_TO_POINTER (playlist_num));
    if (was_playing)
        change_playback (true);

    return true;
}

int playback_entry_get_position (void)
{
    ENTER;

    Entry * entry = get_playback_entry (false, false);
    int entry_num = entry ? entry->number : -1;

    RETURN (entry_num);
}

String playback_entry_get_filename (void)
{
    ENTER;

    Entry * entry = get_playback_entry (false, false);
    String filename = entry ? entry->filename : String ();

    RETURN (filename);
}

PluginHandle * playback_entry_get_decoder (void)
{
    ENTER;

    Entry * entry = get_playback_entry (true, false);
    PluginHandle * decoder = entry ? entry->decoder : nullptr;

    RETURN (decoder);
}

Tuple playback_entry_get_tuple (void)
{
    ENTER;

    Entry * entry = get_playback_entry (false, true);
    Tuple tuple = entry->tuple.ref ();

    RETURN (tuple);
}

String playback_entry_get_title (void)
{
    ENTER;

    Entry * entry = get_playback_entry (false, true);
    String title = entry ? (entry->formatted ? entry->formatted : entry->title) : String ();

    RETURN (title);
}

int playback_entry_get_length (void)
{
    ENTER;

    Entry * entry = get_playback_entry (false, true);
    int length = entry ? entry->length : 0;

    RETURN (length);
}

void playback_entry_set_tuple (Tuple && tuple)
{
    ENTER;
    if (! playing_playlist || ! playing_playlist->position)
        RETURN ();

    Entry * entry = playing_playlist->position;
    entry_set_tuple (playing_playlist, entry, std::move (tuple));

    queue_update (PLAYLIST_UPDATE_METADATA, playing_playlist, entry->number, 1);
    LEAVE;
}

void playlist_save_state (void)
{
    /* get playback state before locking playlists */
    bool paused = aud_drct_get_paused ();
    int time = aud_drct_get_time ();

    ENTER;

    const char * user_dir = aud_get_path (AudPath::UserDir);
    StringBuf path = filename_build ({user_dir, STATE_FILE});

    FILE * handle = g_fopen (path, "w");
    if (! handle)
        RETURN ();

    fprintf (handle, "active %d\n", active_playlist ? active_playlist->number : -1);
    fprintf (handle, "playing %d\n", playing_playlist ? playing_playlist->number : -1);

    for (auto & playlist : playlists)
    {
        fprintf (handle, "playlist %d\n", playlist->number);

        if (playlist->filename)
            fprintf (handle, "filename %s\n", (const char *) playlist->filename);

        fprintf (handle, "position %d\n", playlist->position ? playlist->position->number : -1);

        if (playlist.get () == playing_playlist)
        {
            playlist->resume_paused = paused;
            playlist->resume_time = time;
        }

        fprintf (handle, "resume-state %d\n", paused ? RESUME_PAUSE : RESUME_PLAY);
        fprintf (handle, "resume-time %d\n", playlist->resume_time);
    }

    fclose (handle);
    LEAVE;
}

static char parse_key[512];
static char * parse_value;

static void parse_next (FILE * handle)
{
    parse_value = nullptr;

    if (! fgets (parse_key, sizeof parse_key, handle))
        return;

    char * space = strchr (parse_key, ' ');
    if (! space)
        return;

    * space = 0;
    parse_value = space + 1;

    char * newline = strchr (parse_value, '\n');
    if (newline)
        * newline = 0;
}

static bool parse_integer (const char * key, int * value)
{
    return (parse_value && ! strcmp (parse_key, key) && sscanf (parse_value, "%d", value) == 1);
}

static String parse_string (const char * key)
{
    return (parse_value && ! strcmp (parse_key, key)) ? String (parse_value) : String ();
}

void playlist_load_state (void)
{
    ENTER;
    int playlist_num;

    const char * user_dir = aud_get_path (AudPath::UserDir);
    StringBuf path = filename_build ({user_dir, STATE_FILE});

    FILE * handle = g_fopen (path, "r");
    if (! handle)
        RETURN ();

    parse_next (handle);

    if (parse_integer ("active", & playlist_num))
    {
        if (! (active_playlist = lookup_playlist (playlist_num)))
            active_playlist = playlists[0].get ();
        parse_next (handle);
    }

    if (parse_integer ("playing", & resume_playlist))
        parse_next (handle);

    while (parse_integer ("playlist", & playlist_num) && playlist_num >= 0 &&
     playlist_num < playlists.len ())
    {
        Playlist * playlist = playlists[playlist_num].get ();
        int entries = playlist->entries.len ();

        parse_next (handle);

        playlist->filename = parse_string ("filename");
        if (playlist->filename)
            parse_next (handle);

        int position = -1;
        if (parse_integer ("position", & position))
            parse_next (handle);

        if (position >= 0 && position < entries)
            set_position (playlist, playlist->entries [position].get (), true);

        int resume_state = RESUME_PLAY;
        if (parse_integer ("resume-state", & resume_state))
            parse_next (handle);

        playlist->resume_paused = (resume_state == RESUME_PAUSE);

        if (parse_integer ("resume-time", & playlist->resume_time))
            parse_next (handle);

        /* compatibility with Audacious 3.3 */
        if (playlist_num == resume_playlist && resume_state == RESUME_STOP)
            resume_playlist = -1;
    }

    fclose (handle);

    /* clear updates queued during init sequence */

    for (auto & playlist : playlists)
    {
        playlist->next_update = Update ();
        playlist->last_update = Update ();
    }

    queued_update.stop ();
    update_level = 0;

    LEAVE;
}

EXPORT void aud_resume (void)
{
    aud_playlist_set_playing (resume_playlist);
}
