// src/audio/outputs/sndio_out.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// SNDIO sound driver for MOC by Alexander Polakov.
// Copyright (C) 2011 Alexander Polakov <polachok@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef HAVE_SNDIO_H
#include <sndio.h>
#endif

#include <cassert>

#include "core/common.h"
#include "audio/audio.h"
#include "core/log.h"
#include "audio/outputs/sndio_out.h"

#define PCT_TO_SIO(pct) ((127 * (pct) + 50) / 100)
#define SIO_TO_PCT(vol) ((100 * (vol) + 64) / 127)

static struct sio_hdl *hdl = nullptr;
static int curvol = 100;
static struct sound_params params = {0, 0, 0};

static void sndio_close();

static void volume_cb(void *unused ATTR_UNUSED, unsigned int vol)
{
  curvol = SIO_TO_PCT(vol);
}

static int sndio_init(struct output_driver_caps *caps)
{
  assert(caps != nullptr);

  caps->formats = SFMT_S8 | SFMT_U8 | SFMT_U16 | SFMT_S16 | SFMT_NE;
  caps->min_channels = 1;
  caps->max_channels = 6;
  caps->min_rate = AUDIO_RATE_MIN;
  caps->max_rate = AUDIO_RATE_MAX;

  return 1;
}

static void sndio_shutdown()
{
  if (hdl)
  {
    sndio_close();
  }
}

/* Return 0 on failure. */
static int sndio_open(struct sound_params *sound_params)
{
  struct sio_par par;

  assert(hdl == nullptr);

  if ((hdl = sio_open(nullptr, SIO_PLAY, 0)) == nullptr)
  {
    return 0;
  }

  params = *sound_params;
  sio_initpar(&par);
  /* Add volume change callback. */
  sio_onvol(hdl, volume_cb, nullptr);
  par.rate = sound_params->rate;
  par.pchan = sound_params->channels;
  par.bits =
      (((sound_params->fmt & SFMT_S8) || (sound_params->fmt & SFMT_U8)) ? 8
                                                                        : 16);
  par.le = SIO_LE_NATIVE;
  par.sig =
      (((sound_params->fmt & SFMT_S16) || (sound_params->fmt & SFMT_S8)) ? 1
                                                                         : 0);
  par.round = par.rate / 8;
  par.appbufsz = par.round * 2;
  logit("rate %d pchan %d bits %d sign %d", par.rate, par.pchan, par.bits,
        par.sig);

  if (!sio_setpar(hdl, &par) || !sio_getpar(hdl, &par) || !sio_start(hdl))
  {
    logit("Failed to set sndio parameters.");
    sio_close(hdl);
    hdl = nullptr;
    return 0;
  }
  sio_setvol(hdl, PCT_TO_SIO(curvol));

  return 1;
}

/* Return the number of bytes played, or -1 on error. */
static int sndio_play(const char *buff, const size_t size)
{
  int count;

  assert(hdl != nullptr);

  count = (int)sio_write(hdl, buff, size);
  if (!count && sio_eof(hdl))
  {
    return -1;
  }

  return count;
}

static void sndio_close()
{
  assert(hdl != nullptr);

  sio_stop(hdl);
  sio_close(hdl);
  hdl = nullptr;
}

static int sndio_read_mixer() { return curvol; }

static void sndio_set_mixer(int vol)
{
  if (hdl != nullptr)
  {
    sio_setvol(hdl, PCT_TO_SIO(vol));
  }
}

static int sndio_get_buff_fill()
{
  assert(hdl != nullptr);

  /* Since we cannot stop SNDIO playing the samples already in
   * its buffer, there will never be anything left unheard. */

  return 0;
}

static int sndio_reset()
{
  assert(hdl != nullptr);

  /* SNDIO will continue to play the samples already in its buffer
   * regardless of what we do, so there's nothing we can do. */

  return 1;
}

static int sndio_get_rate()
{
  assert(hdl != nullptr);

  return params.rate;
}

static void sndio_toggle_mixer_channel() { assert(hdl != nullptr); }

static std::string sndio_get_mixer_channel_name() { return "mocf"; }

class SndioOutput : public AudioOutput {
public:
    int init(struct output_driver_caps *caps) override { return sndio_init(caps); }
    void shutdown() override { sndio_shutdown(); }
    int open(struct sound_params *sound_params) override { return sndio_open(sound_params); }
    void close() override { sndio_close(); }
    int play(const char *buff, const size_t size) override { return sndio_play(buff, size); }
    int read_mixer() override { return sndio_read_mixer(); }
    void set_mixer(int vol) override { sndio_set_mixer(vol); }
    int get_buff_fill() override { return sndio_get_buff_fill(); }
    int reset() override { return sndio_reset(); }
    int get_rate() override { return sndio_get_rate(); }
    void toggle_mixer_channel() override { sndio_toggle_mixer_channel(); }
    std::string get_mixer_channel_name() override { return sndio_get_mixer_channel_name(); }
};

std::unique_ptr<AudioOutput> create_sndio_output() {
    return std::make_unique<SndioOutput>();
}

// EOF
