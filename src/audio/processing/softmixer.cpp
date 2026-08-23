// src/audio/processing/softmixer.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Softmixer-extension Copyright (C) 2007-2008 Hendrik Iben <hiben@tzi.de>
// Provides a software-mixer to regulate volume independent from
// hardware.
// Copyright (C) 2004-2008 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <atomic>

/* #define DEBUG */

#include "core/common.h"
#include "audio/audio.h"
#include "audio/conversion/audio_conversion.h"
#include "audio/processing/softmixer.h"
#include "core/options.h"
#include "library/files.h"
#include "core/log.h"

/* The audio thread reads these while the server thread changes them. */
static std::atomic<int> active;
static std::atomic<int> mix_mono;
static std::atomic<int> mixer_real;
static std::atomic<float> mixer_realf;

/* Only ever touched by the server thread. */
static int mixer_val;
static int mixer_amp;

static void softmixer_read_config();
static void softmixer_write_config();

/* public code */

std::string softmixer_name()
{
  return active ? SOFTMIXER_NAME : SOFTMIXER_NAME_OFF;
}

void softmixer_init()
{
  active = 0;
  mix_mono = 0;
  mixer_amp = 100;
  softmixer_set_value(100);
  softmixer_read_config();
  logit("Softmixer initialized");
}

void softmixer_shutdown()
{
  if (options_get_bool(SOFTMIXER_SAVE_OPTION))
  {
    softmixer_write_config();
  }
  logit("Softmixer stopped");
}

void softmixer_set_value(const int val)
{
  mixer_val = std::clamp(val, 0, 100);
  int real = exp((mixer_val * mixer_amp) / 100 * 0.06908);
  if (mixer_val < 10)
  {
    real = static_cast<int>(real * mixer_val /
                       10.f); // linear roll-off to zero for low values
  }
  real = std::clamp(real, SOFTMIXER_MIN, SOFTMIXER_MAX);
  mixer_real.store(real, std::memory_order_relaxed);
  mixer_realf.store(static_cast<float>(real) / 1000.0f, std::memory_order_relaxed);

  debug("Softmixer value: %d, gain: %d", mixer_val, real);
}

int softmixer_get_value() { return mixer_val; }

void softmixer_set_active(int act)
{
  if (act)
  {
    active = 1;
  }
  else
  {
    active = 0;
  }
}

int softmixer_is_active() { return active; }

void softmixer_set_mono(int mono)
{
  if (mono)
  {
    mix_mono = 1;
  }
  else
  {
    mix_mono = 0;
  }
}

int softmixer_is_mono() { return mix_mono; }

/* private code */

/* Apply the softmixer gain to each sample, clamping to [min_val, max_val].
 * For unsigned types the sample is re-centred around zero before scaling. */
template <typename T, typename AccT, AccT min_val, AccT max_val, AccT offset = 0>
static void process_buffer_impl(T *buf, size_t samples)
{
  debug("mixing");
  const AccT gain = mixer_real.load(std::memory_order_relaxed);
  for (size_t i = 0; i < samples; i++)
  {
    AccT tmp = static_cast<AccT>(buf[i]) - offset;
    tmp *= gain;
    tmp /= 1000;
    tmp += offset;
    tmp = std::clamp<AccT>(tmp, min_val, max_val);
    buf[i] = static_cast<T>(tmp);
  }
}

/* Average all channels for each frame into a mono value, then write it back
 * to all channels, clamping to [min_val, max_val]. */
template <typename T, typename AccT, AccT min_val, AccT max_val>
static void mix_mono_impl(T *buf, int channels, size_t samples)
{
  debug("making mono");
  assert(channels > 1);
  size_t i = 0;
  while (i < samples)
  {
    AccT mono = 0;
    for (int c = 0; c < channels; c++)
      mono += static_cast<AccT>(*buf++);
    buf -= channels;
    mono /= channels;
    mono = std::clamp<AccT>(mono, min_val, max_val);
    for (int c = 0; c < channels; c++)
      *buf++ = static_cast<T>(mono);
    i += channels;
  }
}

static void softmixer_read_config()
{
  std::string cfname = create_file_name(SOFTMIXER_SAVE_FILE);

  FILE *cf = fopen(cfname.c_str(), "r");

  if (cf == nullptr)
  {
    logit("Unable to read softmixer configuration");
    return;
  }

  int tmp;

  while (auto linebuffer = read_line(cf))
  {
    if (strncasecmp(linebuffer->c_str(), SOFTMIXER_CFG_ACTIVE,
                    sizeof(SOFTMIXER_CFG_ACTIVE) - 1) == 0)
    {
      if (sscanf(linebuffer->c_str() + sizeof(SOFTMIXER_CFG_ACTIVE) - 1, " %i", &tmp) == 1)
      {
        if (tmp > 0)
        {
          active = 1;
        }
        else
        {
          active = 0;
        }
      }
    }
    if (strncasecmp(linebuffer->c_str(), SOFTMIXER_CFG_AMP, sizeof(SOFTMIXER_CFG_AMP) - 1) ==
        0)
    {
      if (sscanf(linebuffer->c_str() + sizeof(SOFTMIXER_CFG_AMP) - 1, " %i", &tmp) == 1)
      {
        if (in_closed_range(SOFTMIXER_MIN, tmp, SOFTMIXER_MAX))
        {
          mixer_amp = tmp;
        }
        else
        {
          logit("Tried to set softmixer amplification out of range.");
        }
      }
    }
    if (strncasecmp(linebuffer->c_str(), SOFTMIXER_CFG_VALUE,
                    sizeof(SOFTMIXER_CFG_VALUE) - 1) == 0)
    {
      if (sscanf(linebuffer->c_str() + sizeof(SOFTMIXER_CFG_VALUE) - 1, " %i", &tmp) == 1)
      {
        if (in_closed_range(0, tmp, 100))
        {
          softmixer_set_value(tmp);
        }
        else
        {
          logit("Tried to set softmixer value out of range.");
        }
      }
    }
    if (strncasecmp(linebuffer->c_str(), SOFTMIXER_CFG_MONO,
                    sizeof(SOFTMIXER_CFG_MONO) - 1) == 0)
    {
      if (sscanf(linebuffer->c_str() + sizeof(SOFTMIXER_CFG_MONO) - 1, " %i", &tmp) == 1)
      {
        if (tmp > 0)
        {
          mix_mono = 1;
        }
        else
        {
          mix_mono = 0;
        }
      }
    }

  }

  fclose(cf);
}

static void softmixer_write_config()
{
  std::string cfname = create_file_name(SOFTMIXER_SAVE_FILE);

  FILE *cf = fopen(cfname.c_str(), "w");

  if (cf == nullptr)
  {
    logit("Unable to write softmixer configuration");
    return;
  }

  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_ACTIVE, active.load());
  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_AMP, mixer_amp);
  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_VALUE, mixer_val);
  fprintf(cf, "%s %i\n", SOFTMIXER_CFG_MONO, mix_mono.load());

  fclose(cf);

  logit("Softmixer configuration written");
}

void softmixer_process_buffer(char *buf, size_t size,
                              const struct sound_params *sound_params)
{
  int do_softmix;
  int do_monomix;

  debug("Processing %zu bytes...", size);

  do_softmix = active && (mixer_real != 1000); /* 1000 is unity gain */
  do_monomix = mix_mono && (sound_params->channels > 1);

  if (!do_softmix && !do_monomix)
  {
    return;
  }

  long sound_format = sound_params->fmt & SFMT_MASK_FORMAT;
  int samplewidth = sfmt_Bps(sound_format);

  assert(size % (samplewidth * sound_params->channels) == 0);

  switch (sound_format)
  {
    case SFMT_U8:
      if (do_softmix)
        process_buffer_impl<uint8_t, int16_t, 0, UINT8_MAX, (int16_t)(UINT8_MAX >> 1)>(reinterpret_cast<uint8_t *>(buf), size);
      if (do_monomix)
        mix_mono_impl<uint8_t, int16_t, 0, UINT8_MAX>(reinterpret_cast<uint8_t *>(buf), sound_params->channels, size);
      break;
    case SFMT_S8:
      if (do_softmix)
        process_buffer_impl<int8_t, int16_t, INT8_MIN, INT8_MAX>(reinterpret_cast<int8_t *>(buf), size);
      if (do_monomix)
        mix_mono_impl<int8_t, int16_t, INT8_MIN, INT8_MAX>(reinterpret_cast<int8_t *>(buf), sound_params->channels, size);
      break;
    case SFMT_U16:
      if (do_softmix)
        process_buffer_impl<uint16_t, int32_t, 0, UINT16_MAX, (int32_t)(UINT16_MAX >> 1)>(reinterpret_cast<uint16_t *>(buf), size / sizeof(uint16_t));
      if (do_monomix)
        mix_mono_impl<uint16_t, int32_t, 0, UINT16_MAX>(reinterpret_cast<uint16_t *>(buf), sound_params->channels, size / sizeof(uint16_t));
      break;
    case SFMT_S16:
      if (do_softmix)
        process_buffer_impl<int16_t, int32_t, INT16_MIN, INT16_MAX>(reinterpret_cast<int16_t *>(buf), size / sizeof(int16_t));
      if (do_monomix)
        mix_mono_impl<int16_t, int32_t, INT16_MIN, INT16_MAX>(reinterpret_cast<int16_t *>(buf), sound_params->channels, size / sizeof(int16_t));
      break;
    case SFMT_U24:
      if (do_softmix)
        process_buffer_impl<uint32_t, int64_t, 0, U24_MAX, (int64_t)(-S24_MIN)>(reinterpret_cast<uint32_t *>(buf), size / sizeof(uint32_t));
      if (do_monomix)  /* was incorrectly checking mix_mono; fixed */
        mix_mono_impl<uint32_t, int64_t, 0, U24_MAX>(reinterpret_cast<uint32_t *>(buf), sound_params->channels, size / sizeof(uint32_t));
      break;
    case SFMT_S24:
      if (do_softmix)
        process_buffer_impl<int32_t, int64_t, S24_MIN, S24_MAX>(reinterpret_cast<int32_t *>(buf), size / sizeof(int32_t));
      if (do_monomix)  /* was incorrectly checking mix_mono; fixed */
        mix_mono_impl<int32_t, int64_t, S24_MIN, S24_MAX>(reinterpret_cast<int32_t *>(buf), sound_params->channels, size / sizeof(int32_t));
      break;
    case SFMT_U32:
      if (do_softmix)
        process_buffer_impl<uint32_t, int64_t, 0, UINT32_MAX, (int64_t)(UINT32_MAX >> 1)>(reinterpret_cast<uint32_t *>(buf), size / sizeof(uint32_t));
      if (do_monomix)
        mix_mono_impl<uint32_t, int64_t, 0, UINT32_MAX>(reinterpret_cast<uint32_t *>(buf), sound_params->channels, size / sizeof(uint32_t));
      break;
    case SFMT_S32:
      if (do_softmix)
        process_buffer_impl<int32_t, int64_t, INT32_MIN, INT32_MAX>(reinterpret_cast<int32_t *>(buf), size / sizeof(int32_t));
      if (do_monomix)
        mix_mono_impl<int32_t, int64_t, INT32_MIN, INT32_MAX>(reinterpret_cast<int32_t *>(buf), sound_params->channels, size / sizeof(int32_t));
      break;
    case SFMT_FLOAT:
      if (do_softmix)
      {
        float *fbuf = reinterpret_cast<float *>(buf);
        size_t n = size / sizeof(float);
        const float gain = mixer_realf.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; i++)
          fbuf[i] = std::clamp(fbuf[i] * gain, -1.0f, 1.0f);
      }
      if (do_monomix)
        mix_mono_impl<float, float, -1.0f, 1.0f>(reinterpret_cast<float *>(buf), sound_params->channels, size / sizeof(float));
      break;
    default:
      debug("No softmixer/monomixer for chosen format.");
  }
}

// EOF
