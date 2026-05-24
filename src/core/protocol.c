// src/core/protocol.c
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

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "core/common.h"
#include "core/log.h"
#include "core/protocol.h"
#include "library/playlist.h"

/* -----------------------------------------------------------------------
 * event_queue
 * ----------------------------------------------------------------------- */

void event_queue_init(struct event_queue *q)
{
  assert(q != NULL);
  q->head = NULL;
  q->tail = NULL;
}

/* Push an event onto the tail of the queue. */
void event_push(struct event_queue *q, const int event, void *data)
{
  assert(q != NULL);

  struct event *e = (struct event *)xmalloc(sizeof(struct event));
  e->next = NULL;
  e->type = event;
  e->data = data;

  if (!q->head)
  {
    q->head = e;
    q->tail = e;
  }
  else
  {
    assert(q->tail != NULL);
    assert(q->tail->next == NULL);
    q->tail->next = e;
    q->tail       = e;
  }
}

/* Remove the first event from the queue (does NOT free data). */
void event_pop(struct event_queue *q)
{
  struct event *e;

  assert(q != NULL);
  assert(q->head != NULL);
  assert(q->tail != NULL);

  e = q->head;
  q->head = e->next;
  if (q->tail == e)
    q->tail = NULL;
  free(e);
}

struct event *event_get_first(struct event_queue *q)
{
  assert(q != NULL);
  return q->head;
}

int event_queue_empty(const struct event_queue *q)
{
  assert(q != NULL);
  return q->head == NULL ? 1 : 0;
}

/* Free data associated with an event. */
void free_event_data(const int type, void *data)
{
  if (!data)
    return;

  if (type == EV_QUEUE_ADD)
  {
    plist_free_item_fields((struct plist_item *)data);
    free(data);
  }
  else if (type == EV_FILE_TAGS)
  {
    free_tag_ev_data((struct tag_ev_response *)data);
  }
  else if (type == EV_STATUS_MSG || type == EV_SRV_ERROR ||
           type == EV_QUEUE_DEL)
  {
    free(data);
  }
  else if (type == EV_QUEUE_MOVE)
  {
    free_move_ev_data((struct move_ev_data *)data);
  }
  else
  {
    abort(); /* BUG: unknown event type with non-NULL data */
  }
}

/* Free all events in the queue (including their data). */
void event_queue_free(struct event_queue *q)
{
  struct event *e;

  assert(q != NULL);

  while ((e = event_get_first(q)))
  {
    free_event_data(e->type, e->data);
    event_pop(q);
  }
}

/* -----------------------------------------------------------------------
 * tag_ev_response helpers
 * ----------------------------------------------------------------------- */

void free_tag_ev_data(struct tag_ev_response *d)
{
  assert(d != NULL);
  free(d->file);
  tags_free(d->tags);
  free(d);
}

struct tag_ev_response *tag_ev_data_dup(const struct tag_ev_response *d)
{
  assert(d != NULL);
  assert(d->file != NULL);

  struct tag_ev_response *n =
      (struct tag_ev_response *)xmalloc(sizeof(struct tag_ev_response));
  n->file = xstrdup(d->file);
  n->tags = d->tags ? tags_dup(d->tags) : NULL;
  return n;
}

/* -----------------------------------------------------------------------
 * move_ev_data helpers
 * ----------------------------------------------------------------------- */

void free_move_ev_data(struct move_ev_data *m)
{
  assert(m != NULL);
  free(m->from);
  free(m->to);
  free(m);
}

struct move_ev_data *move_ev_data_dup(const struct move_ev_data *m)
{
  assert(m != NULL);
  assert(m->from != NULL);
  assert(m->to != NULL);

  struct move_ev_data *n =
      (struct move_ev_data *)xmalloc(sizeof(struct move_ev_data));
  n->from = xstrdup(m->from);
  n->to   = xstrdup(m->to);
  return n;
}

// EOF
