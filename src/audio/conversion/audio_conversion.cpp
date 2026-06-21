// src/audio/conversion/audio_conversion.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Code for conversion between float and fixed point types is based on
// libsamplerate:
// For future: audio conversion should be performed in order:
// channels -> rate -> format
// Copyright (C) 2005 Damian Pietras <daper@daper.net>
// Copyright (C) 2002-2004 Erik de Castro Lopo <erikd@mega-nerd.com>
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
#include <cstdlib>
#include <cstring>
#include <vector>
#include <stdexcept>

#ifdef HAVE_SAMPLERATE
#include <samplerate.h>
#endif

#define DEBUG

#include "core/common.h"
#include "audio/conversion/audio_conversion.h"
#include "core/log.h"
#include "core/options.h"

static void float_to_u8(const float *in, unsigned char *out,
                        const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    float f = in[i] * static_cast<float>INT32_MAX;

    if (f >= static_cast<float>INT32_MAX)
    {
      out[i] = UINT8_MAX;
    }
    else if (f <= INT32_MIN)
    {
      out[i] = 0;
    }
    else
    {
      out[i] = static_cast<unsigned int>((lrintf(f) >> 24) - INT8_MIN);
    }
  }
}

static void float_to_s8(const float *in, char *out, const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    float f = in[i] * static_cast<float>INT32_MAX;

    if (f >= static_cast<float>INT32_MAX)
    {
      out[i] = INT8_MAX;
    }
    else if (f <= INT32_MIN)
    {
      out[i] = INT8_MIN;
    }
    else
    {
      out[i] = lrintf(f) >> 24;
    }
  }
}

static void float_to_u16(const float *in, unsigned char *out,
                         const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    uint16_t *out_val = reinterpret_cast<uint16_t *>(out + i * sizeof(uint16_t));
    float f = in[i] * static_cast<float>INT32_MAX;

    if (f >= static_cast<float>INT32_MAX)
    {
      *out_val = UINT16_MAX;
    }
    else if (f <= INT32_MIN)
    {
      *out_val = 0;
    }
    else
    {
      *out_val = static_cast<unsigned int>((lrintf(f) >> 16) - INT16_MIN);
    }
  }
}

static void float_to_s16(const float *in, char *out, const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    int16_t *out_val = reinterpret_cast<int16_t *>(out + i * sizeof(int16_t));
    float f = in[i] * static_cast<float>INT32_MAX;

    if (f >= static_cast<float>INT32_MAX)
    {
      *out_val = INT16_MAX;
    }
    else if (f <= INT32_MIN)
    {
      *out_val = INT16_MIN;
    }
    else
    {
      *out_val = lrintf(f) >> 16;
    }
  }
}

static void float_to_u24(const float *in, unsigned char *out,
                         const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    uint32_t *out_val = reinterpret_cast<uint32_t *>(out + i * sizeof(uint32_t));
    float f = in[i] * S24_MAX;

    if (f >= S24_MAX)
    {
      *out_val = U24_MAX;
    }
    else if (f <= S24_MIN)
    {
      *out_val = 0;
    }
    else
    {
      *out_val = static_cast<uint32_t>(lrintf(f) - S24_MIN);
    }
  }
}

static void float_to_s24(const float *in, char *out, const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    int32_t *out_val = reinterpret_cast<int32_t *>(out + i * sizeof(int32_t));
    float f = in[i] * S24_MAX;

    if (f >= S24_MAX)
    {
      *out_val = S24_MAX;
    }
    else if (f <= S24_MIN)
    {
      *out_val = S24_MIN;
    }
    else
    {
      *out_val = lrintf(f);
    }
  }
}

static void float_to_u24_3(const float *in, unsigned char *out,
                           const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    int8_t *out_val = reinterpret_cast<int8_t *>(out + 3 * i);
    float f = in[i] * S24_MAX;
    uint32_t out_i;

    if (f >= S24_MAX)
    {
      out_i = U24_MAX;
    }
    else if (f <= S24_MIN)
    {
      out_i = 0;
    }
    else
    {
      out_i = static_cast<uint32_t>(lrintf(f) - S24_MIN);
    }
    out_val[0] = (out_i & 0x000000FF);
    out_val[1] = (out_i & 0x0000FF00) >> 8;
    out_val[2] = (out_i & 0x00FF0000) >> 16;
  }
}

static void float_to_s24_3(const float *in, char *out, const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  int32_t out_i;
  for (i = 0; i < samples; i++)
  {
    int8_t *out_val = reinterpret_cast<int8_t *>(out + 3 * i);
    float f = in[i] * S24_MAX;

    if (f >= S24_MAX)
    {
      out_i = S24_MAX;
    }
    else if (f <= S24_MIN)
    {
      out_i = S24_MIN;
    }
    else
    {
      out_i = lrintf(f);
    }
    out_val[0] = (out_i & 0x000000FF);
    out_val[1] = (out_i & 0x0000FF00) >> 8;
    out_val[2] = (out_i & 0x00FF0000) >> 16;
  }
}

static void float_to_u32(const float *in, unsigned char *out,
                         const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    uint32_t *out_val = reinterpret_cast<uint32_t *>(out + i * sizeof(uint32_t));
    float f = in[i] * INT32_MAX;

    if (f >= INT32_MAX)
    {
      *out_val = INT32_MAX;
    }
    else if (f <= INT32_MIN)
    {
      *out_val = 0;
    }
    else
    {
      *out_val = static_cast<uint32_t>(lrintf(f) - INT32_MIN);
    }
  }
}

static void float_to_s32(const float *in, char *out, const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    int32_t *out_val = reinterpret_cast<int32_t *>(out + i * sizeof(int32_t));
    float f = in[i] * INT32_MAX;

    if (f >= INT32_MAX)
    {
      *out_val = INT32_MAX;
    }
    else if (f <= INT32_MIN)
    {
      *out_val = INT32_MIN;
    }
    else
    {
      *out_val = lrintf(f);
    }
  }
}

static void u8_to_float(const unsigned char *in, float *out,
                        const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    out[i] = ((static_cast<int>(*in++)) + INT8_MIN) / static_cast<float>(INT8_MAX + 1);
  }
}

static void s8_to_float(const char *in, float *out, const size_t samples)
{
  size_t i;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    out[i] = *in++ / static_cast<float>(INT8_MAX + 1);
  }
}

static void u16_to_float(const unsigned char *in, float *out,
                         const size_t samples)
{
  size_t i;
  const uint16_t *in_16 = (uint16_t *)in;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    out[i] = (static_cast<int>(*in_16++) + INT16_MIN) / static_cast<float>(INT16_MAX + 1);
  }
}

static void s16_to_float(const char *in, float *out, const size_t samples)
{
  size_t i;
  const int16_t *in_16 = (int16_t *)in;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    out[i] = *in_16++ / static_cast<float>(INT16_MAX + 1);
  }
}

static void u24_to_float(const unsigned char *in, float *out,
                         const size_t samples)
{
  size_t i;
  const uint32_t *in_32 = (uint32_t *)in;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    out[i] = (static_cast<float>(*in_32++) + static_cast<float>(S24_MIN)) / (static_cast<float>S24_MAX + 1.0);
  }
}

static void s24_to_float(const char *in, float *out, const size_t samples)
{
  size_t i;
  const int32_t *in_32 = (int32_t *)in;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    out[i] = *in_32++ / (static_cast<float>S24_MAX + 1.0);
  }
}

static void s24_3_to_float(const char *in, float *out, const size_t samples)
{
  size_t i;
  const int8_t *in_8 = (int8_t *)in;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
#ifdef WORDS_BIGENDIAN
    out[i] = (*(in_8 + 2) + (*(in_8 + 1) << 8) + (*(in_8) << 16)) /
             ((float)S24_MAX + 1.0);
#else
    out[i] = (*(in_8) + (*(in_8 + 1) << 8) + (*(in_8 + 2) << 16)) /
             (static_cast<float>S24_MAX + 1.0);
#endif
    in_8 += 3;
  }
}

static void u24_3_to_float(const char *in, float *out, const size_t samples)
{
  size_t i;
  const uint8_t *in_8 = (uint8_t *)in;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
#ifdef WORDS_BIGENDIAN
    out[i] =
        (*(in_8 + 2) + (*(in_8 + 1) << 8) + (*(in_8) << 16) + (float)S24_MIN) /
        ((float)S24_MAX + 1.0);
#else
    out[i] =
        (*(in_8) + (*(in_8 + 1) << 8) + (*(in_8 + 2) << 16) + static_cast<float>(S24_MIN)) /
        (static_cast<float>S24_MAX + 1.0);
#endif
    in_8 += 3;
  }
}

static void u32_to_float(const unsigned char *in, float *out,
                         const size_t samples)
{
  size_t i;
  const uint32_t *in_32 = (uint32_t *)in;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    out[i] = (static_cast<float>(*in_32++) + static_cast<float>INT32_MIN) / (static_cast<float>INT32_MAX + 1.0);
  }
}

static void s32_to_float(const char *in, float *out, const size_t samples)
{
  size_t i;
  const int32_t *in_32 = (int32_t *)in;

  assert(in != nullptr);
  assert(out != nullptr);

  for (i = 0; i < samples; i++)
  {
    out[i] = *in_32++ / (static_cast<float>INT32_MAX + 1.0);
  }
}

/* Convert fixed point samples in format fmt (size in bytes) to float. */
static std::vector<float> fixed_to_float(const char *buf, const size_t size, const long fmt)
{
  std::vector<float> out;
  char fmt_name[SFMT_STR_MAX];

  assert((fmt & SFMT_MASK_FORMAT) != SFMT_FLOAT);

  switch (fmt & SFMT_MASK_FORMAT)
  {
    case SFMT_U8:
      out.resize(size);
      u8_to_float((unsigned char *)buf, out.data(), size);
      break;
    case SFMT_S8:
      out.resize(size);
      s8_to_float(buf, out.data(), size);
      break;
    case SFMT_U16:
      out.resize(size / 2);
      u16_to_float((unsigned char *)buf, out.data(), size / 2);
      break;
    case SFMT_S16:
      out.resize(size / 2);
      s16_to_float(buf, out.data(), size / 2);
      break;
    case SFMT_U24:
      out.resize(size / 4);
      u24_to_float((unsigned char *)buf, out.data(), size / 4);
      break;
    case SFMT_S24:
      out.resize(size / 4);
      s24_to_float(buf, out.data(), size / 4);
      break;
    case SFMT_S24_3:
      out.resize(size / 3);
      s24_3_to_float(buf, out.data(), size / 3);
      break;
    case SFMT_U24_3:
      out.resize(size / 3);
      u24_3_to_float(buf, out.data(), size / 3);
      break;
    case SFMT_U32:
      out.resize(size / 4);
      u32_to_float((unsigned char *)buf, out.data(), size / 4);
      break;
    case SFMT_S32:
      out.resize(size / 4);
      s32_to_float(buf, out.data(), size / 4);
      break;
    default:
      error("Can't convert from %s to float!",
            sfmt_str(fmt, fmt_name, sizeof(fmt_name)));
      abort();
  }

  return out;
}

/* Convert float samples to fixed point format fmt. */
static std::vector<char> float_to_fixed(const float *buf, const size_t samples,
                                        const long fmt)
{
  char fmt_name[SFMT_STR_MAX];
  std::vector<char> new_snd;

  assert((fmt & SFMT_MASK_FORMAT) != SFMT_FLOAT);

  switch (fmt & SFMT_MASK_FORMAT)
  {
    case SFMT_U8:
      new_snd.resize(samples);
      float_to_u8(buf, reinterpret_cast<unsigned char *>(new_snd.data()), samples);
      break;
    case SFMT_S8:
      new_snd.resize(samples);
      float_to_s8(buf, new_snd.data(), samples);
      break;
    case SFMT_U16:
      new_snd.resize(samples * 2);
      float_to_u16(buf, reinterpret_cast<unsigned char *>(new_snd.data()), samples);
      break;
    case SFMT_S16:
      new_snd.resize(samples * 2);
      float_to_s16(buf, new_snd.data(), samples);
      break;
    case SFMT_U24:
      new_snd.resize(samples * 4);
      float_to_u24(buf, reinterpret_cast<unsigned char *>(new_snd.data()), samples);
      break;
    case SFMT_S24:
      new_snd.resize(samples * 4);
      float_to_s24(buf, new_snd.data(), samples);
      break;
    case SFMT_U24_3:
      new_snd.resize(samples * 3);
      float_to_u24_3(buf, reinterpret_cast<unsigned char *>(new_snd.data()), samples);
      break;
    case SFMT_S24_3:
      new_snd.resize(samples * 3);
      float_to_s24_3(buf, new_snd.data(), samples);
      break;
    case SFMT_U32:
      new_snd.resize(samples * 4);
      float_to_u32(buf, reinterpret_cast<unsigned char *>(new_snd.data()), samples);
      break;
    case SFMT_S32:
      new_snd.resize(samples * 4);
      float_to_s32(buf, new_snd.data(), samples);
      break;
    default:
      error("Can't convert from float to %s!",
            sfmt_str(fmt, fmt_name, sizeof(fmt_name)));
      abort();
  }

  return new_snd;
}

static inline void change_sign_8(uint8_t *buf, const size_t samples)
{
  size_t i;

  for (i = 0; i < samples; i++)
  {
    *buf++ ^= 1 << 7;
  }
}

static inline void change_sign_16(uint16_t *buf, const size_t samples)
{
  size_t i;

  for (i = 0; i < samples; i++)
  {
    *buf++ ^= 1 << 15;
  }
}

static inline void change_sign_24(uint32_t *buf, const size_t samples)
{
  size_t i;

  for (i = 0; i < samples; i++)
  {
    *buf++ ^= 1 << 23;
  }
}

static inline void change_sign_32(uint32_t *buf, const size_t samples)
{
  size_t i;

  for (i = 0; i < samples; i++)
  {
    *buf++ ^= 1 << 31;
  }
}

/* Change the signs of samples in format *fmt.  Also changes fmt to the new
 * format. */
static void change_sign(char *buf, const size_t size, long *fmt)
{
  char fmt_name[SFMT_STR_MAX];

  switch (*fmt & SFMT_MASK_FORMAT)
  {
    case SFMT_S8:
    case SFMT_U8:
      change_sign_8(reinterpret_cast<uint8_t *>(buf), size);
      if (*fmt & SFMT_S8)
      {
        *fmt = sfmt_set_fmt(*fmt, SFMT_U8);
      }
      else
      {
        *fmt = sfmt_set_fmt(*fmt, SFMT_S8);
      }
      break;
    case SFMT_S16:
    case SFMT_U16:
      change_sign_16(reinterpret_cast<uint16_t *>(buf), size / 2);
      if (*fmt & SFMT_S16)
      {
        *fmt = sfmt_set_fmt(*fmt, SFMT_U16);
      }
      else
      {
        *fmt = sfmt_set_fmt(*fmt, SFMT_S16);
      }
      break;
    case SFMT_S24:
    case SFMT_U24:
      change_sign_24(reinterpret_cast<uint32_t *>(buf), size / 4);
      if (*fmt & SFMT_S24)
      {
        *fmt = sfmt_set_fmt(*fmt, SFMT_U24);
      }
      else
      {
        *fmt = sfmt_set_fmt(*fmt, SFMT_S24);
      }
      break;
    case SFMT_S32:
    case SFMT_U32:
      change_sign_32(reinterpret_cast<uint32_t *>(buf), size / 4);
      if (*fmt & SFMT_S32)
      {
        *fmt = sfmt_set_fmt(*fmt, SFMT_U32);
      }
      else
      {
        *fmt = sfmt_set_fmt(*fmt, SFMT_S32);
      }
      break;
    default:
      error("Request for changing sign of unknown format: %s",
            sfmt_str(*fmt, fmt_name, sizeof(fmt_name)));
      abort();
  }
}

static inline void audio_conv_bswap_16(int16_t *buf, const size_t num)
{
  size_t i;

  for (i = 0; i < num; i++)
  {
    buf[i] = bswap_16(buf[i]);
  }
}

static inline void audio_conv_bswap_24(int8_t *buf, const size_t num)
{
  size_t i;
  int8_t tmp;

  for (i = 0; i < num; i += 3)
  {
    tmp = buf[i];
    buf[i] = buf[i + 2];
    buf[i + 2] = tmp;
  }
}

static inline void audio_conv_bswap_32(int32_t *buf, const size_t num)
{
  size_t i;

  for (i = 0; i < num; i++)
  {
    buf[i] = bswap_32(buf[i]);
  }
}

/* Swap endianness of fixed point samples. */
static void swap_endian(char *buf, const size_t size, const long fmt)
{
  if ((fmt & (SFMT_S8 | SFMT_U8 | SFMT_FLOAT)))
  {
    return;
  }
  debug("Swapping endianness");

  switch (fmt & SFMT_MASK_FORMAT)
  {
    case SFMT_S16:
    case SFMT_U16:
      audio_conv_bswap_16(reinterpret_cast<int16_t *>(buf), size / 2);
      break;
    case SFMT_S24:
    case SFMT_U24:
    case SFMT_S32:
    case SFMT_U32:
      audio_conv_bswap_32(reinterpret_cast<int32_t *>(buf), size / 4);
      break;
    case SFMT_S24_3:
    case SFMT_U24_3:
      audio_conv_bswap_24(reinterpret_cast<int8_t *>(buf), size);
      break;
    default:
      error("Can't convert to native endian!");
      abort(); /* we can't do anything smarter */
  }
}

/* Initialize the audio_conversion structure for conversion between parameters
 * from and to. Throws AudioConversionException on error. */
AudioConversion::AudioConversion(const sound_params& from, const sound_params& to)
  : from_params(from), to_params(to)
{
  assert(from_params.rate != to_params.rate || from_params.fmt != to_params.fmt ||
         from_params.channels != to_params.channels);

  if (from_params.channels != to_params.channels)
  {
    /* the only conversion we can do */
    if (!((from_params.channels == 1 || from_params.channels == 6) && to_params.channels == 2))
    {
      error("Can't change number of channels (%d to %d)!", from_params.channels,
            to_params.channels);
      throw AudioConversionException("Unsupported channel conversion");
    }
  }

  if (from_params.rate != to_params.rate)
  {
    if (options_get_int("EnableResample") == 0)
    {
      error("Resampling disabled!");
      throw AudioConversionException("Resampling disabled");
    }
#ifdef HAVE_SAMPLERATE
    int err;
    int resample_type = -1;
    const char *method = options_get_symb("ResampleMethod");

    if (!strcasecmp(method, "SincBestQuality"))
    {
      resample_type = SRC_SINC_BEST_QUALITY;
    }
    else if (!strcasecmp(method, "SincMediumQuality"))
    {
      resample_type = SRC_SINC_MEDIUM_QUALITY;
    }
    else if (!strcasecmp(method, "SincFastest"))
    {
      resample_type = SRC_SINC_FASTEST;
    }
    else if (!strcasecmp(method, "ZeroOrderHold"))
    {
      resample_type = SRC_ZERO_ORDER_HOLD;
    }
    else if (!strcasecmp(method, "Linear"))
    {
      resample_type = SRC_LINEAR;
    }
    else
    {
      fatal("Bad ResampleMethod option: %s", method);
    }

    src_state = src_new(resample_type, from_params.channels, &err);
    if (!src_state)
    {
      error("Can't resample from %dHz to %dHz: %s", from_params.rate, to_params.rate,
            src_strerror(err));
      throw AudioConversionException("Failed to initialize resampler");
    }
    logit("Resampling from %dHz to %dHz using %s", from_params.rate, to_params.rate,
          method);
#else
    error("Resampling not supported!");
    throw AudioConversionException("Resampling not supported");
#endif
  }
}

AudioConversion::~AudioConversion()
{
#ifdef HAVE_SAMPLERATE
  if (src_state)
  {
    src_delete(src_state);
  }
#endif
}

#ifdef HAVE_SAMPLERATE
std::vector<float> AudioConversion::resample_sound(const float *buf,
                             const size_t samples, const int nchannels)
{
  SRC_DATA resample_data;
  std::vector<float> output;

  resample_data.end_of_input = 0;
  resample_data.src_ratio = to_params.rate / static_cast<double>(from_params.rate);

  resample_data.input_frames =
      samples / nchannels + resample_buf.size() / nchannels;
  resample_data.output_frames =
      resample_data.input_frames * resample_data.src_ratio;

  size_t old_resample_size = resample_buf.size();
  resample_buf.resize(old_resample_size + samples);
  memcpy(resample_buf.data() + old_resample_size, buf, samples * sizeof(float));

  output.resize(resample_data.output_frames * nchannels);

  resample_data.data_in = resample_buf.data();
  resample_data.data_out = output.data();

  int output_samples = 0;

  do
  {
    int err;

    if ((err = src_process(src_state, &resample_data)))
    {
      error("Can't resample: %s", src_strerror(err));
      return {};
    }

    resample_data.data_in += resample_data.input_frames_used * nchannels;
    resample_data.input_frames -= resample_data.input_frames_used;
    resample_data.data_out += resample_data.output_frames_gen * nchannels;
    resample_data.output_frames -= resample_data.output_frames_gen;
    output_samples += resample_data.output_frames_gen * nchannels;
  } while (resample_data.input_frames && resample_data.output_frames_gen &&
           resample_data.output_frames);

  output.resize(output_samples);

  if (resample_data.input_frames)
  {
    size_t remaining_samples = resample_data.input_frames * nchannels;
    std::vector<float> new_resample_buf(resample_data.data_in, resample_data.data_in + remaining_samples);
    resample_buf = std::move(new_resample_buf);
  }
  else
  {
    resample_buf.clear();
  }

  return output;
}
#endif

/* Double the channels from */
static std::vector<char> mono_to_stereo(const char *mono, const size_t size,
                            const long format)
{
  int Bps = sfmt_Bps(format);
  size_t i;
  std::vector<char> stereo(size * 2);

  for (i = 0; i < size; i += Bps)
  {
    memcpy(stereo.data() + (i * 2), mono + i, Bps);
    memcpy(stereo.data() + (i * 2 + Bps), mono + i, Bps);
  }

  return stereo;
}

/* DPL downmix: 5.1 -> stereo */
static std::vector<char> ch6_to_stereo(const char *ch6, const size_t size,
                           const long format)
{
  debug("Downmixing from 5.1 to 2.0");
  int Bps = sfmt_Bps(format);
  size_t i;
  int j, k;
  std::vector<char> stereo(size / 3);

  float a[2][6]; // downmix matrix
  a[0][0] = 1.0;
  a[0][2] = 0.707;
  a[0][1] = 0;
  a[0][4] = -0.8165;
  a[0][5] = -0.5774;
  a[0][3] = 0.707;
  a[1][0] = 0;
  a[1][2] = 0.707;
  a[1][1] = 1.0;
  a[1][4] = 0.5774;
  a[1][5] = 0.8165;
  a[1][3] = 0.707;
  const float normalization = 0.2626;

  if (format & SFMT_S16)
  {
    debug("Downmixing from 5.1 to 2.0: S16");

    int16_t sample_in[6];
    int16_t sample_out[2];

    for (i = 0; i < size; i += 6 * Bps)
    {
      memcpy(&sample_in, ch6 + i, Bps * 6);
      sample_out[0] = 0;
      sample_out[1] = 0;
      for (j = 0; j < 2; j++)
      {
        for (k = 0; k < 6; k++)
        {
          sample_out[j] += a[j][k] * sample_in[k] * normalization;
        }
      }
      memcpy(stereo.data() + (i / 3), sample_out, 2 * Bps);
    }
  }
  else if (format & SFMT_FLOAT)
  {
    debug("Downmixing from 5.1 to 2.0: FLOAT");

    float sample_in[6];
    float sample_out[2];

    for (i = 0; i < size; i += 6 * Bps)
    {
      memcpy(&sample_in, ch6 + i, Bps * 6);
      sample_out[0] = 0;
      sample_out[1] = 0;
      for (j = 0; j < 2; j++)
      {
        for (k = 0; k < 6; k++)
        {
          sample_out[j] += (a[j][k] * sample_in[k] * normalization);
        }
      }
      memcpy(stereo.data() + (i / 3), sample_out, 2 * Bps);
    }
  }
  else if (format & SFMT_S32)
  {
    debug("Downmixing from 5.1 to 2.0: S32");

    int32_t sample_in[6];
    int32_t sample_out[2];

    for (i = 0; i < size; i += 6 * Bps)
    {
      memcpy(&sample_in, ch6 + i, Bps * 6);
      sample_out[0] = 0;
      sample_out[1] = 0;
      for (j = 0; j < 2; j++)
      {
        for (k = 0; k < 6; k++)
        {
          sample_out[j] += (a[j][k] * sample_in[k] * normalization);
        }
      }
      memcpy(stereo.data() + (i / 3), sample_out, 2 * Bps);
    }
  }
  else
  {
    error("Can't downsample that sample format yet.");
    abort();
  }
  return stereo;
}

static std::vector<char> s32_to_s24_3(const int32_t *in, const size_t samples)
{
  size_t i;
  std::vector<char> new_buf(samples * 3);
  int8_t *out = reinterpret_cast<int8_t *>(new_buf.data());

  for (i = 0; i < samples; i++)
  {
    out[3 * i] = (in[i] & 0x0000FF00) >> 8;
    out[3 * i + 1] = (in[i] & 0x00FF0000) >> 16;
    out[3 * i + 2] = (in[i] & 0xFF000000) >> 24;
  }
  return new_buf;
}

static std::vector<char> s32_to_s16(const int32_t *in, const size_t samples)
{
  size_t i;
  std::vector<char> new_buf(samples * 2);
  int16_t *out = reinterpret_cast<int16_t *>(new_buf.data());

  for (i = 0; i < samples; i++)
  {
    out[i] = in[i] >> 16;
  }

  return new_buf;
}

static std::vector<char> u32_to_u16(const uint32_t *in, const size_t samples)
{
  size_t i;
  std::vector<char> new_buf(samples * 2);
  uint16_t *out = reinterpret_cast<uint16_t *>(new_buf.data());

  for (i = 0; i < samples; i++)
  {
    out[i] = in[i] >> 16;
  }

  return new_buf;
}

static std::vector<char> s32_to_s24(const int32_t *in, const size_t samples)
{
  size_t i;
  std::vector<char> new_buf(samples * 4);
  int32_t *out = reinterpret_cast<int32_t *>(new_buf.data());

  for (i = 0; i < samples; i++)
  {
    out[i] = in[i] >> 8;
  }

  return new_buf;
}

static std::vector<char> u32_to_u24(const uint32_t *in, const size_t samples)
{
  size_t i;
  std::vector<char> new_buf(samples * 4);
  uint32_t *out = reinterpret_cast<uint32_t *>(new_buf.data());

  for (i = 0; i < samples; i++)
  {
    out[i] = in[i] >> 8;
  }

  return new_buf;
}

static std::vector<char> s24_to_s16(const int32_t *in, const size_t samples)
{
  size_t i;
  std::vector<char> new_buf(samples * 2);
  int16_t *out = reinterpret_cast<int16_t *>(new_buf.data());

  for (i = 0; i < samples; i++)
  {
    out[i] = in[i] >> 8;
  }

  return new_buf;
}

static std::vector<char> u24_to_u16(const uint32_t *in, const size_t samples)
{
  size_t i;
  std::vector<char> new_buf(samples * 2);
  uint16_t *out = reinterpret_cast<uint16_t *>(new_buf.data());

  for (i = 0; i < samples; i++)
  {
    out[i] = in[i] >> 8;
  }

  return new_buf;
}

/* Do the sound conversion.  buf of length size is the sample buffer to
 * convert.
 * Return the converted sound in a std::vector.
 *
 * Conversion workflow:
 *   1. Change endianness
 *   2. Change sample format (to float or target SFMT if no resampling needed)
 *   3. Resample
 *   4. Change sample format to destination SFMT
 *   5. Up/downmix channels
 */
std::vector<char> AudioConversion::process(const char *buf, const size_t size)
{
  std::vector<char> curr_sound(buf, buf + size);
  long curr_sfmt = from_params.fmt;

  if (!(curr_sfmt & SFMT_NE))
  {
    swap_endian(curr_sound.data(), curr_sound.size(), curr_sfmt);
    curr_sfmt = sfmt_set_endian(curr_sfmt, SFMT_NE);
  }

  /* Special case (optimization): 32bit -> 24bit_3 */
  if ((curr_sfmt & (SFMT_S32 | SFMT_U32)) &&
      (to_params.fmt & (SFMT_S24_3 | SFMT_U24_3)) &&
      from_params.rate == to_params.rate)
  {
    if ((curr_sfmt & SFMT_MASK_FORMAT) == SFMT_S32)
    {
      curr_sound = s32_to_s24_3(reinterpret_cast<int32_t *>(curr_sound.data()), curr_sound.size() / 4);
      curr_sfmt = sfmt_set_fmt(curr_sfmt, SFMT_S24_3);
    }
    else
    {
      curr_sound = s32_to_s24_3(reinterpret_cast<int32_t *>(curr_sound.data()), curr_sound.size() / 4);
      curr_sfmt = sfmt_set_fmt(curr_sfmt, SFMT_U24_3);
    }

    logit("Fast conversion: 32bit -> 24_3bit!");
  }

  /* Special case (optimization): if we only need to convert 32bit samples
   * to 16bit, we can do it very simply and quickly. */
  if ((curr_sfmt & (SFMT_S32 | SFMT_U32)) &&
      (to_params.fmt & (SFMT_S16 | SFMT_U16)) &&
      from_params.rate == to_params.rate)
  {
    if ((curr_sfmt & SFMT_MASK_FORMAT) == SFMT_S32)
    {
      curr_sound = s32_to_s16(reinterpret_cast<int32_t *>(curr_sound.data()), curr_sound.size() / 4);
      curr_sfmt = sfmt_set_fmt(curr_sfmt, SFMT_S16);
    }
    else
    {
      curr_sound = u32_to_u16(reinterpret_cast<uint32_t *>(curr_sound.data()), curr_sound.size() / 4);
      curr_sfmt = sfmt_set_fmt(curr_sfmt, SFMT_U16);
    }

    logit("Fast conversion: 32bit -> 16bit!");
  }

  /* Special case (optimization): 32bit to 24bit */
  if ((curr_sfmt & (SFMT_S32 | SFMT_U32)) &&
      (to_params.fmt & (SFMT_S24 | SFMT_U24)) &&
      from_params.rate == to_params.rate)
  {
    if ((curr_sfmt & SFMT_MASK_FORMAT) == SFMT_S32)
    {
      curr_sound = s32_to_s24(reinterpret_cast<int32_t *>(curr_sound.data()), curr_sound.size() / 4);
      curr_sfmt = sfmt_set_fmt(curr_sfmt, SFMT_S24);
    }
    else
    {
      curr_sound = u32_to_u24(reinterpret_cast<uint32_t *>(curr_sound.data()), curr_sound.size() / 4);
      curr_sfmt = sfmt_set_fmt(curr_sfmt, SFMT_U24);
    }

    logit("Fast conversion: 32bit -> 24bit!");
  }

  /* Special case (optimization): 24bit to 16bit */
  if ((curr_sfmt & (SFMT_S24 | SFMT_U24)) &&
      (to_params.fmt & (SFMT_S16 | SFMT_U16)) &&
      from_params.rate == to_params.rate)
  {
    if ((curr_sfmt & SFMT_MASK_FORMAT) == SFMT_S24)
    {
      curr_sound = s24_to_s16(reinterpret_cast<int32_t *>(curr_sound.data()), curr_sound.size() / 4);
      curr_sfmt = sfmt_set_fmt(curr_sfmt, SFMT_S16);
    }
    else
    {
      curr_sound = u24_to_u16(reinterpret_cast<uint32_t *>(curr_sound.data()), curr_sound.size() / 4);
      curr_sfmt = sfmt_set_fmt(curr_sfmt, SFMT_U16);
    }

    logit("Fast conversion: 24bit -> 16bit!");
  }

  /* convert to float if necessary */
  if ((from_params.rate != to_params.rate ||
       (to_params.fmt & SFMT_MASK_FORMAT) == SFMT_FLOAT ||
       !sfmt_same_bps(to_params.fmt, curr_sfmt)) &&
      (curr_sfmt & SFMT_MASK_FORMAT) != SFMT_FLOAT)
  {
    auto float_sound = fixed_to_float(curr_sound.data(), curr_sound.size(), curr_sfmt);
    curr_sound.assign(reinterpret_cast<char*>(float_sound.data()), reinterpret_cast<char*>(float_sound.data() + float_sound.size()));
    curr_sfmt = sfmt_set_fmt(curr_sfmt, SFMT_FLOAT);
  }

#ifdef HAVE_SAMPLERATE
  if (from_params.rate != to_params.rate)
  {
    auto resampled = resample_sound(reinterpret_cast<float *>(curr_sound.data()),
                                    curr_sound.size() / sizeof(float),
                                    from_params.channels);
    curr_sound.assign(reinterpret_cast<char*>(resampled.data()), reinterpret_cast<char*>(resampled.data() + resampled.size()));
  }
#endif

  if ((curr_sfmt & SFMT_MASK_FORMAT) != (to_params.fmt & SFMT_MASK_FORMAT))
  {
    if (sfmt_same_bps(curr_sfmt, to_params.fmt))
    {
      change_sign(curr_sound.data(), curr_sound.size(), &curr_sfmt);
    }
    else
    {
      assert(curr_sfmt & SFMT_FLOAT);
      curr_sound = float_to_fixed(reinterpret_cast<float *>(curr_sound.data()), curr_sound.size() / sizeof(float), to_params.fmt);
      curr_sfmt = sfmt_set_fmt(curr_sfmt, to_params.fmt);
    }
  }

  if (from_params.channels == 1 && to_params.channels == 2)
  {
    curr_sound = mono_to_stereo(curr_sound.data(), curr_sound.size(), curr_sfmt);
  }

  if (from_params.channels == 6 && to_params.channels == 2)
  {
    curr_sound = ch6_to_stereo(curr_sound.data(), curr_sound.size(), from_params.fmt);
  }

  if ((curr_sfmt & SFMT_MASK_ENDIANNESS) !=
      (to_params.fmt & SFMT_MASK_ENDIANNESS))
  {
    swap_endian(curr_sound.data(), curr_sound.size(), curr_sfmt);
    curr_sfmt = sfmt_set_endian(curr_sfmt, to_params.fmt & SFMT_MASK_ENDIANNESS);
  }

  return curr_sound;
}

// EOF
