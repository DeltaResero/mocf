// src/audio/outputs/null_out.cpp
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

class NullOutput : public AudioOutput {
private:
    struct sound_params params = {0, 0, 0};

public:
    int init(struct output_driver_caps *caps) override {
        caps->formats = SFMT_S8 | SFMT_S16 | SFMT_S32 | SFMT_FLOAT | SFMT_NE;
        caps->min_channels = 1;
        caps->max_channels = 8;
        caps->min_rate = AUDIO_RATE_MIN;
        caps->max_rate = AUDIO_RATE_MAX;
        return 1;
    }

    void shutdown() override {}

    int open(struct sound_params *sound_params) override {
        params = *sound_params;
        return 1;
    }

    void close() override { 
        params.rate = 0; 
    }

    int play(const char *unused ATTR_UNUSED, const size_t size) override {
        xsleep(size, audio_get_bps());
        return size;
    }

    int read_mixer() override { return 100; }
    void set_mixer(int unused ATTR_UNUSED) override {}
    int get_buff_fill() override { return 0; }
    int reset() override { return 1; }
    int get_rate() override { return params.rate; }
    void toggle_mixer_channel() override {}
    std::string get_mixer_channel_name() override { return "FakeMixer"; }
};

std::unique_ptr<AudioOutput> create_null_output() {
    return std::make_unique<NullOutput>();
}

// EOF
