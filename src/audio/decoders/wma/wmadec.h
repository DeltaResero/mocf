// src/audio/decoders/wma/wmadec.h
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// mocf - Music on Console Framebuffer
//
// Interface to the WMA v1/v2 decoder core ported from FFmpeg (LGPLv2.1+).
// Everything FFmpeg-derived lives behind this header; the plugin and the ASF
// reader see only the handful of declarations below. See wmadec.cpp for the
// full list of upstream files and their copyright holders.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License as published by the Free
// Software Foundation; either version 2.1 of the License, or (at your option)
// any later version.

#ifndef MOCF_WMA_WMADEC_H
#define MOCF_WMA_WMADEC_H

#include <cstdint>
#include <memory>

/* Stream parameters, as carried by the ASF Stream Properties object. */
struct WmaStreamParams
{
  int version = 0;            ///< 1 for WMA v1 (0x0160), 2 for WMA v2 (0x0161)
  int channels = 0;
  int sample_rate = 0;
  int block_align = 0;        ///< coded superframe size; the decoder needs this
  int bit_rate = 0;
  const uint8_t *extradata = nullptr;
  int extradata_size = 0;
};

struct WmaDecoderCore;

struct WmaCoreDeleter
{
  void operator()(WmaDecoderCore *core) const;
};

using unique_wma_core = std::unique_ptr<WmaDecoderCore, WmaCoreDeleter>;

/// Creates and initialises a decoder. Returns nullptr if the stream is not
/// decodable (unsupported channel count, missing block_align, and so on).
unique_wma_core wma_core_create(const WmaStreamParams &params);

/**
 * Decodes one coded superframe.
 *
 * @param pkt        superframe bytes, or nullptr with @p size 0 to drain the
 *                   trailing MDCT overlap once the stream has ended
 * @param planes     receives per-channel pointers into decoder-owned memory,
 *                   valid until the next call
 * @param nb_samples receives the sample count per channel
 * @return 0 on success (with @p nb_samples possibly 0), negative on a corrupt
 *         superframe. A negative return is recoverable: the next call may
 *         succeed.
 */
int wma_core_decode(WmaDecoderCore *core, const uint8_t *pkt, int size,
                    float *const **planes, int *nb_samples);

/// Drops inter-frame state. Must be called after seeking.
void wma_core_flush(WmaDecoderCore *core);

/// Samples per frame; the priming to discard at start of stream is twice this.
int wma_core_frame_len(const WmaDecoderCore *core);

#endif // MOCF_WMA_WMADEC_H
