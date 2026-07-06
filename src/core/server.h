// src/core/server.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef SERVER_H
#define SERVER_H

#include "core/protocol.h"
#include "library/playlist.h"
#include <memory>
#include <queue>

  /* -----------------------------------------------------------------------
   * Engine event queue — thread-safe push (engine threads) / drain (UI thread)
   * The UI thread watches engine_event_queue_fd() in its pselect() and calls
   * engine_event_queue_flush() when it fires to move pending events into its
   * own local std::queue for processing.
   * ----------------------------------------------------------------------- */
  struct engine_event_queue; /* opaque */

  struct engine_event_queue *engine_event_queue_new(void);
  void engine_event_queue_free(struct engine_event_queue *eq);

  /* File descriptor the UI thread should add to its select/pselect fd_set. */
  int  engine_event_queue_fd(const struct engine_event_queue *eq);

  /* Drain all pending events from the engine queue into dest and consume any
   * pending wakeup bytes from the pipe.  Non-blocking — call after pselect. */
  void engine_event_queue_flush(struct engine_event_queue *eq,
                                std::queue<Event> &dest);

  /* Blocking variant: block until at least one event is available, then drain.
   * Used by the UI in contexts that must wait for a specific event
   * (e.g. fill_tags). */
  void engine_event_queue_wait_flush(struct engine_event_queue *eq,
                                     std::queue<Event> &dest);

  /* -----------------------------------------------------------------------
   * Engine lifecycle
   * ----------------------------------------------------------------------- */

  /* Called from the engine thread once server_init() finishes. */
  void engine_signal_ready(void);

  /* Called from the main (UI) thread before init_interface(): blocks until
   * engine_signal_ready() has been called. */
  void engine_wait_ready(void);

  /* Called from interface_end() to ask the engine thread to quit. */
  void engine_quit(void);

  /* Start the engine: open audio, load tags cache, then signal ready. */
  void server_init(struct engine_event_queue *eq);

  /* The engine thread's main loop — waits for engine_quit(), then shuts
   * down audio and tags cache. */
  void server_loop(void);

  /* -----------------------------------------------------------------------
   * Engine→server callbacks (called by audio/player threads)
   * ----------------------------------------------------------------------- */
  void server_error(const char *file, int line, const char *function,
                    const char *msg);
  void engine_error(const char *file, const char *msg);
  void state_change(void);
  void set_info_rate(const int rate);
  void set_info_channels(const int channels);
  void set_info_bitrate(const int bitrate);
  void set_info_avg_bitrate(const int avg_bitrate);
  void tags_change(void);
  void ctime_change(void);
  void status_msg(const char *msg);
  void tags_response(const char *file, const struct file_tags *tags);
  void ev_audio_start(void);
  void ev_audio_stop(void);
  void server_queue_pop(const char *filename);

  /* -----------------------------------------------------------------------
   * Sound-info getters (UI→engine, replacing CMD_GET_BITRATE etc.)
   * ----------------------------------------------------------------------- */
  int engine_get_bitrate(void);
  int engine_get_avg_bitrate(void);
  int engine_get_rate(void);
  int engine_get_channels(void);

  /* -----------------------------------------------------------------------
   * Option sync (UI→engine)
   * ----------------------------------------------------------------------- */
  void engine_set_option(const char *name, bool val);

  /* -----------------------------------------------------------------------
   * Tags requests (UI→engine)
   * ----------------------------------------------------------------------- */
  void engine_request_file_tags(const char *file, int tags_sel);
  void engine_abort_tags_requests(const char *file);

  /* -----------------------------------------------------------------------
   * Queue operations (UI→engine) — each fires the appropriate EV_QUEUE_*
   * ----------------------------------------------------------------------- */
  void engine_queue_add(const char *file);
  void engine_queue_del(const char *file);
  void engine_queue_clear(void);

  /* Returns a freshly allocated copy of the server queue; caller must
   * delete it. */
  std::unique_ptr<struct plist> engine_get_queue(void);

  /* -----------------------------------------------------------------------
   * Compound commands with side effects (UI→engine)
   * ----------------------------------------------------------------------- */
  void engine_jump_to(int sec);   /* sec<0 means percentage (-10 = 10%) */
  void engine_toggle_mixer_channel(void);
  void engine_toggle_softmixer(void);
  void engine_toggle_equalizer(void);
  void engine_equalizer_refresh(void);
  void engine_equalizer_prev(void);
  void engine_equalizer_next(void);
  void engine_toggle_make_mono(void);


#endif

// EOF
