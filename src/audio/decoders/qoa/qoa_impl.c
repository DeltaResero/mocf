// src/audio/decoders/qoa/qoa_impl.c
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
//
// Compiles the QOA decoder (qoa.h - see that file for its own
// copyright/license and provenance) as C. The reference source
// assigns malloc()'s result straight to typed pointers, legal in C
// but not C++; building it here lets qoa.cpp include only its
// extern "C" declarations.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#define QOA_IMPLEMENTATION
#include "qoa.h"
