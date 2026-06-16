// src/ui/curses/interface.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef INTERFACE_H
#define INTERFACE_H

#include <string>
#include <vector>

#include "core/server.h"

#ifdef __cplusplus
extern "C"
{
#endif

  /* The desired state of the application. */
  enum want_quit
  {
    NO_QUIT,  /* don't want to quit */
    QUIT_APP  /* quit the application and audio engine */
  };

  void init_interface(struct engine_event_queue *eq, const std::vector<std::string> &args);
  void interface_loop();
  void interface_end();
  int user_wants_interrupt();
  void interface_error(const char *msg);
  void interface_fatal(const char *format, ...) ATTR_PRINTF(1, 2);

#ifdef __cplusplus
}
#endif

#endif

// EOF
