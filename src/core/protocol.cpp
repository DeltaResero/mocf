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
#include <string>

#include "core/common.h"
#include "core/log.h"
#include "core/protocol.h"
#include "library/playlist.h"


namespace {

void free_plist_item(void *p)      { delete static_cast<struct plist_item *>(p); }
void free_std_string(void *p)      { delete static_cast<std::string *>(p); }
void free_tag_ev_response(void *p) { delete static_cast<struct tag_ev_response *>(p); }
void free_srv_error_ev(void *p)    { delete static_cast<struct srv_error_ev *>(p); }
void free_move_ev_data(void *p)    { delete static_cast<struct move_ev_data *>(p); }

/* Events that carry no data should never see a non-nullptr pointer;
 * abort() catches that bug immediately instead of leaking it. */
void free_none(void *p)            { if (p) abort(); }

} // namespace

/* Returns the deleter appropriate for the given event type's data. */
void (*event_deleter(const int type))(void *)
{
  switch (type)
  {
    case EV_QUEUE_ADD:                  return free_plist_item;
    case EV_FILE_TAGS:                  return free_tag_ev_response;
    case EV_SRV_ERROR:                  return free_srv_error_ev;
    case EV_STATUS_MSG: case EV_QUEUE_DEL: return free_std_string;
    case EV_QUEUE_MOVE:                  return free_move_ev_data;
    default:                             return free_none;
  }
}

/* Free data associated with an event. */
void free_event_data(const int type, void *data)
{
  event_deleter(type)(data);
}

// EOF
