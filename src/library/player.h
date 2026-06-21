// src/library/player.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef PLAYER_H
#define PLAYER_H

#include "audio/outputs/out_buf.h"
#include "io/io.h"
#include "library/playlist.h"


  void player_cleanup();
  void player(const char *file, const char *next_file, OutBuf *out_buf);
  void player_stop();
  void player_seek(const int n);
  void player_jump_to(const int n);
  void player_reset();
  void player_init();
  void player_pause();
  void player_unpause();


#endif

// EOF
