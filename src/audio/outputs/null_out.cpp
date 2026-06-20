// src/audio/outputs/null_out.c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Fake output device - only for testing.
// Copyright (C) 2004 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>

#include "core/common.h"
#include "audio/audio.h"
#include "audio/outputs/null_out.h"

static struct sound_params params = {0, 0, 0};

static int null_open(struct sound_params *sound_params)
{
  params = *sound_params;
  return 1;
}

static void null_close() { params.rate = 0; }

static int null_play(const char *unused ATTR_UNUSED, const size_t size)
{
  xsleep(size, audio_get_bps());
  return size;
}

static int null_read_mixer() { return 100; }

static void null_set_mixer(int unused ATTR_UNUSED) {}

static int null_get_buff_fill() { return 0; }

static int null_reset() { return 1; }

static int null_init(struct output_driver_caps *caps)
{
  caps->formats = SFMT_S8 | SFMT_S16 | SFMT_S32 | SFMT_FLOAT | SFMT_NE;
  caps->min_channels = 1;
  caps->max_channels = 8;
  caps->min_rate = AUDIO_RATE_MIN;
  caps->max_rate = AUDIO_RATE_MAX;

  return 1;
}

static int null_get_rate() { return params.rate; }

static void null_toggle_mixer_channel() {}

static char *null_get_mixer_channel_name() { return xstrdup("FakeMixer"); }

class NullOutput : public AudioOutput {
public:
    int init(struct output_driver_caps *caps) override { return null_init(caps); }
    void shutdown() override {}
    int open(struct sound_params *sound_params) override { return null_open(sound_params); }
    void close() override { null_close(); }
    int play(const char *buff, const size_t size) override { return null_play(buff, size); }
    int read_mixer() override { return null_read_mixer(); }
    void set_mixer(int vol) override { null_set_mixer(vol); }
    int get_buff_fill() override { return null_get_buff_fill(); }
    int reset() override { return null_reset(); }
    int get_rate() override { return null_get_rate(); }
    void toggle_mixer_channel() override { null_toggle_mixer_channel(); }
    char* get_mixer_channel_name() override { return null_get_mixer_channel_name(); }
};

std::unique_ptr<AudioOutput> create_null_output() {
    return std::make_unique<NullOutput>();
}

// EOF
