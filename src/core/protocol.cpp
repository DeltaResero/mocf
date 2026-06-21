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


/* Free data associated with an event. */
void free_event_data(const int type, void *data)
{
  if (!data)
    return;

  if (type == EV_QUEUE_ADD)
  {
    auto *item = static_cast<struct plist_item *>(data);
    plist_free_item_fields(item);
    delete item;
  }
  else if (type == EV_FILE_TAGS)
  {
    delete static_cast<struct tag_ev_response *>(data);
  }
  else if (type == EV_SRV_ERROR)
  {
    delete static_cast<struct srv_error_ev *>(data);
  }
  else if (type == EV_STATUS_MSG || type == EV_QUEUE_DEL)
  {
    delete static_cast<std::string *>(data);
  }
  else if (type == EV_QUEUE_MOVE)
  {
    delete static_cast<struct move_ev_data *>(data);
  }
  else
  {
    abort(); /* BUG: unknown event type with non-nullptr data */
  }
}

// EOF
