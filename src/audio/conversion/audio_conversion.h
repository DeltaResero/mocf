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
#include <vector>
#include <stdexcept>

#ifdef HAVE_SAMPLERATE
#include <samplerate.h>
#endif

#include "audio/audio.h"

class AudioConversionException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class AudioConversion {
public:
    AudioConversion(const sound_params& from, const sound_params& to);
    ~AudioConversion();

    std::vector<char> process(const char *buf, size_t size);

private:
    sound_params from_params;
    sound_params to_params;

#ifdef HAVE_SAMPLERATE
    SRC_STATE *src_state = nullptr;
    std::vector<float> resample_buf;

    std::vector<float> resample_sound(const float *buf, const size_t samples, const int nchannels);
#endif
};

#endif

// EOF
