// src/audio/decoders/mlp/mlpdec.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// Interface to the MLP/TrueHD decoder core ported from FFmpeg n4.4.8
// (LGPLv2.1+). Everything FFmpeg-derived lives behind this header; the plugin
// sees only the declarations below. See mlpdec.cpp for the full list of
// upstream files and their copyright holders.
//
// Distributed in mocf under the GNU GPL version 3 or later, as permitted
// by section 3 of upstream's LGPL version 2.1.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.

#ifndef MOCF_MLP_MLPDEC_H
#define MOCF_MLP_MLPDEC_H

#include <cstdint>
#include <memory>

struct MlpDecoderCore;

struct MlpCoreDeleter
{
  void operator()(MlpDecoderCore *core) const;
};

using unique_mlp_core = std::unique_ptr<MlpDecoderCore, MlpCoreDeleter>;

/// Front left | front right, as an AV_CH_LAYOUT_STEREO-compatible mask. The
/// plugin passes this to mlp_core_create() so that TrueHD's hierarchical
/// substreams collapse to their native 2-channel presentation.
constexpr uint64_t MLP_LAYOUT_STEREO = 0x1 | 0x2;

/**
 * Creates a decoder.
 *
 * @param is_truehd  non-zero for TrueHD (major sync 0xba), zero for MLP
 *        (0xbb). The two share this decoder but differ in channel, substream
 *        and matrix limits, so the caller must say which it probed; a major
 *        sync announcing the other type is then rejected. Use
 *        mlp_probe_is_truehd() on the first access unit to determine it.
 * @param request_channel_layout when a substream's channel layout is a
 *        superset of this mask, decoding stops at that substream and the
 *        remaining ones are skipped. TrueHD carries a 2-channel presentation
 *        in substream 0 and adds 5.1 and 7.1 in later substreams, so passing
 *        MLP_LAYOUT_STEREO yields the mastered stereo downmix while skipping
 *        the surround substreams entirely. Pass 0 to decode every substream.
 * @return nullptr on allocation failure.
 */
unique_mlp_core mlp_core_create(int is_truehd, uint64_t request_channel_layout);

/**
 * Identifies the format of an access unit carrying a major sync.
 *
 * @return 1 for TrueHD, 0 for MLP, negative if @p buf does not start with an
 *         access unit whose major sync word is recognised.
 */
int mlp_probe_is_truehd(const uint8_t *buf, int size);

/**
 * Decodes one access unit.
 *
 * Stream properties are not known until the first major sync has been parsed,
 * so mlp_core_channels() and friends only return meaningful values once
 * mlp_core_params_valid() is true.
 *
 * @param buf        access unit bytes, including the 4-byte length/check
 *                   prefix
 * @param out        receives a pointer to interleaved PCM owned by the
 *                   decoder, valid until the next call. Samples are int32_t
 *                   when mlp_core_is32() is true, int16_t otherwise. Set to
 *                   nullptr when the access unit produced no output.
 * @param nb_samples receives the sample count per channel
 * @return bytes consumed on success, negative on a corrupt access unit. A
 *         negative return is recoverable: the next call may succeed.
 */
int mlp_core_decode(MlpDecoderCore *core, const uint8_t *buf, int size,
                    const void **out, int *nb_samples);

/// Drops inter-frame state. Must be called after seeking.
void mlp_core_flush(MlpDecoderCore *core);

int mlp_core_channels(const MlpDecoderCore *core);
int mlp_core_sample_rate(const MlpDecoderCore *core);
int mlp_core_bits_per_sample(const MlpDecoderCore *core);
/// Non-zero when output samples are int32_t (24 significant bits).
int mlp_core_is32(const MlpDecoderCore *core);
/// Non-zero once a major sync has been parsed and the fields above are valid.
int mlp_core_params_valid(const MlpDecoderCore *core);
/// PCM samples per access unit. Constant for a given stream.
int mlp_core_access_unit_size(const MlpDecoderCore *core);

#endif // MOCF_MLP_MLPDEC_H
