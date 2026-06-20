// src/audio/conversion/audio_conversion.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef AUDIO_CONVERSION_H
#define AUDIO_CONVERSION_H

#ifdef HAVE_STDINT_H
#include <cstdint>
#endif

#include <sys/types.h>

#ifdef HAVE_SAMPLERATE
#include <samplerate.h>
#endif

#include "audio/audio.h"


  struct audio_conversion
  {
    struct sound_params from;
    struct sound_params to;

#ifdef HAVE_SAMPLERATE
    SRC_STATE *src_state;
    float *resample_buf;
    size_t resample_buf_nsamples; /* in samples ( sizeof(float) ) */
#endif
  };

  int audio_conv_new(struct audio_conversion *conv,
                     const struct sound_params *from,
                     const struct sound_params *to);
  char *audio_conv(struct audio_conversion *conv, const char *buf,
                   const size_t size, size_t *conv_len);
  void audio_conv_destroy(struct audio_conversion *conv);


#endif

// EOF
