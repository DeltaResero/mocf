// src/audio/decoders/wma/asf.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// Minimal audio-only ASF (Windows Media) reader.
//
// Scope is deliberately far narrower than FFmpeg's asfdec_f.c (~1640 lines):
// mocf only needs to find the one audio stream, hand its WAVEFORMATEX to the
// WMA decoder, and reassemble media objects from data packets. Video, DRM
// (asfcrypt), streaming/network sources and multi-stream muxing are all out of
// scope, which is what keeps this at a size that fits a 32MB target.
//
// Layout follows the ASF specification: a Header Object carrying File
// Properties and Stream Properties, then a Data Object of fixed-size packets.
// Each packet holds one or more payloads; a payload is a slice of a "media
// object", which for WMA is exactly one block_align-sized superframe.
#ifndef MOCF_WMA_ASF_H
#define MOCF_WMA_ASF_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

struct io_stream;

struct AsfAudioInfo
{
  int codec_tag = 0;        ///< 0x0160 = WMAv1, 0x0161 = WMAv2
  int channels = 0;
  int sample_rate = 0;
  int byte_rate = 0;
  int block_align = 0;
  int bits_per_sample = 0;
  int stream_number = 0;
  std::vector<uint8_t> extradata;

  int64_t play_duration_100ns = 0;  ///< File Properties play duration
  int64_t preroll_ms = 0;
  uint32_t packet_size = 0;
  uint64_t data_packet_count = 0;
  int64_t data_offset = 0;          ///< first byte of the first data packet
};

class AsfReader
{
public:
  /// Parses the header and locates the audio stream. False if this is not an
  /// ASF file, or carries no WMA v1/v2 audio stream.
  bool open(struct io_stream *io);

  /**
   * Reassembles and returns the next media object (one WMA superframe).
   * @return payload size, 0 at end of stream, negative on a broken packet.
   */
  int next_packet(uint8_t *buf, int buf_size);

  /// Repositions to @p ms. Returns false if the target is out of range.
  bool seek_ms(int64_t ms);

  const AsfAudioInfo &info() const { return info_; }
  double duration_sec() const;

  /// Why open() failed, phrased for the user. Empty if it did not.
  /// Not named error(): core/common.h defines that as a macro.
  const std::string &failure_reason() const { return error_; }

private:
  bool parse_header(uint64_t header_size, uint32_t nb_objects);
  bool parse_stream_properties(const uint8_t *p, size_t len);
  bool parse_file_properties(const uint8_t *p, size_t len);
  /// Reads one data packet and appends any payloads for our stream.
  int read_data_packet();

  struct io_stream *io_ = nullptr;
  AsfAudioInfo info_;
  std::string error_;

  uint64_t packet_index_ = 0;
  // Reassembly state for a media object split across payloads/packets.
  std::vector<uint8_t> obj_;
  uint32_t obj_expected_ = 0;
  int obj_number_ = -1;
  // Completed media objects waiting to be handed out.
  std::vector<std::vector<uint8_t>> ready_;
};

#endif // MOCF_WMA_ASF_H
