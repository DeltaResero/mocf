// src/audio/decoders/wma/asf.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "asf.h"

#include "core/common.h"
#include "core/log.h"
#include "io/io.h"

#include <cstring>

namespace {

// ASF GUIDs, little-endian on disk exactly as written here.
const uint8_t GUID_HEADER[16] = {
  0x30,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
const uint8_t GUID_DATA[16] = {
  0x36,0x26,0xB2,0x75,0x8E,0x66,0xCF,0x11,0xA6,0xD9,0x00,0xAA,0x00,0x62,0xCE,0x6C};
const uint8_t GUID_FILE_PROPERTIES[16] = {
  0xA1,0xDC,0xAB,0x8C,0x47,0xA9,0xCF,0x11,0x8E,0xE4,0x00,0xC0,0x0C,0x20,0x53,0x65};
const uint8_t GUID_STREAM_PROPERTIES[16] = {
  0x91,0x07,0xDC,0xB7,0xB7,0xA9,0xCF,0x11,0x8E,0xE6,0x00,0xC0,0x0C,0x20,0x53,0x65};
const uint8_t GUID_AUDIO_MEDIA[16] = {
  0x40,0x9E,0x69,0xF8,0x4D,0x5B,0xCF,0x11,0xA8,0xFD,0x00,0x80,0x5F,0x5C,0x44,0x2B};

inline bool guid_eq(const uint8_t *a, const uint8_t *b)
{
  return memcmp(a, b, 16) == 0;
}

/// Reads exactly @p n bytes; a short read is a failure, not a partial success.
inline bool io_read_exact(struct io_stream *s, void *buf, size_t n)
{
  return io_read(s, buf, n) == static_cast<ssize_t>(n);
}

inline bool io_seek_abs(struct io_stream *s, int64_t pos)
{
  return io_seek(s, static_cast<off_t>(pos), SEEK_SET) != -1;
}

inline uint16_t rl16(const uint8_t *p) { return p[0] | (p[1] << 8); }
inline uint32_t rl32(const uint8_t *p)
{
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t rl64(const uint8_t *p)
{
  return static_cast<uint64_t>(rl32(p)) | (static_cast<uint64_t>(rl32(p + 4)) << 32);
}

/**
 * Reads one of ASF's variable-width fields. The two-bit type selects the width:
 * 0 means the field is absent and reads as zero.
 */
inline bool read_var(const uint8_t *p, size_t len, size_t &off, int type,
                     uint32_t &out)
{
  switch (type)
  {
    case 0: out = 0; return true;
    case 1: if (off + 1 > len) return false; out = p[off]; off += 1; return true;
    case 2: if (off + 2 > len) return false; out = rl16(p + off); off += 2; return true;
    case 3: if (off + 4 > len) return false; out = rl32(p + off); off += 4; return true;
    default: return false;
  }
}

// A media object larger than this is rejected rather than trusted: WMA's
// block_align is a few KB, and the field is attacker-controlled.
const uint32_t ASF_MAX_MEDIA_OBJECT = 1 << 20;

// Guards against a header that declares an implausible packet size.
const uint32_t ASF_MAX_PACKET_SIZE = 1 << 16;

} // namespace

bool AsfReader::open(struct io_stream *io)
{
  io_ = io;

  uint8_t hdr[30];
  if (!io_seek_abs(io_, 0) || !io_read_exact(io_, hdr, sizeof(hdr)))
    return false;
  if (!guid_eq(hdr, GUID_HEADER))
    return false;

  const uint64_t header_size = rl64(hdr + 16);
  const uint32_t nb_objects = rl32(hdr + 24);
  if (header_size < 30 || header_size > (64u << 20))
    return false;

  if (!parse_header(header_size, nb_objects))
    return false;

  if (!info_.codec_tag || !info_.channels || !info_.sample_rate ||
      !info_.block_align)
  {
    logit("asf: no usable WMA audio stream");
    return false;
  }
  if (info_.packet_size == 0 || info_.packet_size > ASF_MAX_PACKET_SIZE)
  {
    logit("asf: bad packet size %u", info_.packet_size);
    return false;
  }

  packet_index_ = 0;
  return io_seek_abs(io_, info_.data_offset);
}

bool AsfReader::parse_header(uint64_t header_size, uint32_t nb_objects)
{
  // The header is small (typically < 64KB); read it whole rather than seeking
  // around it object by object.
  if (header_size > (8u << 20))
    return false;
  std::vector<uint8_t> buf(static_cast<size_t>(header_size) - 30);
  if (!buf.empty() && !io_read_exact(io_, buf.data(), buf.size()))
    return false;

  size_t p = 0;
  for (uint32_t i = 0; i < nb_objects && p + 24 <= buf.size(); i++)
  {
    const uint8_t *guid = buf.data() + p;
    const uint64_t osize = rl64(buf.data() + p + 16);
    if (osize < 24 || p + osize > buf.size())
      break;

    if (guid_eq(guid, GUID_FILE_PROPERTIES))
    {
      if (!parse_file_properties(buf.data() + p, static_cast<size_t>(osize)))
        return false;
    }
    else if (guid_eq(guid, GUID_STREAM_PROPERTIES))
    {
      // Ignore failures: a file may carry streams we do not care about.
      parse_stream_properties(buf.data() + p, static_cast<size_t>(osize));
    }
    p += static_cast<size_t>(osize);
  }

  // The Data Object follows the Header Object.
  uint8_t d[50];
  if (!io_seek_abs(io_, static_cast<int64_t>(header_size)) || !io_read_exact(io_, d, sizeof(d)))
    return false;
  if (!guid_eq(d, GUID_DATA))
  {
    logit("asf: data object not where the header says");
    return false;
  }
  info_.data_packet_count = rl64(d + 40);
  // GUID(16) + size(8) + File ID(16) + Total Data Packets(8) + Reserved(2).
  info_.data_offset = static_cast<int64_t>(header_size) + 50;
  return true;
}

bool AsfReader::parse_file_properties(const uint8_t *p, size_t len)
{
  // GUID(16) size(8) FileID(16) FileSize(8) CreationDate(8) DataPackets(8)
  // PlayDuration(8) SendDuration(8) Preroll(8) Flags(4) MinPacket(4)
  // MaxPacket(4) MaxBitrate(4)
  if (len < 104)
    return false;
  info_.play_duration_100ns = static_cast<int64_t>(rl64(p + 64));
  info_.preroll_ms = static_cast<int64_t>(rl64(p + 80));
  const uint32_t min_packet = rl32(p + 92);
  const uint32_t max_packet = rl32(p + 96);
  // Only fixed-size packets are supported; that covers every file WMA ships in.
  if (min_packet != max_packet)
  {
    error_ = "Variable-size ASF packets are not supported";
    logit("asf: variable packet size (%u..%u) unsupported", min_packet,
          max_packet);
    return false;
  }
  info_.packet_size = min_packet;
  return true;
}

bool AsfReader::parse_stream_properties(const uint8_t *p, size_t len)
{
  // GUID(16) size(8) StreamType(16) ErrorCorrType(16) TimeOffset(8)
  // TypeSpecificLen(4) ErrorCorrLen(4) Flags(2) Reserved(4) then
  // type-specific data (a WAVEFORMATEX for audio).
  if (len < 78)
    return false;
  if (!guid_eq(p + 24, GUID_AUDIO_MEDIA))
    return false;  // not audio
  if (info_.codec_tag)
    return false;  // already have an audio stream; ignore the rest

  const uint32_t ts_len = rl32(p + 64);
  const uint16_t flags = rl16(p + 72);
  const size_t wfx = 78;
  if (ts_len < 18 || wfx + ts_len > len)
    return false;

  const uint16_t tag = rl16(p + wfx);
  if (tag != 0x0160 && tag != 0x0161)
  {
    char buf[96];
    snprintf(buf, sizeof(buf),
             "Unsupported WMA variant (codec 0x%04X); only WMA v1 and v2 are "
             "supported", tag);
    error_ = buf;
    logit("asf: %s", buf);
    return false;
  }

  info_.codec_tag       = tag;
  info_.channels        = rl16(p + wfx + 2);
  info_.sample_rate     = static_cast<int>(rl32(p + wfx + 4));
  info_.byte_rate       = static_cast<int>(rl32(p + wfx + 8));
  info_.block_align     = rl16(p + wfx + 12);
  info_.bits_per_sample = rl16(p + wfx + 14);
  info_.stream_number   = flags & 0x7F;

  const uint16_t cb = rl16(p + wfx + 16);
  if (cb && wfx + 18 + cb <= len)
    info_.extradata.assign(p + wfx + 18, p + wfx + 18 + cb);

  // WMA v1/v2 is stereo at most; anything else is a malformed or mislabelled
  // stream and would overrun the decoder's fixed channel arrays.
  if (info_.channels < 1 || info_.channels > 2)
  {
    char buf[80];
    snprintf(buf, sizeof(buf), "Invalid channel count (%d) for WMA v1/v2",
             info_.channels);
    error_ = buf;
    logit("asf: %s", buf);
    info_.codec_tag = 0;
    return false;
  }
  if (info_.block_align <= 0)
  {
    info_.codec_tag = 0;
    return false;
  }
  return true;
}

int AsfReader::read_data_packet()
{
  const uint32_t psize = info_.packet_size;
  if (info_.data_packet_count && packet_index_ >= info_.data_packet_count)
    return 0;

  std::vector<uint8_t> pkt(psize);
  if (!io_read_exact(io_, pkt.data(), psize))
    return 0;  // end of stream
  packet_index_++;

  const uint8_t *p = pkt.data();
  const size_t len = psize;
  size_t off = 0;

  uint8_t ltf = p[off++];
  if (ltf & 0x80)
  {
    // Error Correction Data present: flags byte, then that many bytes.
    const uint8_t ec_flags = p[0];
    const size_t ec_len = ec_flags & 0x0F;
    off = 1 + ec_len;
    if (off + 2 > len)
      return -1;
    ltf = p[off++];
  }

  if (off >= len)
    return -1;
  const uint8_t pf = p[off++];

  uint32_t packet_len = 0, sequence = 0, padding = 0;
  if (!read_var(p, len, off, (ltf >> 5) & 3, packet_len)) return -1;
  if (!read_var(p, len, off, (ltf >> 1) & 3, sequence))   return -1;
  if (!read_var(p, len, off, (ltf >> 3) & 3, padding))    return -1;

  if (off + 6 > len)
    return -1;
  off += 6;  // Send Time (4) + Duration (2)

  // When Packet Length is absent the packet occupies the whole fixed size.
  size_t pkt_end = packet_len ? packet_len : len;
  if (pkt_end > len)
    pkt_end = len;
  if (padding >= pkt_end)
    return -1;
  pkt_end -= padding;

  int nb_payloads = 1;
  int payload_len_type = 0;
  const bool multiple = (ltf & 1) != 0;
  if (multiple)
  {
    if (off >= len) return -1;
    const uint8_t pfl = p[off++];
    nb_payloads = pfl & 0x3F;
    payload_len_type = (pfl >> 6) & 3;
  }

  for (int i = 0; i < nb_payloads; i++)
  {
    if (off >= pkt_end)
      break;

    const uint8_t sn = p[off++];
    const int stream = sn & 0x7F;

    uint32_t media_obj_num = 0, obj_offset = 0, repl_len = 0;
    if (!read_var(p, pkt_end, off, (pf >> 4) & 3, media_obj_num)) return -1;
    if (!read_var(p, pkt_end, off, (pf >> 2) & 3, obj_offset))    return -1;
    if (!read_var(p, pkt_end, off, pf & 3, repl_len))             return -1;

    // repl_len == 1 marks a "compressed payload": obj_offset is really a
    // presentation-time delta and the payload holds several whole media
    // objects, each prefixed by a length byte.
    const bool compressed = (repl_len == 1);
    uint32_t media_obj_size = 0;
    if (!compressed)
    {
      if (repl_len >= 8)
        media_obj_size = rl32(p + off);
      if (off + repl_len > pkt_end) return -1;
      off += repl_len;
    }
    else
    {
      if (off + 1 > pkt_end) return -1;
      off += 1;  // presentation time delta
    }

    uint32_t payload_len;
    if (multiple)
    {
      if (!read_var(p, pkt_end, off, payload_len_type, payload_len)) return -1;
    }
    else
    {
      payload_len = static_cast<uint32_t>(pkt_end - off);
    }
    if (off + payload_len > pkt_end)
      return -1;

    const uint8_t *data = p + off;
    off += payload_len;

    if (stream != info_.stream_number)
      continue;  // another stream in the same packet

    if (compressed)
    {
      size_t q = 0;
      while (q < payload_len)
      {
        const uint32_t sub = data[q++];
        if (q + sub > payload_len)
          return -1;
        ready_.emplace_back(data + q, data + q + sub);
        q += sub;
      }
      continue;
    }

    if (media_obj_size > ASF_MAX_MEDIA_OBJECT)
      return -1;

    // A payload that is a whole object on its own: the common WMA case.
    if (obj_offset == 0 && media_obj_size == payload_len)
    {
      ready_.emplace_back(data, data + payload_len);
      obj_.clear();
      obj_expected_ = 0;
      obj_number_ = -1;
      continue;
    }

    // Otherwise the object is split across payloads; reassemble by object
    // number and offset.
    if (obj_offset == 0 || static_cast<int>(media_obj_num) != obj_number_)
    {
      obj_.clear();
      obj_number_ = static_cast<int>(media_obj_num);
      obj_expected_ = media_obj_size;
    }
    if (obj_offset != obj_.size())
    {
      // A gap: the stream was cut or seeked into. Drop the partial object
      // rather than splice unrelated bytes together.
      obj_.clear();
      obj_number_ = -1;
      obj_expected_ = 0;
      continue;
    }
    if (obj_expected_ && obj_.size() + payload_len > obj_expected_)
      return -1;
    obj_.insert(obj_.end(), data, data + payload_len);
    if (obj_expected_ && obj_.size() == obj_expected_)
    {
      ready_.push_back(obj_);
      obj_.clear();
      obj_number_ = -1;
      obj_expected_ = 0;
    }
  }
  return 1;
}

int AsfReader::next_packet(uint8_t *buf, int buf_size)
{
  while (ready_.empty())
  {
    const int r = read_data_packet();
    if (r == 0)
      return 0;  // end of stream
    if (r < 0)
    {
      // Skip the broken packet and keep going: a damaged packet mid-file
      // should cost one superframe, not the rest of the track.
      logit("asf: skipping malformed packet %llu",
              static_cast<unsigned long long>(packet_index_));
      continue;
    }
  }

  std::vector<uint8_t> &front = ready_.front();
  const int n = static_cast<int>(front.size());
  if (n > buf_size)
    return -1;
  memcpy(buf, front.data(), n);
  ready_.erase(ready_.begin());
  return n;
}

double AsfReader::duration_sec() const
{
  // Play duration counts from the start of preroll; subtract it so the reported
  // length matches what actually plays.
  double d = static_cast<double>(info_.play_duration_100ns) / 1e7;
  d -= static_cast<double>(info_.preroll_ms) / 1e3;
  return d > 0.0 ? d : 0.0;
}

bool AsfReader::seek_ms(int64_t ms)
{
  const double dur = duration_sec();
  if (ms < 0)
    ms = 0;
  if (dur > 0.0 && static_cast<double>(ms) / 1e3 > dur)
    return false;

  // No Simple Index is required for audio-only ASF, and the packets are fixed
  // size with an essentially constant bitrate, so seek proportionally and let
  // the decoder resynchronise. Being a fraction of a second out is acceptable;
  // reading a partial packet is not.
  uint64_t target = 0;
  if (dur > 0.0 && info_.data_packet_count)
    target = static_cast<uint64_t>((static_cast<double>(ms) / 1e3 / dur) *
                                   static_cast<double>(info_.data_packet_count));
  if (info_.data_packet_count && target >= info_.data_packet_count)
    target = info_.data_packet_count - 1;

  const int64_t pos =
      info_.data_offset + static_cast<int64_t>(target) * info_.packet_size;
  if (!io_seek_abs(io_, pos))
    return false;

  packet_index_ = target;
  obj_.clear();
  obj_expected_ = 0;
  obj_number_ = -1;
  ready_.clear();
  return true;
}
