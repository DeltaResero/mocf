// src/audio/processing/equalizer.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef EQUALIZER_H
#define EQUALIZER_H

#include <string>

  void equalizer_init();
  void equalizer_shutdown();
  void equalizer_process_buffer(char *buf, size_t size,
                                const struct sound_params *sound_params);
  void equalizer_refresh();
  int equalizer_is_active();
  int equalizer_set_active(int active);
  std::string equalizer_current_eqname();
  void equalizer_next();
  void equalizer_prev();


#endif

// EOF
