// src/audio/processing/softmixer.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef SOFTMIXER_H
#define SOFTMIXER_H


#define SOFTMIXER_MIN 0
/* Allow amplification, might result in clipping... */
#define SOFTMIXER_MAX 2000

#define SOFTMIXER_NAME "Software"
#define SOFTMIXER_NAME_OFF "Software Off"

#define SOFTMIXER_CFG_ACTIVE "Active:"
#define SOFTMIXER_CFG_AMP "Amplification:"
#define SOFTMIXER_CFG_VALUE "Value:"
#define SOFTMIXER_CFG_MONO "Mono:"

#define SOFTMIXER_SAVE_OPTION "Softmixer_SaveState"
#define SOFTMIXER_SAVE_FILE "softmixer"

  char *softmixer_name();

  void softmixer_init();
  void softmixer_shutdown();

  int softmixer_get_value();
  void softmixer_set_value(const int val);

  int softmixer_is_active();
  void softmixer_set_active(int act);

  int softmixer_is_mono();
  void softmixer_set_mono(int mono);

  void softmixer_process_buffer(char *buf, const size_t size,
                                const struct sound_params *sound_params);


#endif

// EOF
