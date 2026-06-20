// src/core/protocol.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "library/playlist.h"
#include <queue>


  /* -----------------------------------------------------------------------
   * In-process event queue — used both for the local (UI-side) queue and
   * as the storage inside struct engine_event_queue.
   * ----------------------------------------------------------------------- */

  struct Event
  {
    int type;           /* type of the event (one of EV_*) */
    void *data;         /* optional data associated with the event */
  };

  /* Used as data field in the event queue for EV_FILE_TAGS. */
  struct tag_ev_response
  {
    char *file;
    struct file_tags *tags;
  };

  /* Used as data field in the event queue for EV_QUEUE_MOVE. */
  struct move_ev_data
  {
    /* Two files that are to be exchanged. */
    char *from;
    char *to;
  };

  /* Data carried by EV_SRV_ERROR events. */
  struct srv_error_ev
  {
    char *file; /* path of the file that failed to open */
    char *msg;  /* human-readable error message         */
  };

/* Definition of events sent by the engine to the UI. */
#define EV_STATE      0x01  /* player state has changed */
#define EV_CTIME      0x02  /* current time of the song has changed */
#define EV_SRV_ERROR  0x04  /* an error occurred in the engine */
#define EV_BITRATE    0x07  /* the bitrate has changed */
#define EV_RATE       0x08  /* the rate has changed */
#define EV_CHANNELS   0x09  /* the number of channels has changed */
#define EV_OPTIONS    0x0c  /* an option has changed */
#define EV_TAGS       0x0e  /* tags for the current file have changed */
#define EV_STATUS_MSG 0x0f  /* string data: status message */
#define EV_MIXER_CHANGE 0x10 /* the mixer channel was changed */
#define EV_FILE_TAGS  0x11  /* tag_ev_response data: response for tags request */
#define EV_AVG_BITRATE 0x12 /* average bitrate has changed (new song) */
#define EV_AUDIO_START 0x13 /* playing of audio has started */
#define EV_AUDIO_STOP  0x14 /* playing of audio has stopped */

/* Queue-synchronisation events. */
#define EV_QUEUE_ADD   0x54 /* plist_item data: item added to queue */
#define EV_QUEUE_DEL   0x55 /* string data: file removed from queue */
#define EV_QUEUE_MOVE  0x56 /* move_ev_data: two queue items swapped */
#define EV_QUEUE_CLEAR 0x57 /* no data: queue was cleared */

/* Player state values. */
#define STATE_PLAY  0x01
#define STATE_STOP  0x02
#define STATE_PAUSE 0x03

  /* -----------------------------------------------------------------------
   * Event data API
   * ----------------------------------------------------------------------- */
  void free_event_data(const int type, void *data);

  struct tag_ev_response *tag_ev_data_dup(const struct tag_ev_response *d);
  void free_tag_ev_data(struct tag_ev_response *d);
  void free_move_ev_data(struct move_ev_data *m);
  struct move_ev_data *move_ev_data_dup(const struct move_ev_data *m);


#endif

// EOF
