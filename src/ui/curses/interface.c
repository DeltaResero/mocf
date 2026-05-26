// src/ui/curses/interface.c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2004 - 2006 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdarg.h>
#include <locale.h>
#include <assert.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>

#ifdef HAVE_SYS_INOTIFY_H
#include <sys/inotify.h>
#endif

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "ui/curses/interface_elements.h"
#include "ui/curses/interface.h"
#include "utils/lists.h"
#include "library/playlist.h"
#include "library/playlist_file.h"
#include "core/protocol.h"
#include "core/server.h"
#include "audio/audio.h"
#include "ui/input/keys.h"
#include "core/options.h"
#include "library/files.h"
#include "audio/decoder.h"
#include "ui/themes.h"
#include "audio/processing/softmixer.h"
#include "utils/utf8.h"

#define PLAYLIST_FILE "playlist.m3u"

#define QUEUE_CLEAR_THRESH 128

/* Engine event queue (replaces the socketpair). */
static struct engine_event_queue *g_engine_eq = NULL;

static struct plist *playlist = NULL;  /* our playlist */
static struct plist *queue = NULL;     /* our queue */
static struct plist *dir_plist = NULL; /* contents of the current directory */

/* Queue for events coming from the engine. */
static struct event_queue events;

/* Current working directory (the directory we show). */
static char cwd[PATH_MAX] = "";

/* If the user presses quit, or we receive a termination signal. */
static volatile enum want_quit want_quit = NO_QUIT;

/* Set to true when the UI playlist has been modified and differs from what
 * the engine last received. Replaces the legacy serial-number sync logic. */
static bool playlist_dirty = false;

/* Pointer to whichever plist the engine currently has loaded (NULL = none).
 * Used alongside playlist_dirty to decide when a full resend is needed. */
static struct plist *engine_plist = NULL;

/* If user presses CTRL-C, set this to 1.  This should interrupt long
 * operations which block the interface. */
static volatile int wants_interrupt = 0;

#ifdef SIGWINCH
/* If we get SIGWINCH. */
static volatile int want_resize = 0;
#endif

/* Information about the currently played file. */
static struct file_info curr_file;

/* Silent seeking - where we are in seconds. -1 - no seeking. */
static int silent_seek_pos = -1;
static time_t silent_seek_key_last = (time_t)0; /* when the silent seek key was
               last used */

/* When the menu was last moved (arrow keys, page up, etc.) */
static time_t last_menu_move_time = (time_t)0;

/* File descriptors for inotify (inotify, watch) */
#ifdef HAVE_SYS_INOTIFY_H
static int inotify_fd = -1;
static int inotify_wd = -1;
#endif

static void sig_quit(int sig LOGIT_ONLY)
{
  log_signal(sig);
  want_quit = QUIT_APP;
}

static void sig_interrupt(int sig LOGIT_ONLY)
{
  log_signal(sig);
  wants_interrupt = 1;
}

#ifdef SIGWINCH
static void sig_winch(int sig LOGIT_ONLY)
{
  log_signal(sig);
  want_resize = 1;
}
#endif

int user_wants_interrupt() { return wants_interrupt; }

static void clear_interrupt() { wants_interrupt = 0; }

/* -----------------------------------------------------------------------
 * Direct-call helpers replacing the old socketpair protocol wrappers.
 * ----------------------------------------------------------------------- */

/* Drain all pending engine events into the local event queue. */
static void drain_engine_events()
{
  engine_event_queue_flush(g_engine_eq, &events);
}

/* Block until at least one engine event arrives, then drain. */
static void wait_and_drain_engine_events()
{
  engine_event_queue_wait_flush(g_engine_eq, &events);
}

static int send_tags_request(const char *file, const int tags_sel)
{
  assert(file != NULL);
  assert(tags_sel != 0);

  if (file_type(file) == F_SOUND)
  {
    engine_request_file_tags(file, tags_sel);
    debug("Asking for tags for %s", file);
    return 1;
  }
  else
  {
    debug("Not sending tags request for URL (%s)", file);
    return 0;
  }
}


static void init_playlists()
{
  dir_plist = (struct plist *)xmalloc(sizeof(struct plist));
  plist_init(dir_plist);
  playlist = (struct plist *)xmalloc(sizeof(struct plist));
  plist_init(playlist);
  queue = (struct plist *)xmalloc(sizeof(struct plist));
  plist_init(queue);
}

static void file_info_reset(struct file_info *f)
{
  f->file = NULL;
  f->tags = NULL;
  f->title = NULL;
  f->bitrate = -1;
  f->rate = -1;
  f->curr_time = -1;
  f->total_time = -1;
  f->channels = 1;
  f->state = STATE_STOP;
}

static void file_info_cleanup(struct file_info *f)
{
  if (f->tags)
  {
    tags_free(f->tags);
  }
  if (f->file)
  {
    free(f->file);
  }
  if (f->title)
  {
    free(f->title);
  }

  f->file = NULL;
  f->tags = NULL;
  f->title = NULL;
}

/* Initialise the block marker information. */
static void file_info_block_init(struct file_info *f) { f->block_file = NULL; }

/* Reset the block start and end markers. */
static void file_info_block_reset(struct file_info *f)
{
  if (f->block_file)
  {
    free(f->block_file);
  }
  f->block_file = NULL;
}

/* Enter the current time into a block start or end marker. */
static void file_info_block_mark(int *marker)
{
  if (curr_file.state == STATE_STOP)
  {
    error("Cannot make block marks while stopped.");
  }
  else if (file_type(curr_file.file) != F_SOUND)
  {
    error("Cannot make block marks in non-audio files.");
  }
  else if (!curr_file.block_file)
  {
    error("Cannot make block marks in files of unknown duration.");
  }
  else
  {
    *marker = curr_file.curr_time;
    iface_set_block(curr_file.block_start, curr_file.block_end);
  }
}

/* Sync a boolean option from engine state into the UI. Since the engine and
 * UI share the same options_* globals, we just re-read the local value and
 * update the iface display. */
static void sync_bool_option(const char *name)
{
  bool value = options_get_bool(name);
  iface_set_option_state(name, value);
}

/* Refresh all option display states from the shared options store. */
static void get_engine_options()
{
  sync_bool_option("Shuffle");
  sync_bool_option("Repeat");
  sync_bool_option("AutoNext");
}

static int get_mixer_value()
{
  return audio_get_mixer();
}

static int get_state()
{
  return audio_get_state();
}

static int get_channels()
{
  return engine_get_channels();
}

static int get_rate()
{
  return engine_get_rate();
}

static int get_bitrate()
{
  return engine_get_bitrate();
}

static int get_avg_bitrate()
{
  return engine_get_avg_bitrate();
}

static int get_curr_time()
{
  return audio_get_time();
}

static char *get_curr_file()
{
  char *s = audio_get_sname();
  return s ? s : xstrdup("");
}

static void update_mixer_value()
{
  int val = get_mixer_value();
  iface_set_mixer_value(MAX(val, 0));
}

static void update_mixer_name()
{
  char *name = audio_get_mixer_channel_name();
  debug("Mixer name: %s", name);
  iface_set_mixer_name(name);
  free(name);
  update_mixer_value();
}

/* Make new cwd path from CWD and this path. */
static void set_cwd(const char *path)
{
  if (path[0] == '/')
  {
    strcpy(cwd, "/"); /* for absolute path */
  }
  else if (!cwd[0])
  {
    if (!getcwd(cwd, sizeof(cwd)))
    {
      fatal("Can't get CWD: %s", xstrerror(errno));
    }
  }

  resolve_path(cwd, sizeof(cwd), path);
}

/* Try to find the directory we can start and set cwd to it. */
static void set_start_dir()
{
  if (!getcwd(cwd, sizeof(cwd)))
  {
    if (errno == ERANGE)
    {
      fatal("CWD is larger than PATH_MAX!");
    }
    const char *home = get_home();
    if (strlen(home) >= sizeof(cwd))
    {
      fatal("Home directory path is longer than PATH_MAX!");
    }
    strcpy(cwd, home);
  }
}

/* Set cwd to last directory written to a file, return 1 on success. */
static int read_last_dir()
{
  FILE *dir_file;
  int res = 1;
  int read;

  if (!(dir_file = fopen(create_file_name("last_directory"), "r")))
  {
    return 0;
  }

  if ((read = fread(cwd, sizeof(char), sizeof(cwd) - 1, dir_file)) == 0)
  {
    res = 0;
  }
  else
  {
    cwd[read] = 0;
  }

  fclose(dir_file);
  return res;
}

/* Check if dir2 is in dir1. */
static int is_subdir(const char *dir1, const char *dir2)
{
  return !strncmp(dir1, dir2, strlen(dir1)) ? 1 : 0;
}

static int sort_strcmp_func(const void *a, const void *b)
{
  return strcoll(*(char **)a, *(char **)b);
}

static int sort_dirs_func(const void *a, const void *b)
{
  char *sa = *(char **)a;
  char *sb = *(char **)b;

  /* '../' is always first */
  if (!strcmp(sa, "../"))
  {
    return -1;
  }
  if (!strcmp(sb, "../"))
  {
    return 1;
  }

  return strcmp(sa, sb);
}

static int get_tags_setting()
{
  int needed_tags = 0;

  if (options_get_bool("ReadTags"))
  {
    needed_tags |= TAGS_COMMENTS;
  }
  if (strcasecmp(options_get_symb("ShowTime"), "no"))
  {
    needed_tags |= TAGS_TIME;
  }

  return needed_tags;
}

/* For each file in the playlist, send a request for all the given tags if
 * the file is missing any of those tags.  Return the number of requests. */
static int ask_for_tags(const struct plist *plist, const int tags_sel)
{
  int i;
  int req = 0;

  assert(plist != NULL);

  if (tags_sel != 0)
  {
    for (i = 0; i < plist->num; i++)
    {
      if (!plist_deleted(plist, i) &&
          (!plist->items[i].tags || ~plist->items[i].tags->filled & tags_sel))
      {
        char *file;

        file = plist_get_file(plist, i);
        req += send_tags_request(file, tags_sel);
        free(file);
      }
    }
  }

  return req;
}

static void interface_message(const char *format, ...)
{
  va_list va;
  char *msg;

  va_start(va, format);
  msg = format_msg_va(format, va);
  va_end(va);

  iface_message(msg);

  free(msg);
}

/* Update tags (and titles) for the given item on the playlist with new tags. */
static void update_item_tags(struct plist *plist, const int num,
                             struct file_tags *tags)
{
  struct file_tags *old_tags = plist_get_tags(plist, num);

  /* Get the tags from the old tags if it's not present in the new tags.
   * FIXME: There is risk, that the file was modified and the time
   * from the old tags is not valid. */
  if (old_tags)
  {
    tags_update(tags, old_tags, 1);
  }

  plist_set_tags(plist, num, tags);

  if (plist->items[num].title_tags)
  {
    free(plist->items[num].title_tags);
    plist->items[num].title_tags = NULL;
  }

  make_tags_title(plist, num);

  if (options_get_bool("ReadTags") && !plist->items[num].title_tags)
  {
    if (!plist->items[num].title_file)
    {
      make_file_title(plist, num, options_get_bool("HideFileExtension"));
    }
  }

  if (old_tags)
  {
    tags_free(old_tags);
  }
}

/* Truncate string at screen-upsetting whitespace. */
static void sanitise_string(char *str)
{
  if (!str)
  {
    return;
  }

  while (*str)
  {
    if (*str != ' ' && isspace(*str))
    {
      *str = 0x00;
      break;
    }
    str++;
  }
}

/* Handle EV_FILE_TAGS. */
static void ev_file_tags(const struct tag_ev_response *data)
{
  int n;

  assert(data != NULL);
  assert(data->file != NULL);
  assert(data->tags != NULL);

  debug("Received tags for %s", data->file);

  sanitise_string(data->tags->title);
  if (data->tags->title && !data->tags->title[0])
  {
    free(data->tags->title);
    ((struct file_tags *)data->tags)->title = NULL;
  }

  sanitise_string(data->tags->artist);
  if (data->tags->artist && !data->tags->artist[0])
  {
    free(data->tags->artist);
    ((struct file_tags *)data->tags)->artist = NULL;
  }

  sanitise_string(data->tags->album);
  if (data->tags->album && !data->tags->album[0])
  {
    free(data->tags->album);
    ((struct file_tags *)data->tags)->album = NULL;
  }

  if ((n = plist_find_fname(dir_plist, data->file)) != -1)
  {
    update_item_tags(dir_plist, n, data->tags);
    iface_update_item(IFACE_MENU_DIR, dir_plist, n);
  }

  if ((n = plist_find_fname(playlist, data->file)) != -1)
  {
    update_item_tags(playlist, n, data->tags);
    iface_update_item(IFACE_MENU_PLIST, playlist, n);
  }

  if (curr_file.file && !strcmp(data->file, curr_file.file))
  {
    debug("Tags apply to the currently played file.");

    if (data->tags->time != -1)
    {
      curr_file.total_time = data->tags->time;
      iface_set_total_time(curr_file.total_time);
      if (file_type(curr_file.file) == F_SOUND)
      {
        if (!curr_file.block_file)
        {
          curr_file.block_file = xstrdup(curr_file.file);
          curr_file.block_start = 0;
          curr_file.block_end = curr_file.total_time;
        }
        iface_set_block(curr_file.block_start, curr_file.block_end);
      }
    }
    else
    {
      debug("No time information");
    }

    if (data->tags->title)
    {
      if (curr_file.title)
      {
        free(curr_file.title);
      }
      curr_file.title = build_title(data->tags);
      iface_set_played_file_title(curr_file.title);
    }

    if (curr_file.tags)
    {
      tags_free(curr_file.tags);
    }
    curr_file.tags = tags_dup(data->tags);
  }
}

/* Update the current time. */
static void update_ctime()
{
  curr_file.curr_time = get_curr_time();
  if (silent_seek_pos == -1)
  {
    iface_set_curr_time(curr_file.curr_time);
  }
}



/* Make sure that the currently played file is visible if it is in one of our
 * menus. */
static void follow_curr_file()
{
  if (curr_file.file && file_type(curr_file.file) == F_SOUND &&
      last_menu_move_time <= time(NULL) - 2)
  {
    if (plist_find_fname(playlist, curr_file.file) != -1)
    {
      iface_make_visible(IFACE_MENU_PLIST, curr_file.file);
    }
    else if (plist_find_fname(dir_plist, curr_file.file) != -1)
    {
      iface_make_visible(IFACE_MENU_DIR, curr_file.file);
    }
    else
    {
      logit("Not my playlist.");
    }
  }
}

static void update_curr_file()
{
  char *file;

  file = get_curr_file();

  if (!file[0] || curr_file.state == STATE_STOP)
  {
    /* Nothing is played/paused. */

    file_info_cleanup(&curr_file);
    file_info_reset(&curr_file);
    iface_set_played_file(NULL);
    free(file);
  }
  else if (file[0] && (!curr_file.file || strcmp(file, curr_file.file)))
  {
    /* played file has changed */

    file_info_cleanup(&curr_file);
    if (curr_file.block_file && strcmp(file, curr_file.block_file))
    {
      file_info_block_reset(&curr_file);
    }

    /* The total time could not get reset. */
    iface_set_total_time(-1);

    iface_set_played_file(file);
    send_tags_request(file, TAGS_COMMENTS | TAGS_TIME);
    curr_file.file = file;

    /* make a title that will be used until we get tags */
    if (!strchr(file, '/'))
    {
      curr_file.title = xstrdup(file);
    }
    else
    {
      if (options_get_bool("FileNamesIconv"))
      {
        curr_file.title = files_iconv_str(strrchr(file, '/') + 1);
      }
      else
      {
        curr_file.title = xstrdup(strrchr(file, '/') + 1);
      }
    }

    iface_set_played_file(file);
    iface_set_played_file_title(curr_file.title);
    /* Silent seeking makes no sense if the playing file has changed. */
    silent_seek_pos = -1;
    iface_set_curr_time(curr_file.curr_time);

    if (options_get_bool("FollowPlayedFile"))
    {
      follow_curr_file();
    }
  }
  else
  {
    free(file);
  }
}

static void update_rate()
{
  curr_file.rate = get_rate();
  iface_set_rate(curr_file.rate);
}

static void update_channels()
{
  curr_file.channels = get_channels() == 2 ? 2 : 1;
  iface_set_channels(curr_file.channels);
}

static void update_bitrate()
{
  curr_file.bitrate = get_bitrate();
  iface_set_bitrate(curr_file.bitrate);
}

/* Get and show the engine state. */
static void update_state()
{
  int old_state = curr_file.state;

  /* play | stop | pause */
  curr_file.state = get_state();
  iface_set_state(curr_file.state);

  /* Silent seeking makes no sense if the state has changed. */
  if (old_state != curr_file.state)
  {
    silent_seek_pos = -1;
  }

  update_curr_file();

  update_channels();
  update_bitrate();
  update_rate();
  update_ctime();
}

/* Update playlist with a new item. */
static void event_plist_add(const struct plist_item *item)
{
  if (plist_find_fname(playlist, item->file) == -1)
  {
    int item_num = plist_add_from_item(playlist, item);
    int needed_tags = 0;
    int i;

    if (options_get_bool("ReadTags") && (!item->tags || !item->tags->title))
    {
      needed_tags |= TAGS_COMMENTS;
    }
    if (!strcasecmp(options_get_symb("ShowTime"), "yes") &&
        (!item->tags || item->tags->time == -1))
    {
      needed_tags |= TAGS_TIME;
    }

    if (needed_tags)
    {
      send_tags_request(item->file, needed_tags);
    }

    if (options_get_bool("ReadTags"))
    {
      make_tags_title(playlist, item_num);
    }
    else
    {
      make_file_title(playlist, item_num,
                      options_get_bool("HideFileExtension"));
    }

    /* Update queue position if this file is already queued. */
    if ((i = plist_find_fname(queue, item->file)) != -1)
    {
      playlist->items[item_num].queue_pos = plist_get_position(queue, i);
    }

    iface_add_to_plist(playlist, item_num);
  }
}

/* Handle EV_QUEUE_ADD. */
static void event_queue_add(const struct plist_item *item)
{
  if (plist_find_fname(queue, item->file) == -1)
  {
    plist_add_from_item(queue, item);
    iface_set_files_in_queue(plist_count(queue));
    iface_update_queue_position_last(queue, playlist, dir_plist);
    logit("Adding %s to queue", item->file);
  }
  else
  {
    logit("Adding file already present in queue");
  }
}

/* Get error message from the engine and show it.  If the message carries an
 * embedded file path (format: "\x01<path>\x01<message>"), mark that file in
 * the menu and display only the message portion.  This prefix is added by
 * play_file() when a fatal open error occurs so we know which file failed
 * even after the engine has already moved on to the next track. */
static void update_error(char *err)
{
  if (err[0] == '\x01')
  {
    char *sep = strchr(err + 1, '\x01');
    if (sep)
    {
      *sep = '\0';
      const char *failed_file = err + 1;
      const char *message     = sep + 1;

      error("%s", message);
      iface_mark_file_error(failed_file);
      *sep = '\x01'; /* restore for caller's free() */
      return;
    }
  }

  error("%s", err);
}


static void recv_engine_queue(struct plist *q)
{
  struct plist *engine_q;
  int i;

  logit("Getting queue from engine.");
  engine_q = engine_get_queue();
  if (!engine_q)
    return;

  for (i = 0; i < engine_q->num; i++)
  {
    if (!plist_deleted(engine_q, i))
      plist_add_from_item(q, &engine_q->items[i]);
  }

  plist_free(engine_q);
  free(engine_q);
}

/* Clear the playlist locally. */
static void clear_playlist()
{
  if (iface_in_plist_menu())
  {
    iface_switch_to_dir();
  }
  plist_clear(playlist);
  iface_clear_plist();
  interface_message("The playlist was cleared.");
  iface_set_status("");
  playlist_dirty = true;
}

static void clear_queue()
{
  iface_clear_queue_positions(queue, playlist, dir_plist);

  plist_clear(queue);
  iface_set_files_in_queue(0);

  interface_message("The queue was cleared.");
}

/* Remove an item from the playlist. */
static void event_plist_del(char *file)
{
  int item = plist_find_fname(playlist, file);

  if (item != -1)
  {
    char *file;
    int have_all_times;
    int playlist_total_time;

    file = plist_get_file(playlist, item);
    plist_delete(playlist, item);

    iface_del_plist_item(file);
    playlist_total_time = plist_total_time(playlist, &have_all_times);
    iface_plist_set_total_time(playlist_total_time, have_all_times);
    free(file);

    if (plist_count(playlist) == 0)
    {
      clear_playlist();
    }
  }
  else
  {
    logit("Engine requested deleting an item not present on the"
          " playlist.");
  }
}

/* Handle EV_QUEUE_DEL. */
static void event_queue_del(char *file)
{
  int item = plist_find_fname(queue, file);

  if (item != -1)
  {
    plist_delete(queue, item);

    /* Free the deleted items occasionally.
     * QUEUE_CLEAR_THRESH is chosen to be two times
     * the initial size of the playlist. */
    if (plist_count(queue) == 0 && queue->num >= QUEUE_CLEAR_THRESH)
    {
      plist_clear(queue);
    }

    iface_set_files_in_queue(plist_count(queue));
    iface_update_queue_positions(queue, playlist, dir_plist, file);
    logit("Deleting %s from queue", file);
  }
  else
  {
    logit("Deleting an item not present in the queue");
  }
}

/* Swap 2 file on the playlist. */
static void swap_playlist_items(const char *file1, const char *file2)
{
  assert(file1 != NULL);
  assert(file2 != NULL);

  plist_swap_files(playlist, file1, file2);
  iface_swap_plist_items(file1, file2);
  playlist_dirty = true;
}

/* Move an item in the playlist. */
static void event_plist_move(const struct move_ev_data *d)
{
  assert(d != NULL);
  assert(d->from != NULL);
  assert(d->to != NULL);

  swap_playlist_items(d->from, d->to);
}

/* Handle EV_QUEUE_MOVE. */
static void event_queue_move(const struct move_ev_data *d)
{
  assert(d != NULL);
  assert(d->from != NULL);
  assert(d->to != NULL);

  plist_swap_files(queue, d->from, d->to);
}

/* Handle server event. */
static void server_event(const int event, void *data)
{
  logit("EVENT: 0x%02x", event);

  switch (event)
  {
    case EV_CTIME:
      update_ctime();
      break;
    case EV_STATE:
      update_state();
      break;
    case EV_BITRATE:
      update_bitrate();
      break;
    case EV_RATE:
      update_rate();
      break;
    case EV_CHANNELS:
      update_channels();
      break;
    case EV_SRV_ERROR:
      update_error((char *)data);
      break;
    case EV_OPTIONS:
      get_engine_options();
      break;

    case EV_STATUS_MSG:
      iface_set_status((char *)data);
      break;
    case EV_MIXER_CHANGE:
      update_mixer_name();
      break;
    case EV_FILE_TAGS:
      ev_file_tags((struct tag_ev_response *)data);
      break;
    case EV_AVG_BITRATE:
      curr_file.avg_bitrate = get_avg_bitrate();
      break;
    case EV_QUEUE_ADD:
      event_queue_add((struct plist_item *)data);
      break;
    case EV_QUEUE_DEL:
      event_queue_del((char *)data);
      break;
    case EV_QUEUE_CLEAR:
      clear_queue();
      break;
    case EV_QUEUE_MOVE:
      event_queue_move((struct move_ev_data *)data);
      break;
    case EV_AUDIO_START:
      break;
    case EV_AUDIO_STOP:
      break;
    default:
      interface_fatal("Unknown event: 0x%02x!", event);
  }

  free_event_data(event, data);
}

/* Send requests for the given tags for every file on the playlist and wait
 * for all responses. If no_iface has non-zero value, it will not access the
 * interface. */
static void fill_tags(struct plist *plist, const int tags_sel,
                      const int no_iface)
{
  int files;

  assert(plist != NULL);
  assert(tags_sel != 0);

  iface_set_status("Reading tags...");
  files = ask_for_tags(plist, tags_sel);

  /* Process events until we have all tags. */
  while (files && !user_wants_interrupt())
  {
    int type;
    void *data;

    if (!no_iface && !event_queue_empty(&events))
    {
      struct event e = *event_get_first(&events);
      type = e.type;
      data = e.data;
      event_pop(&events);
    }
    else
    {
      /* Block until at least one event arrives from the engine. */
      wait_and_drain_engine_events();

      if (event_queue_empty(&events))
        continue;

      struct event e = *event_get_first(&events);
      type = e.type;
      data = e.data;
      event_pop(&events);
    }

    if (type == EV_FILE_TAGS)
    {
      struct tag_ev_response *ev = (struct tag_ev_response *)data;
      int n;

      /* Count this response toward our pending-tags total before handing
       * the event off to server_event(), which will call ev_file_tags()
       * and update_item_tags() for us (and then free the data).  We must
       * not touch data->tags after server_event() returns. */
      if ((n = plist_find_fname(plist, ev->file)) != -1)
      {
        if (ev->tags->filled & tags_sel)
        {
          files--;
        }
      }

      /* Delegate all update + free work to the normal event handler. */
      if (!no_iface)
        server_event(type, data);
      else
        free_event_data(type, data);
    }
    else if (no_iface)
    {
      abort(); /* can't handle other events without the interface */
    }
    else
    {
      server_event(type, data);
    }
  }

  iface_set_status("");
}

/* Load the directory content into dir_plist and switch the menu to it.
 * If dir is NULL, go to the cwd.  If reload is not zero, we are reloading
 * the current directory, so use iface_update_dir_content().
 * Return 1 on success, 0 on error. */
static int go_to_dir(const char *dir, const int reload)
{
  struct plist *old_dir_plist;
  char last_dir[PATH_MAX];
  const char *new_dir = dir ? dir : cwd;
  int going_up = 0;
  lists_t_strs *dirs, *playlists;

  iface_set_status("Reading directory...");

  if (dir && is_subdir(dir, cwd))
  {
    strcpy(last_dir, strrchr(cwd, '/') + 1);
    strcat(last_dir, "/");
    going_up = 1;
  }

  old_dir_plist = dir_plist;
  dir_plist = (struct plist *)xmalloc(sizeof(struct plist));
  plist_init(dir_plist);
  dirs = lists_strs_new(FILES_LIST_INIT_SIZE);
  playlists = lists_strs_new(FILES_LIST_INIT_SIZE);

  if (!read_directory(new_dir, dirs, playlists, dir_plist))
  {
    iface_set_status("");
    plist_free(dir_plist);
    lists_strs_free(dirs);
    lists_strs_free(playlists);
    free(dir_plist);
    dir_plist = old_dir_plist;
    return 0;
  }

  /* TODO: call engine_abort_tags_requests() here (what if we requested tags for the
   playlist?) */

  plist_free(old_dir_plist);
  free(old_dir_plist);

#ifdef HAVE_SYS_INOTIFY_H
  if (!reload && inotify_wd >= 0)
  {
    int res = inotify_rm_watch(inotify_fd, inotify_wd);
    debug("removing watch: %s", (res == -1) ? xstrerror(errno) : "OK");
  }
#endif

  if (dir) /* if dir is NULL, we went to cwd */
  {
    strcpy(cwd, dir);
  }

  switch_titles_file(dir_plist);

  plist_sort_fname(dir_plist);
  lists_strs_sort(dirs, sort_dirs_func);
  lists_strs_sort(playlists, sort_strcmp_func);

  ask_for_tags(dir_plist, get_tags_setting());

  if (reload)
  {
    iface_update_dir_content(IFACE_MENU_DIR, dir_plist, dirs, playlists);
  }
  else
  {
    iface_set_dir_content(IFACE_MENU_DIR, dir_plist, dirs, playlists);
#ifdef HAVE_SYS_INOTIFY_H
    if (inotify_fd >= 0)
    {
      inotify_wd = inotify_add_watch(inotify_fd, new_dir,
                                     IN_MODIFY | IN_CREATE | IN_DELETE);
      debug("adding watch for dir %s: %s", new_dir,
            (inotify_wd == -1) ? xstrerror(errno) : "OK");
    }
#endif
  }
  lists_strs_free(dirs);
  lists_strs_free(playlists);
  if (going_up)
  {
    iface_set_curr_item_title(last_dir);
  }

  iface_set_title(IFACE_MENU_DIR, cwd);
  iface_update_queue_positions(queue, NULL, dir_plist, NULL);

  if (iface_in_plist_menu())
  {
    iface_switch_to_dir();
  }

  return 1;
}

static void enter_first_dir();

/* Switch between the directory view and the playlist. */
static void toggle_menu()
{
  if (iface_in_plist_menu())
  {
    if (!cwd[0])
    {
      /* we were at the playlist from the startup */
      enter_first_dir();
    }
    else
    {
      iface_switch_to_dir();
    }
  }
  else if ((plist_count(playlist)))
  {
    iface_switch_to_plist();
  }
  else
  {
    error("The playlist is empty.");
  }
}

/* Load the playlist file and switch the menu to it. Return 1 on success. */
static int go_to_playlist(const char *file, bool default_playlist)
{
  if (plist_count(playlist))
  {
    error("Please clear the playlist, because "
          "I'm not sure you want to do this.");
    return 0;
  }

  plist_clear(playlist);

  iface_set_status("Loading playlist...");
  if (plist_load(playlist, file, cwd))
  {
    if (!default_playlist)
    {
      toggle_menu();
    }
    iface_set_dir_content(IFACE_MENU_PLIST, playlist, NULL, NULL);
    iface_update_queue_positions(queue, playlist, NULL, NULL);

    interface_message("Playlist loaded.");
  }
  else
  {
    interface_message("The playlist is empty");
    iface_set_status("");
    return 0;
  }

  return 1;
}

/* Enter to the initial directory or toggle to the initial playlist (only
 * if the function has not been called yet). */
static void enter_first_dir()
{
  static int first_run = 1;

  if (options_get_bool("StartInMusicDir"))
  {
    char *music_dir;

    if ((music_dir = options_get_str("MusicDir")))
    {
      set_cwd(music_dir);
      if (first_run && file_type(music_dir) == F_PLAYLIST &&
          plist_count(playlist) == 0 && go_to_playlist(music_dir, false))
      {
        cwd[0] = 0;
        first_run = 0;
      }
      else if (file_type(cwd) == F_DIR && go_to_dir(NULL, 0))
      {
        first_run = 0;
        return;
      }
    }
    else
    {
      error("MusicDir is not set");
    }
  }

  if (!(read_last_dir() && go_to_dir(NULL, 0)))
  {
    set_start_dir();
    if (!go_to_dir(NULL, 0))
    {
      interface_fatal("Can't enter any directory!");
    }
  }

  first_run = 0;
}

static void use_engine_queue()
{
  iface_set_status("Getting the queue...");
  debug("Getting the queue...");

  recv_engine_queue(queue);
  iface_set_files_in_queue(plist_count(queue));
  iface_update_queue_positions(queue, playlist, dir_plist, NULL);
  iface_set_status("");
}

/* Process a single directory argument. */
static void process_dir_arg(const char *dir)
{
  set_cwd(dir);
  if (!go_to_dir(NULL, 0))
  {
    enter_first_dir();
  }
}

/* Process a single playlist argument. */
static void process_plist_arg(const char *file)
{
  char path[PATH_MAX + 1]; /* the playlist's directory */
  char *slash;

  if (file[0] == '/')
  {
    strcpy(path, "/");
  }
  else if (!getcwd(path, sizeof(path)))
  {
    interface_fatal("Can't get CWD: %s", xstrerror(errno));
  }

  resolve_path(path, sizeof(path), file);
  slash = strrchr(path, '/');
  assert(slash != NULL);
  *slash = 0;

  iface_set_status("Loading playlist...");
  plist_load(playlist, file, path);
  iface_set_status("");
}

/* Process a list of arguments. */
static void process_multiple_args(lists_t_strs *args)
{
  int size, ix;
  const char *arg;
  char this_cwd[PATH_MAX];

  if (!getcwd(this_cwd, sizeof(cwd)))
  {
    interface_fatal("Can't get CWD: %s", xstrerror(errno));
  }

  size = lists_strs_size(args);

  for (ix = 0; ix < size; ix += 1)
  {
    int dir;
    char path[2 * PATH_MAX];

    arg = lists_strs_at(args, ix);
    dir = is_dir(arg);

    if (arg[0] == '/')
    {
      strcpy(path, "/");
    }
    else
    {
      strcpy(path, this_cwd);
    }
    resolve_path(path, sizeof(path), arg);

    if (dir == 1)
    {
      read_directory_recurr(path, playlist);
    }
    else if (!dir && is_sound_file(path))
    {
      if (plist_find_fname(playlist, path) == -1)
      {
        plist_add(playlist, path);
      }
    }
    else if (is_plist_file(path))
    {
      char *plist_dir, *slash;

      /* Here we've chosen to resolve the playlist's relative paths
       * with respect to the directory of the playlist (or of the
       * symlink being used to reference it).  If some other base is
       * desired, then we probably need to introduce a new option. */

      plist_dir = xstrdup(path);
      slash = strrchr(plist_dir, '/');
      assert(slash != NULL);
      *slash = 0;

      plist_load(playlist, path, plist_dir);

      free(plist_dir);
    }
  }
}

/* Process file names passed as arguments. */
static void process_args(lists_t_strs *args)
{
  int size;
  const char *arg;

  size = lists_strs_size(args);
  arg = lists_strs_at(args, 0);

  if (size == 1 && is_dir(arg) == 1)
  {
    process_dir_arg(arg);
    return;
  }

  if (size == 1 && is_plist_file(arg))
  {
    process_plist_arg(arg);
  }
  else
  {
    process_multiple_args(args);
  }

  if (plist_count(playlist))
  {
    switch_titles_file(playlist);
    ask_for_tags(playlist, get_tags_setting());
    iface_set_dir_content(IFACE_MENU_PLIST, playlist, NULL, NULL);
    iface_update_queue_positions(queue, playlist, NULL, NULL);
    iface_switch_to_plist();
  }
  else
  {
    enter_first_dir();
  }
}

/* Load the playlist from .moc directory. */
static void load_playlist()
{
  char *plist_file = create_file_name(PLAYLIST_FILE);

  if (file_type(plist_file) == F_PLAYLIST)
  {
    go_to_playlist(plist_file, true);
  }
}

#ifdef SIGWINCH
/* Handle resizing xterm. */
static void do_resize()
{
  iface_resize();
  logit("resize");
  want_resize = 0;
}
#endif

/* Strip the last directory from the path. Returned memory is mallod()ed. */
static char *dir_up(const char *path)
{
  char *slash;
  char *dir;

  assert(path != NULL);

  dir = xstrdup(path);
  slash = strrchr(dir, '/');
  assert(slash != NULL);
  if (slash == dir)
  {
    *(slash + 1) = 0;
  }
  else
  {
    *slash = 0;
  }

  return dir;
}

static void go_dir_up()
{
  char *dir;

  dir = dir_up(cwd);
  go_to_dir(dir, 0);
  free(dir);
}

/* Send the playlist to the engine. If clear != 0, clear the engine's playlist
 * before sending. */
static void send_playlist(struct plist *plist, const int clear)
{
  int i;

  if (clear)
  {
    audio_plist_clear();
  }

  for (i = 0; i < plist->num; i++)
  {
    if (!plist_deleted(plist, i))
    {
      audio_plist_add(plist->items[i].file);
    }
  }
}

/* Send the playlist to the engine if necessary and request playing this
 * item. */
static void play_it(const char *file)
{
  struct plist *curr_plist;

  assert(file != NULL);

  if (iface_in_dir_menu())
  {
    curr_plist = dir_plist;
  }
  else
  {
    curr_plist = playlist;
  }

  if (options_get_bool("ForceShufflePlaylistOnly"))
  {
    engine_set_option("Shuffle", !iface_in_dir_menu());
    sync_bool_option("Shuffle");
  }

  if (curr_plist == dir_plist || curr_plist != engine_plist || playlist_dirty)
  {
    logit("Sending playlist to engine");
    send_playlist(curr_plist, 1);
    engine_plist = curr_plist;
    playlist_dirty = false;
  }
  audio_play(file);
}

/* Action when the user selected a file. */
static void go_file()
{
  enum file_type type = iface_curritem_get_type();
  char *file = iface_get_curr_file();

  if (!file)
  {
    return;
  }

  if (type == F_SOUND)
  {
    play_it(file);
  }
  else if (type == F_DIR && iface_in_dir_menu())
  {
    if (!strcmp(file, ".."))
    {
      go_dir_up();
    }
    else
    {
      go_to_dir(file, 0);
    }
  }
  else if (type == F_PLAYLIST)
  {
    go_to_playlist(file, false);
  }

  free(file);
}

/* pause/unpause */
static void switch_pause()
{
  switch (curr_file.state)
  {
    case STATE_PLAY:
      audio_pause();
      break;
    case STATE_PAUSE:
      audio_unpause();
      break;
    default:
      logit("User pressed pause when not playing.");
  }
}

static void set_mixer(int val)
{
  val = CLAMP(0, val, 100);
  audio_set_mixer(val);
}

static void adjust_mixer(const int diff)
{
  set_mixer(get_mixer_value() + diff);
}

/* Recursively add the content of a directory to the playlist. */
static void add_dir_plist()
{
  struct plist plist;
  char *file;
  enum file_type type;

  if (iface_in_plist_menu())
  {
    error("Can't add to the playlist a file from the playlist.");
    return;
  }

  file = iface_get_curr_file();

  if (!file)
  {
    return;
  }

  type = iface_curritem_get_type();
  if (type != F_DIR && type != F_PLAYLIST)
  {
    error("This is not a directory or a playlist.");
    free(file);
    return;
  }

  if (!strcmp(file, ".."))
  {
    free(file);
    file = xstrdup(cwd);
  }

  iface_set_status("Reading directories...");
  plist_init(&plist);

  if (type == F_DIR)
  {
    read_directory_recurr(file, &plist);
    plist_sort_fname(&plist);
  }
  else
  {
    plist_load(&plist, file, cwd);
  }

  plist_remove_common_items(&plist, playlist);

  /* Add the new files to the engine's playlist if the engine has our
   * playlist. */
  playlist_dirty = true;

  {
    int i;

    switch_titles_file(&plist);
    ask_for_tags(&plist, get_tags_setting());

    for (i = 0; i < plist.num; i++)
    {
      if (!plist_deleted(&plist, i))
      {
        iface_add_to_plist(&plist, i);
      }
    }
    plist_cat(playlist, &plist);
  }

  plist_free(&plist);
  free(file);
}

/* Remove a file from the in-memory playlist and from the engine's playlist
 * if the engine currently has our list.
 * Assumed to be in the playlist menu.
 */
static void remove_file_from_playlist(const char *file)
{
  int n;

  assert(file != NULL);
  assert(plist_count(playlist) > 0);

  n = plist_find_fname(playlist, file);
  assert(n != -1);

  plist_delete(playlist, n);
  iface_del_plist_item(file);

  if (plist_count(playlist) == 0)
  {
    clear_playlist();
  }

  /* Delete this item from the engine's playlist if it currently has ours. */
  if (engine_plist == playlist)
    audio_plist_delete(file);
  else
    playlist_dirty = true;
}

/* Remove all dead entries (point to non-existent or unreadable). */
static void remove_dead_entries_plist()
{
  const char *file = NULL;
  int i;

  if (!iface_in_plist_menu())
  {
    error("Can't prune when not in the playlist.");
    return;
  }

  for (i = 0, file = plist_get_next_dead_entry(playlist, &i); file != NULL;
       file = plist_get_next_dead_entry(playlist, &i))
  {
    remove_file_from_playlist(file);
  }
}

/* Add the currently selected file to the playlist. */
static void add_file_plist()
{
  char *file;

  if (iface_in_plist_menu())
  {
    error("Can't add to the playlist a file from the playlist.");
    return;
  }

  if (iface_curritem_get_type() == F_DIR)
  {
    add_dir_plist();
    return;
  }

  file = iface_get_curr_file();

  if (!file)
  {
    return;
  }

  if (iface_curritem_get_type() != F_SOUND)
  {
    error("You can only add a file using this command.");
    free(file);
    return;
  }

  if (plist_find_fname(playlist, file) == -1)
  {
    int added;
    struct plist_item *item =
        &dir_plist->items[plist_find_fname(dir_plist, file)];

    added = plist_add_from_item(playlist, item);
    iface_add_to_plist(playlist, added);

    /* Add to the engine's playlist if it currently has ours. */
    if (engine_plist == playlist)
      audio_plist_add(file);
    else
      playlist_dirty = true;
  }
  else
  {
    error("The file is already on the playlist.");
  }

  iface_menu_key(KEY_CMD_MENU_DOWN);

  free(file);
}

static void queue_toggle_file()
{
  char *file;

  file = iface_get_curr_file();

  if (!file)
  {
    return;
  }

  if (iface_curritem_get_type() != F_SOUND)
  {
    error("You can only add a file using this command.");
    free(file);
    return;
  }

  /* Check if the file is already in the queue; if it isn't, add it,
   * otherwise, remove it. */

  if (plist_find_fname(queue, file) == -1)
  {
    /* Add item to the engine's queue. */
    engine_queue_add(file);
    logit("Added to queue: %s", file);
  }
  else
  {
    /* Delete this item from the engine's queue. */
    engine_queue_del(file);
    logit("Removed from queue: %s", file);
  }

  iface_menu_key(KEY_CMD_MENU_DOWN);

  free(file);
}

static void toggle_option(const char *name)
{
  engine_set_option(name, !options_get_bool(name));
  sync_bool_option(name);
}

static void toggle_show_time()
{
  if (!strcasecmp(options_get_symb("ShowTime"), "yes"))
  {
    options_set_symb("ShowTime", "IfAvailable");
    iface_set_status("ShowTime: IfAvailable");
  }
  else if (!strcasecmp(options_get_symb("ShowTime"), "no"))
  {
    options_set_symb("ShowTime", "yes");
    iface_update_show_time();
    ask_for_tags(dir_plist, TAGS_TIME);
    ask_for_tags(playlist, TAGS_TIME);
    iface_set_status("ShowTime: yes");
  }
  else
  { /* IfAvailable */
    options_set_symb("ShowTime", "no");
    iface_update_show_time();
    iface_set_status("ShowTime: no");
  }
}

static void toggle_show_format()
{
  bool show_format = !options_get_bool("ShowFormat");

  options_set_bool("ShowFormat", show_format);
  if (show_format)
  {
    iface_set_status("ShowFormat: yes");
  }
  else
  {
    iface_set_status("ShowFormat: no");
  }

  iface_update_show_format();
}

/* Reread the directory. */
static void reread_dir()
{
  while (go_to_dir(NULL, 1) == 0)
  {
    go_dir_up();
  }
}

/* Clear the playlist on user request. */
static void cmd_clear_playlist()
{
  clear_playlist();
}

static void cmd_clear_queue() { engine_queue_clear(); }

static void go_to_music_dir()
{
  const char *musicdir_optn;
  char music_dir[PATH_MAX] = "/";

  musicdir_optn = options_get_str("MusicDir");

  if (!musicdir_optn)
  {
    error("MusicDir not defined");
    return;
  }

  resolve_path(music_dir, sizeof(music_dir), musicdir_optn);

  switch (file_type(music_dir))
  {
    case F_DIR:
      go_to_dir(music_dir, 0);
      break;
    case F_PLAYLIST:
      go_to_playlist(music_dir, false);
      break;
    default:
      error("MusicDir is neither a directory nor a playlist!");
  }
}

/* Make a directory from the string resolving ~, './' and '..'.
 * Return the directory, the memory is malloc()ed.
 * Return NULL on error. */
static char *make_dir(const char *str)
{
  char *dir;
  int add_slash = 0;

  dir = (char *)xmalloc(sizeof(char) * (PATH_MAX));

  dir[0] = 0;

  /* If the string ends with a slash and is not just '/', add this
   * slash. */
  if (strlen(str) > 1 && str[strlen(str) - 1] == '/')
  {
    add_slash = 1;
  }

  if (str[0] == '~')
  {
    const char *home = get_home();
    if (strnlen(home, PATH_MAX) == PATH_MAX)
    {
      logit("Path too long!");
      return NULL;
    }
    strcpy(dir, home);

    if (!strcmp(str, "~"))
    {
      add_slash = 1;
    }

    str++;
  }
  else if (str[0] != '/')
  {
    strcpy(dir, cwd);
  }
  else
  {
    strcpy(dir, "/");
  }

  resolve_path(dir, PATH_MAX, str);

  if (add_slash && strlen(dir) < PATH_MAX)
  {
    strcat(dir, "/");
  }

  return dir;
}

static void entry_key_go_dir(const struct iface_key *k)
{
  if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\t')
  {
    char *dir;
    char *complete_dir;
    char buf[PATH_MAX];
    char *entry_text;

    entry_text = iface_entry_get_text();
    dir = make_dir(entry_text);
    free(entry_text);
    if (!dir)
    {
      return;
    }

    complete_dir = find_match_dir(dir);
    if (complete_dir)
    {
      free(dir);
      dir = complete_dir;
    }

    pathstrcpy(buf, dir);
    free(dir);

    iface_entry_set_text(buf);
  }
  else if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n')
  {
    char *entry_text = iface_entry_get_text();

    if (entry_text[0])
    {
      char *dir = make_dir(entry_text);

      iface_entry_history_add();

      if (dir)
      {
        /* strip trailing slash */
        if (dir[strlen(dir) - 1] == '/' && strcmp(dir, "/"))
        {
          dir[strlen(dir) - 1] = 0;
        }
        go_to_dir(dir, 0);
        free(dir);
      }
    }

    iface_entry_disable();
    free(entry_text);
  }
  else
  {
    iface_entry_handle_key(k);
  }
}


static void entry_key_search(const struct iface_key *k)
{
  if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n')
  {
    char *file = iface_get_curr_file();
    char *text = iface_entry_get_text();

    iface_entry_disable();

    if (text[0])
    {
      if (!strcmp(file, ".."))
      {
        free(file);
        file = dir_up(cwd);
      }

      if (file_type(file) == F_DIR)
      {
        go_to_dir(file, 0);
      }
      else if (file_type(file) == F_PLAYLIST)
      {
        go_to_playlist(file, false);
      }
      else
      {
        play_it(file);
      }
    }

    free(text);
    free(file);
  }
  else
  {
    iface_entry_handle_key(k);
  }
}

static void save_playlist(const char *file)
{
  iface_set_status("Saving the playlist...");
  if (options_get_bool("SavePlaylistTags"))
  {
    fill_tags(playlist, TAGS_COMMENTS | TAGS_TIME, 0);
    if (user_wants_interrupt())
    {
      iface_set_status("Reading tags aborted");
    }
  }

  if (plist_save(
          playlist, file,
          (options_get_bool("SavePlaylistTags") && !user_wants_interrupt())))
  {
    interface_message("Playlist saved");
  }
  iface_set_status("");
}

static void entry_key_plist_save(const struct iface_key *k)
{
  if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n')
  {
    char *text = iface_entry_get_text();

    iface_entry_disable();

    if (text[0])
    {
      char *ext = ext_pos(text);
      char *file;

      /* add extension if necessary */
      if (!ext || strcmp(ext, "m3u"))
      {
        char *tmp = (char *)xmalloc((strlen(text) + 5) * sizeof(char));

        sprintf(tmp, "%s.m3u", text);
        free(text);
        text = tmp;
      }

      file = make_dir(text);

      if (file_exists(file))
      {
        iface_make_entry(ENTRY_PLIST_OVERWRITE);
        iface_entry_set_file(file);
      }
      else
      {
        save_playlist(file);

        if (iface_in_dir_menu())
        {
          reread_dir();
        }
      }

      free(file);
    }

    free(text);
  }
  else
  {
    iface_entry_handle_key(k);
  }
}

static void entry_key_plist_overwrite(const struct iface_key *k)
{
  if (k->type == IFACE_KEY_CHAR && toupper(k->key.ucs) == 'Y')
  {
    char *file = iface_entry_get_file();

    assert(file != NULL);

    iface_entry_disable();

    save_playlist(file);
    if (iface_in_dir_menu())
    {
      reread_dir();
    }

    free(file);
  }
  else if (k->type == IFACE_KEY_CHAR && toupper(k->key.ucs) == 'N')
  {
    iface_entry_disable();
    iface_message("Not overwriting.");
  }
}

static void entry_key_user_query(const struct iface_key *k)
{
  if (k->type == IFACE_KEY_CHAR && k->key.ucs == '\n')
  {
    char *entry_text = iface_entry_get_text();
    iface_entry_disable();
    iface_user_reply(entry_text);
    free(entry_text);
  }
  else
  {
    iface_entry_handle_key(k);
  }
}

/* Handle keys while in an entry. */
static void entry_key(const struct iface_key *k)
{
  switch (iface_get_entry_type())
  {
    case ENTRY_GO_DIR:
      entry_key_go_dir(k);
      break;

    case ENTRY_SEARCH:
      entry_key_search(k);
      break;
    case ENTRY_PLIST_SAVE:
      entry_key_plist_save(k);
      break;
    case ENTRY_PLIST_OVERWRITE:
      entry_key_plist_overwrite(k);
      break;
    case ENTRY_USER_QUERY:
      entry_key_user_query(k);
      break;
    default:
      abort(); /* BUG */
  }
}

/* Update items in the menu for all items on the playlist. */
static void update_iface_menu(const enum iface_menu menu,
                              const struct plist *plist)
{
  int i;

  assert(plist != NULL);

  for (i = 0; i < plist->num; i++)
  {
    if (!plist_deleted(plist, i))
    {
      iface_update_item(menu, plist, i);
    }
  }
}

/* Switch ReadTags options and update the menu. */
static void switch_read_tags()
{
  if (options_get_bool("ReadTags"))
  {
    options_set_bool("ReadTags", false);
    switch_titles_file(dir_plist);
    switch_titles_file(playlist);
    iface_set_status("ReadTags: no");
  }
  else
  {
    options_set_bool("ReadTags", true);
    ask_for_tags(dir_plist, TAGS_COMMENTS);
    ask_for_tags(playlist, TAGS_COMMENTS);
    switch_titles_tags(dir_plist);
    switch_titles_tags(playlist);
    iface_set_status("ReadTags: yes");
  }

  update_iface_menu(IFACE_MENU_DIR, dir_plist);
  update_iface_menu(IFACE_MENU_PLIST, playlist);
}

static void seek(const int sec)
{
  audio_seek(sec);
}

static void jump_to(const int sec)
{
  engine_jump_to(sec);
}

static void seek_to_percent(int percent)
{
  engine_jump_to(-percent);
}

static void delete_item()
{
  char *file;

  if (!iface_in_plist_menu())
  {
    error("You can only delete an item from the playlist.");
    return;
  }

  assert(plist_count(playlist) > 0);

  file = iface_get_curr_file();

  remove_file_from_playlist(file);

  free(file);
}

/* Select the file that is currently played. */
static void go_to_playing_file()
{
  if (curr_file.file)
  {
    if (iface_in_plist_menu() &&
        plist_find_fname(playlist, curr_file.file) != -1)
    {
      iface_select_file(curr_file.file);
    }
    else if (iface_in_dir_menu() && file_type(curr_file.file) == F_SOUND)
    {
      if (plist_find_fname(dir_plist, curr_file.file) != -1)
      {
        iface_select_file(curr_file.file);
      }
      else
      {
        char *slash;
        char *file = xstrdup(curr_file.file);

        slash = strrchr(file, '/');
        assert(slash != NULL);
        *slash = 0;

        if (file[0])
        {
          go_to_dir(file, 0);
        }
        else
        {
          go_to_dir("/", 0);
        }

        iface_switch_to_dir();
        free(file);

        iface_select_file(curr_file.file);
      }
    }
  }
}

/* Return the time like the standard time() function, but rounded i.e. if we
 * have 11.8 seconds, return 12 seconds. */
static time_t rounded_time()
{
  struct timespec exact_time;
  time_t curr_time;

  if (get_realtime(&exact_time) == -1)
  {
    interface_fatal("get_realtime() failed: %s", xstrerror(errno));
  }

  curr_time = exact_time.tv_sec;
  if (exact_time.tv_nsec > 500000000L)
  {
    curr_time += 1;
  }

  return curr_time;
}

/* Handle silent seek key. */
static void seek_silent(const int sec)
{
  if (curr_file.state == STATE_PLAY && curr_file.file)
  {
    if (silent_seek_pos == -1)
    {
      silent_seek_pos = curr_file.curr_time + sec;
    }
    else
    {
      silent_seek_pos += sec;
    }

    silent_seek_pos = CLAMP(0, silent_seek_pos, curr_file.total_time);

    silent_seek_key_last = rounded_time();
    iface_set_curr_time(silent_seek_pos);
  }
}

/* Move the current playlist item (direction: 1 - up, -1 - down). */
static void move_item(const int direction)
{
  char *file;
  int second;
  char *second_file;

  if (!iface_in_plist_menu())
  {
    error("You can move only playlist items.");
    return;
  }

  if (!(file = iface_get_curr_file()))
  {
    return;
  }

  second = plist_find_fname(playlist, file);
  assert(second != -1);

  if (direction == -1)
  {
    second = plist_next(playlist, second);
  }
  else if (direction == 1)
  {
    second = plist_prev(playlist, second);
  }
  else
  {
    abort(); /* BUG */
  }

  if (second == -1)
  {
    free(file);
    return;
  }

  second_file = plist_get_file(playlist, second);

  swap_playlist_items(file, second_file);

  /* Update the engine's playlist if it currently has ours. */
  if (engine_plist == playlist)
    audio_plist_move(file, second_file);
  /* playlist_dirty already set by swap_playlist_items */

  free(second_file);
  free(file);
}

/* Handle releasing silent seek key. */
static void do_silent_seek()
{
  time_t curr_time = time(NULL);

  if (silent_seek_pos != -1 && silent_seek_key_last < curr_time)
  {
    seek(silent_seek_pos - curr_file.curr_time - 1);
    silent_seek_pos = -1;
    iface_set_curr_time(curr_file.curr_time);
  }
}

/* Handle the 'next' command. */
static void cmd_next()
{
  if (curr_file.state != STATE_STOP)
  {
    audio_next();
  }
  else if (plist_count(playlist))
  {
    if (engine_plist != playlist || playlist_dirty)
    {
      send_playlist(playlist, 1);
      engine_plist = playlist;
      playlist_dirty = false;
    }

    audio_play("");
  }
}

/* Add themes found in the directory to the list of theme files. */
static void add_themes_to_list(lists_t_strs *themes, const char *themes_dir)
{
  DIR *dir;
  struct dirent *entry;

  assert(themes);
  assert(themes_dir);

  if (!(dir = opendir(themes_dir)))
  {
    char *err = xstrerror(errno);
    logit("Can't open themes directory %s: %s", themes_dir, err);
    free(err);
    return;
  }

  while ((entry = readdir(dir)))
  {
    int rc;
    char file[PATH_MAX];

    if (entry->d_name[0] == '.')
    {
      continue;
    }

    /* Filter out backup files (*~). */
    if (entry->d_name[strlen(entry->d_name) - 1] == '~')
    {
      continue;
    }

    rc = snprintf(file, sizeof(file), "%s/%s", themes_dir, entry->d_name);
    if (rc >= ssizeof(file))
    {
      continue;
    }

    lists_strs_append(themes, file);
  }

  closedir(dir);
}

/* Compare two pathnames based on filename. */
static int themes_cmp(const void *a, const void *b)
{
  int result;
  char *sa = *(char **)a;
  char *sb = *(char **)b;

  result = strcoll(strrchr(sa, '/') + 1, strrchr(sb, '/') + 1);
  if (result == 0)
  {
    result = strcoll(sa, sb);
  }

  return result;
}

/* Add themes found in the directories to the theme selection menu.
 * Return the number of items added. */
static int add_themes_to_menu(const char *user_themes,
                              const char *system_themes)
{
  int ix;
  lists_t_strs *themes;

  assert(user_themes);
  assert(system_themes);

  themes = lists_strs_new(16);
  add_themes_to_list(themes, user_themes);
  add_themes_to_list(themes, system_themes);
  lists_strs_sort(themes, themes_cmp);

  for (ix = 0; ix < lists_strs_size(themes); ix += 1)
  {
    char *file;

    file = lists_strs_at(themes, ix);
    iface_add_file(file, strrchr(file, '/') + 1, F_THEME);
  }

  lists_strs_free(themes);

  return ix;
}

static void make_theme_menu()
{
  iface_switch_to_theme_menu();

  if (add_themes_to_menu(create_file_name("themes"), SYSTEM_THEMES_DIR) == 0)
  {
    if (!cwd[0])
    {
      enter_first_dir(); /* we were at the playlist from the startup */
    }
    else
    {
      iface_switch_to_dir();
    }

    error("No themes found.");
  }
  else
  {
    iface_update_theme_selection(get_current_theme());
  }
  iface_refresh();
}

/* Use theme from the currently selected file. */
static void use_theme()
{
  char *file = iface_get_curr_file();

  assert(iface_curritem_get_type() == F_THEME);

  if (!file)
  {
    return;
  }

  themes_switch_theme(file);
  iface_update_attrs();
  iface_refresh();

  free(file);
}

/* Handle keys while in the theme menu. */
static void theme_menu_key(const struct iface_key *k)
{
  if (!iface_key_is_resize(k))
  {
    enum key_cmd cmd = get_key_cmd(CON_MENU, k);

    switch (cmd)
    {
      case KEY_CMD_GO:
        use_theme();
        break;
      case KEY_CMD_MENU_DOWN:
      case KEY_CMD_MENU_UP:
      case KEY_CMD_MENU_NPAGE:
      case KEY_CMD_MENU_PPAGE:
      case KEY_CMD_MENU_FIRST:
      case KEY_CMD_MENU_LAST:
        iface_menu_key(cmd);
        break;
      default:
        iface_switch_to_dir();
        logit("Bad key");
    }
  }
}

/* Make sure that we have tags and a title for this file which is in a menu. */
static void make_sure_tags_exist(const char *file)
{
  struct plist *plist;
  int item_num;

  if (file_type(file) != F_SOUND)
  {
    return;
  }

  if ((item_num = plist_find_fname(dir_plist, file)) != -1)
  {
    plist = dir_plist;
  }
  else if ((item_num = plist_find_fname(playlist, file)) != -1)
  {
    plist = playlist;
  }
  else
  {
    return;
  }

  if (!plist->items[item_num].tags ||
      plist->items[item_num].tags->filled != (TAGS_COMMENTS | TAGS_TIME))
  {
    int got_it = 0;

    send_tags_request(file, TAGS_COMMENTS | TAGS_TIME);

    while (!got_it)
    {
      struct event *e;

      /* Drain any already-queued events first, then block for more. */
      if (event_queue_empty(&events))
        wait_and_drain_engine_events();

      while ((e = event_get_first(&events)) && !got_it)
      {
        int type   = e->type;
        void *data = e->data;
        event_pop(&events);

        if (type == EV_FILE_TAGS)
        {
          struct tag_ev_response *ev = (struct tag_ev_response *)data;
          if (!strcmp(ev->file, file))
            got_it = 1;
        }

        server_event(type, data);
      }
    }
  }
}

/* Request tags from the engine for a file in the playlist or the directory
 * menu, wait until they arrive and return them (malloc()ed). */
static struct file_tags *get_tags(const char *file)
{
  struct plist *plist;
  int item_num;

  make_sure_tags_exist(file);

  if ((item_num = plist_find_fname(dir_plist, file)) != -1)
  {
    plist = dir_plist;
  }
  else if ((item_num = plist_find_fname(playlist, file)) != -1)
  {
    plist = playlist;
  }
  else
  {
    return tags_new();
  }

  if (file_type(file) == F_SOUND)
  {
    return tags_dup(plist->items[item_num].tags);
  }

  return tags_new();
}

/* Get the title of a file (malloc()ed) that is present in a menu. */
static char *get_title(const char *file)
{
  struct plist *plist;
  int item_num;

  make_sure_tags_exist(file);

  if ((item_num = plist_find_fname(dir_plist, file)) != -1)
  {
    plist = dir_plist;
  }
  else if ((item_num = plist_find_fname(playlist, file)) != -1)
  {
    plist = playlist;
  }
  else
  {
    return NULL;
  }

  return xstrdup(plist->items[item_num].title_tags
                     ? plist->items[item_num].title_tags
                     : plist->items[item_num].title_file);
}


static void go_to_fast_dir(const int num)
{
  char option_name[20];

  assert(RANGE(1, num, 10));

  sprintf(option_name, "FastDir%d", num);

  if (options_get_str(option_name))
  {
    char dir[PATH_MAX] = "/";

    resolve_path(dir, sizeof(dir), options_get_str(option_name));
    go_to_dir(dir, 0);
  }
  else
  {
    error("%s is not defined", option_name);
  }
}

static void toggle_playlist_full_paths(void)
{
  bool new_val = !options_get_bool("PlaylistFullPaths");

  options_set_bool("PlaylistFullPaths", new_val);

  if (new_val)
  {
    iface_set_status("PlaylistFullPaths: on");
  }
  else
  {
    iface_set_status("PlaylistFullPaths: off");
  }

  update_iface_menu(IFACE_MENU_PLIST, playlist);
}

/* Handle key. */
static void menu_key(const struct iface_key *k)
{
  if (iface_in_help())
  {
    iface_handle_help_key(k);
  }
  else if (iface_in_entry())
  {
    entry_key(k);
  }
  else if (iface_in_theme_menu())
  {
    theme_menu_key(k);
  }
  else if (!iface_key_is_resize(k))
  {
    enum key_cmd cmd = get_key_cmd(CON_MENU, k);

    switch (cmd)
    {
      case KEY_CMD_QUIT:
        want_quit = QUIT_APP;
        break;
      case KEY_CMD_GO:
        go_file();
        break;
      case KEY_CMD_MENU_DOWN:
      case KEY_CMD_MENU_UP:
      case KEY_CMD_MENU_NPAGE:
      case KEY_CMD_MENU_PPAGE:
      case KEY_CMD_MENU_FIRST:
      case KEY_CMD_MENU_LAST:
        iface_menu_key(cmd);
        last_menu_move_time = time(NULL);
        break;
      case KEY_CMD_STOP:
        audio_stop();
        break;
      case KEY_CMD_NEXT:
        cmd_next();
        break;
      case KEY_CMD_PREVIOUS:
        audio_prev();
        break;
      case KEY_CMD_PAUSE:
        switch_pause();
        break;
      case KEY_CMD_TOGGLE_READ_TAGS:
        switch_read_tags();
        break;
      case KEY_CMD_TOGGLE_SHUFFLE:
        toggle_option("Shuffle");
        break;
      case KEY_CMD_TOGGLE_REPEAT:
        toggle_option("Repeat");
        break;
      case KEY_CMD_TOGGLE_AUTO_NEXT:
        toggle_option("AutoNext");
        break;
      case KEY_CMD_TOGGLE_MENU:
        toggle_menu();
        break;
      case KEY_CMD_TOGGLE_PLAYLIST_FULL_PATHS:
        toggle_playlist_full_paths();
        break;
      case KEY_CMD_PLIST_ADD_FILE:
        add_file_plist();
        break;
      case KEY_CMD_PLIST_CLEAR:
        cmd_clear_playlist();
        break;
      case KEY_CMD_PLIST_ADD_DIR:
        add_dir_plist();
        break;
      case KEY_CMD_PLIST_REMOVE_DEAD_ENTRIES:
        remove_dead_entries_plist();
        break;
      case KEY_CMD_MIXER_DEC_1:
        adjust_mixer(-1);
        break;
      case KEY_CMD_MIXER_DEC_5:
        adjust_mixer(-5);
        break;
      case KEY_CMD_MIXER_INC_5:
        adjust_mixer(+5);
        break;
      case KEY_CMD_MIXER_INC_1:
        adjust_mixer(+1);
        break;
      case KEY_CMD_SEEK_BACKWARD:
        seek(-options_get_int("SeekTime"));
        break;
      case KEY_CMD_SEEK_FORWARD:
        seek(options_get_int("SeekTime"));
        break;

      case KEY_CMD_SEEK_0:
        seek_to_percent(0 * 10);
        break;
      case KEY_CMD_SEEK_1:
        seek_to_percent(1 * 10);
        break;
      case KEY_CMD_SEEK_2:
        seek_to_percent(2 * 10);
        break;
      case KEY_CMD_SEEK_3:
        seek_to_percent(3 * 10);
        break;
      case KEY_CMD_SEEK_4:
        seek_to_percent(4 * 10);
        break;
      case KEY_CMD_SEEK_5:
        seek_to_percent(5 * 10);
        break;
      case KEY_CMD_SEEK_6:
        seek_to_percent(6 * 10);
        break;
      case KEY_CMD_SEEK_7:
        seek_to_percent(7 * 10);
        break;
      case KEY_CMD_SEEK_8:
        seek_to_percent(8 * 10);
        break;
      case KEY_CMD_SEEK_9:
        seek_to_percent(9 * 10);
        break;

      case KEY_CMD_HELP:
        iface_switch_to_help();
        break;
      case KEY_CMD_HIDE_MESSAGE:
        iface_disable_message();
        break;
      case KEY_CMD_REFRESH:
        iface_refresh();
        break;
      case KEY_CMD_RELOAD:
        if (iface_in_dir_menu())
        {
          reread_dir();
        }
        break;
      case KEY_CMD_TOGGLE_SHOW_HIDDEN_FILES:
        options_set_bool("ShowHiddenFiles",
                         !options_get_bool("ShowHiddenFiles"));
        if (iface_in_dir_menu())
        {
          reread_dir();
        }
        break;
      case KEY_CMD_GO_MUSIC_DIR:
        go_to_music_dir();
        break;
      case KEY_CMD_PLIST_DEL:
        delete_item();
        break;
      case KEY_CMD_MENU_SEARCH:
        iface_make_entry(ENTRY_SEARCH);
        break;
      case KEY_CMD_PLIST_SAVE:
        if (plist_count(playlist))
        {
          iface_make_entry(ENTRY_PLIST_SAVE);
        }
        else
        {
          error("The playlist is empty.");
        }
        break;
      case KEY_CMD_TOGGLE_SHOW_TIME:
        toggle_show_time();
        break;
      case KEY_CMD_TOGGLE_SHOW_FORMAT:
        toggle_show_format();
        break;
      case KEY_CMD_GO_TO_PLAYING_FILE:
        go_to_playing_file();
        break;
      case KEY_CMD_GO_DIR:
        iface_make_entry(ENTRY_GO_DIR);
        break;

      case KEY_CMD_GO_DIR_UP:
        go_dir_up();
        break;
      case KEY_CMD_WRONG:
        error("Bad command");
        break;
      case KEY_CMD_SEEK_FORWARD_5:
        seek_silent(options_get_int("SilentSeekTime"));
        break;
      case KEY_CMD_SEEK_BACKWARD_5:
        seek_silent(-options_get_int("SilentSeekTime"));
        break;
      case KEY_CMD_VOLUME_0:
        set_mixer(0);
        break;
      case KEY_CMD_VOLUME_10:
        set_mixer(10);
        break;
      case KEY_CMD_VOLUME_20:
        set_mixer(20);
        break;
      case KEY_CMD_VOLUME_30:
        set_mixer(30);
        break;
      case KEY_CMD_VOLUME_40:
        set_mixer(40);
        break;
      case KEY_CMD_VOLUME_50:
        set_mixer(50);
        break;
      case KEY_CMD_VOLUME_60:
        set_mixer(60);
        break;
      case KEY_CMD_VOLUME_70:
        set_mixer(70);
        break;
      case KEY_CMD_VOLUME_80:
        set_mixer(80);
        break;
      case KEY_CMD_VOLUME_90:
        set_mixer(90);
        break;
      case KEY_CMD_VOLUME_100:
        set_mixer(100);
        break;
      case KEY_CMD_MARK_START:
        file_info_block_mark(&curr_file.block_start);
        break;
      case KEY_CMD_MARK_END:
        file_info_block_mark(&curr_file.block_end);
        break;
      case KEY_CMD_FAST_DIR_1:
        go_to_fast_dir(1);
        break;
      case KEY_CMD_FAST_DIR_2:
        go_to_fast_dir(2);
        break;
      case KEY_CMD_FAST_DIR_3:
        go_to_fast_dir(3);
        break;
      case KEY_CMD_FAST_DIR_4:
        go_to_fast_dir(4);
        break;
      case KEY_CMD_FAST_DIR_5:
        go_to_fast_dir(5);
        break;
      case KEY_CMD_FAST_DIR_6:
        go_to_fast_dir(6);
        break;
      case KEY_CMD_FAST_DIR_7:
        go_to_fast_dir(7);
        break;
      case KEY_CMD_FAST_DIR_8:
        go_to_fast_dir(8);
        break;
      case KEY_CMD_FAST_DIR_9:
        go_to_fast_dir(9);
        break;
      case KEY_CMD_FAST_DIR_10:
        go_to_fast_dir(10);
        break;
      case KEY_CMD_TOGGLE_MIXER:
        debug("Toggle mixer.");
        engine_toggle_mixer_channel();
        break;
      case KEY_CMD_TOGGLE_SOFTMIXER:
        debug("Toggle softmixer.");
        engine_toggle_softmixer();
        break;
      case KEY_CMD_TOGGLE_EQUALIZER:
        debug("Toggle equalizer.");
        engine_toggle_equalizer();
        break;
      case KEY_CMD_EQUALIZER_REFRESH:
        debug("Equalizer Refresh.");
        engine_equalizer_refresh();
        break;
      case KEY_CMD_EQUALIZER_PREV:
        debug("Equalizer Prev.");
        engine_equalizer_prev();
        break;
      case KEY_CMD_EQUALIZER_NEXT:
        debug("Equalizer Next.");
        engine_equalizer_next();
        break;
      case KEY_CMD_TOGGLE_MAKE_MONO:
        debug("Toggle Mono-Mixing.");
        engine_toggle_make_mono();
        break;
      case KEY_CMD_TOGGLE_LAYOUT:
        iface_toggle_layout();
        break;
      case KEY_CMD_TOGGLE_PERCENT:
        iface_toggle_percent();
        break;
      case KEY_CMD_PLIST_MOVE_UP:
        move_item(1);
        break;
      case KEY_CMD_PLIST_MOVE_DOWN:
        move_item(-1);
        break;

      case KEY_CMD_THEME_MENU:
        make_theme_menu();
        break;
      case KEY_CMD_QUEUE_TOGGLE_FILE:
        queue_toggle_file();
        break;
      case KEY_CMD_QUEUE_CLEAR:
        cmd_clear_queue();
        break;
      default:
        abort();
    }
  }
}

/* Drain all pending engine events into the local queue for dequeue_events()
 * to process.  Called when the engine event pipe fd fires in pselect(). */
static void get_and_handle_event()
{
  drain_engine_events();
}

/* Handle events from the queue. */
static void dequeue_events()
{
  struct event *e;

  debug("Dequeuing events...");

  while ((e = event_get_first(&events)))
  {
    server_event(e->type, e->data);
    event_pop(&events);
  }

  debug("done");
}

/* Action after CTRL-C was pressed. */
static void handle_interrupt()
{
  if (iface_in_entry())
  {
    iface_entry_disable();
  }
}

void init_interface(struct engine_event_queue *eq, lists_t_strs *args)
{
  logit("Starting MOC Interface");

  /* Set locale according to the environment variables. */
  if (!setlocale(LC_CTYPE, ""))
  {
    logit("Could not set locale!");
  }

  g_engine_eq = eq;

  file_info_reset(&curr_file);
  file_info_block_init(&curr_file);
  init_playlists();
  event_queue_init(&events);
  keys_init();
  windows_init();
  get_engine_options();
  update_mixer_name();

#ifdef HAVE_SYS_INOTIFY_H
  inotify_fd = inotify_init();
  debug("initialization of inotify: %s",
        (inotify_fd == -1) ? xstrerror(errno) : "OK");
#endif

  xsignal(SIGQUIT, sig_quit);
  xsignal(SIGTERM, sig_quit);
  xsignal(SIGHUP, sig_quit);
  xsignal(SIGINT, sig_interrupt);

#ifdef SIGWINCH
  xsignal(SIGWINCH, sig_winch);
#endif

  if (!lists_strs_empty(args))
  {
    process_args(args);

    if (plist_count(playlist) == 0)
    {
      load_playlist();
    }
  }
  else
  {
    load_playlist();
    enter_first_dir();
  }

  /* Ask the engine for the queue. */
  use_engine_queue();

  update_state();
}

void interface_loop()
{
  log_circular_start();

  while (want_quit == NO_QUIT)
  {
    fd_set fds;
    int ret;
    struct timespec timeout = {1, 0};

    FD_ZERO(&fds);
    FD_SET(engine_event_queue_fd(g_engine_eq), &fds);
    FD_SET(STDIN_FILENO, &fds);
#ifdef HAVE_SYS_INOTIFY_H
    if (inotify_fd >= 0)
    {
      FD_SET(inotify_fd, &fds);
    }

#endif

    dequeue_events();
#ifdef HAVE_SYS_INOTIFY_H
    ret = pselect(MAX(engine_event_queue_fd(g_engine_eq), inotify_fd) + 1,
                  &fds, NULL, NULL, &timeout, NULL);
#else
    ret = pselect(engine_event_queue_fd(g_engine_eq) + 1, &fds, NULL, NULL,
                  &timeout, NULL);
#endif
    if (ret == -1 && !want_quit && errno != EINTR)
    {
      interface_fatal("pselect() failed: %s", xstrerror(errno));
    }

    iface_tick();

    if (ret == 0)
    {
      do_silent_seek();
    }

#ifdef SIGWINCH
    if (want_resize)
    {
      do_resize();
    }
#endif

    if (ret > 0)
    {
      if (FD_ISSET(STDIN_FILENO, &fds))
      {
        struct iface_key k;

        iface_get_key(&k);

        clear_interrupt();
        menu_key(&k);
      }

      if (!want_quit)
      {
        if (FD_ISSET(engine_event_queue_fd(g_engine_eq), &fds))
        {
          get_and_handle_event();
        }
        do_silent_seek();
#ifdef HAVE_SYS_INOTIFY_H
        if (FD_ISSET(inotify_fd, &fds))
        {
          //					char buffer[1024];
          debug("inotify event, refreshing");
          {
            char dummy[4096];
            ret = read(inotify_fd, dummy, sizeof(dummy));
          }
          //					lseek(inotify_fd,0,SEEK_END);
          reread_dir();
        }
#endif
      }
    }
    else if (user_wants_interrupt())
    {
      handle_interrupt();
    }

    if (!want_quit)
    {
      update_mixer_name();
    }
  }

  log_circular_log();
  log_circular_stop();
}

/* Save the current directory path to a file. */
static void save_curr_dir()
{
  FILE *dir_file;

  if (!(dir_file = fopen(create_file_name("last_directory"), "w")))
  {
    error_errno("Can't save current directory", errno);
    return;
  }

  fprintf(dir_file, "%s", cwd);
  fclose(dir_file);
}

/* Save the playlist in .moc directory or remove the old playist if the
 * playlist is empty. */
static void save_playlist_in_moc()
{
  char *plist_file = create_file_name(PLAYLIST_FILE);

  if (plist_count(playlist) && options_get_bool("SavePlaylist"))
  {
    save_playlist(plist_file);
  }
  else
  {
    unlink(plist_file);
  }
}

void interface_end()
{
  save_curr_dir();
  save_playlist_in_moc();
  engine_quit();
  g_engine_eq = NULL;

#ifdef HAVE_SYS_INOTIFY_H
  if (inotify_wd >= 0)
  {
    inotify_rm_watch(inotify_fd, inotify_wd);
  }
  if (inotify_fd >= 0)
  {
    close(inotify_fd);
  }
#endif

  windows_end();
  keys_cleanup();

  plist_free(dir_plist);
  plist_free(playlist);
  plist_free(queue);
  free(dir_plist);
  free(playlist);
  free(queue);

  event_queue_free(&events);

  logit("Interface exited");

  log_close();
}

void interface_fatal(const char *format, ...)
{
  char *msg;
  va_list va;

  va_start(va, format);
  msg = format_msg_va(format, va);
  va_end(va);

  windows_end();
  fatal("%s", msg);
}

void interface_error(const char *msg) { iface_error(msg); }

// EOF
