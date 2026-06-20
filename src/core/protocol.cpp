// src/core/protocol.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2003 - 2005 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Reduced to in-process event-queue helpers only.  All socket I/O and
// binary packet serialisation has been removed as part of the
// single-process refactor (Step 10).

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstdlib>
#include <cstring>
#include <cassert>

#include "core/common.h"
#include "core/log.h"
#include "core/protocol.h"
#include "library/playlist.h"


/* Free data associated with an event. */
void free_event_data(const int type, void *data)
{
  if (!data)
    return;

  if (type == EV_QUEUE_ADD)
  {
    plist_free_item_fields(static_cast<struct plist_item *>(data));
    delete static_cast<struct plist_item *>(data);
  }
  else if (type == EV_FILE_TAGS)
  {
    free_tag_ev_data(static_cast<struct tag_ev_response *>(data));
  }
  else if (type == EV_SRV_ERROR)
  {
    struct srv_error_ev *e = static_cast<struct srv_error_ev *>(data);
    free(e->file);
    free(e->msg);
    delete e;
  }
  else if (type == EV_STATUS_MSG || type == EV_QUEUE_DEL)
  {
    free(data);
  }
  else if (type == EV_QUEUE_MOVE)
  {
    free_move_ev_data(static_cast<struct move_ev_data *>(data));
  }
  else
  {
    abort(); /* BUG: unknown event type with non-nullptr data */
  }
}

/* -----------------------------------------------------------------------
 * tag_ev_response helpers
 * ----------------------------------------------------------------------- */

void free_tag_ev_data(struct tag_ev_response *d)
{
  assert(d != nullptr);
  free(d->file);
  tags_free(d->tags);
  free(d);
}

struct tag_ev_response *tag_ev_data_dup(const struct tag_ev_response *d)
{
  assert(d != nullptr);
  assert(d->file != nullptr);

  struct tag_ev_response *n = new tag_ev_response;
  n->file = xstrdup(d->file);
  n->tags = d->tags ? tags_dup(d->tags) : nullptr;
  return n;
}

/* -----------------------------------------------------------------------
 * move_ev_data helpers
 * ----------------------------------------------------------------------- */

void free_move_ev_data(struct move_ev_data *m)
{
  assert(m != nullptr);
  free(m->from);
  free(m->to);
  delete m;
}

struct move_ev_data *move_ev_data_dup(const struct move_ev_data *m)
{
  assert(m != nullptr);
  assert(m->from != nullptr);
  assert(m->to != nullptr);

  struct move_ev_data *n = new move_ev_data;
  n->from = xstrdup(m->from);
  n->to   = xstrdup(m->to);
  return n;
}

// EOF
