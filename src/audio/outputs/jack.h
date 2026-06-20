// src/audio/outputs/jack.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef JACK_H
#define JACK_H

#include <memory>
class AudioOutput;

std::unique_ptr<AudioOutput> create_jack_output();

#endif

// EOF
