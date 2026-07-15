// src/audio/decoders/tta/tta.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
// Structure of this plugin is an adaption of the qoa plugin.
// TTA (The Lossless True Audio) decoder ported from FFmpeg (LGPLv2.1+)
// Copyright (c) 2006 Alex Beregszaszi
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define DEBUG

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"

namespace {

constexpr int TTA_FORMAT_SIMPLE = 1;
constexpr int TTA_FORMAT_ENCRYPTED = 2;

/* Size of the fixed TTA1 header: 18 bytes of fields plus its CRC32. */
constexpr int TTA_HEADER_SIZE = 22;

/* Matches get_bits.h's cache width. The decode loop refuses any Rice
 * parameter above this, so it bounds a single get_bits() request. */
constexpr int MIN_CACHE_BITS = 25;

/* Ceiling on the decoded-frame buffer; see where it is enforced in
 * tta_read_header() for why FFmpeg's own guard is not enough here. */
constexpr int64_t MAX_DECODE_BUFFER_BYTES = 8 * 1024 * 1024;

// ---------------------------------------------------------------------
// Minimal LSB-first bit reader, replacing FFmpeg's GetBitContext in its
// BITSTREAM_READER_LE mode (tta.c defines that before including
// get_bits.h). Bit n of the stream is bit (n & 7) of byte (n >> 3), and
// the first bit read becomes the LSB of the returned value.
//
// Field types deliberately mirror FFmpeg's (int index/size_in_bits, so
// get_bits_left() returns int): the decode loop below compares that
// against an unsigned Rice parameter, and the resulting conversion is
// load-bearing. See the comment at that comparison.
// ---------------------------------------------------------------------
struct BitReaderLE
{
  const uint8_t *buf = nullptr;
  int size_in_bits = 0;
  int index = 0;

  void init(const uint8_t *b, int size_bytes)
  {
    buf = b;
    size_in_bits = size_bytes * 8;
    index = 0;
  }

  int get_bits1()
  {
    if (index >= size_in_bits)
    {
      index++;
      return 0;
    }
    int bit = (buf[index >> 3] >> (index & 7)) & 1;
    index++;
    return bit;
  }

  /* n must be > 0 and <= MIN_CACHE_BITS; callers guard both. */
  unsigned int get_bits(int n)
  {
    unsigned int v = 0;
    for (int i = 0; i < n; i++)
    {
      v |= static_cast<unsigned int>(get_bits1()) << i;
    }
    return v;
  }

  int get_bits_left() const { return size_in_bits - index; }

  void skip_bits_long(int n) { index += n; }

  /* align_get_bits(): advance to the next byte boundary. */
  void align()
  {
    int n = (-index) & 7;
    if (n) index += n;
  }
};

/* Counts bits != stop until a bit == stop is read, or len is reached -
 * same semantics as FFmpeg's unary.h get_unary(). */
static int get_unary(BitReaderLE &gb, int stop, int len)
{
  int i;
  for (i = 0; i < len && gb.get_bits1() != stop; i++)
  {
  }
  return i;
}

// ---------------------------------------------------------------------
// Per-channel decoder state (ttadata.h)
// ---------------------------------------------------------------------
constexpr int MAX_ORDER = 16;

struct TTAFilter
{
  int32_t shift, round, error;
  int32_t qm[MAX_ORDER];
  int32_t dx[MAX_ORDER];
  int32_t dl[MAX_ORDER];
};

struct TTARice
{
  uint32_t k0, k1, sum0, sum1;
};

struct TTAChannel
{
  int32_t predictor;
  TTAFilter filter;
  TTARice rice;
};

// ---------------------------------------------------------------------
// Tables (ttadata.c). tta_shift_16 is deliberately the same array
// offset by 4; the tail saturates so the adaptive Rice parameter stops
// climbing rather than running off the end.
// ---------------------------------------------------------------------
const uint32_t tta_shift_1[] = {
    0x00000001, 0x00000002, 0x00000004, 0x00000008,
    0x00000010, 0x00000020, 0x00000040, 0x00000080,
    0x00000100, 0x00000200, 0x00000400, 0x00000800,
    0x00001000, 0x00002000, 0x00004000, 0x00008000,
    0x00010000, 0x00020000, 0x00040000, 0x00080000,
    0x00100000, 0x00200000, 0x00400000, 0x00800000,
    0x01000000, 0x02000000, 0x04000000, 0x08000000,
    0x10000000, 0x20000000, 0x40000000, 0x80000000,
    0x80000000, 0x80000000, 0x80000000, 0x80000000,
    0x80000000, 0x80000000, 0x80000000, 0x80000000,
    0xFFFFFFFF};

const uint32_t *const tta_shift_16 = tta_shift_1 + 4;

/* Indexed by bytes-per-sample - 1. */
const uint8_t tta_filter_configs[] = {10, 9, 10, 12};

static void tta_rice_init(TTARice *c, uint32_t k0, uint32_t k1)
{
  c->k0 = k0;
  c->k1 = k1;
  c->sum0 = tta_shift_16[k0];
  c->sum1 = tta_shift_16[k1];
}

static void tta_filter_init(TTAFilter *c, int32_t shift)
{
  memset(c, 0, sizeof(TTAFilter));
  c->shift = shift;
  c->round = static_cast<int32_t>(tta_shift_1[shift - 1]);
}

// ---------------------------------------------------------------------
// The adaptive hybrid filter (ttadsp.c's tta_filter_process_c). The
// unsigned aliasing of qm and the unsigned wrap in the dl updates are
// intentional and part of the format's arithmetic - kept verbatim.
// ---------------------------------------------------------------------
static void tta_filter_process(int32_t *qmi, int32_t *dx, int32_t *dl,
                               int32_t *error, int32_t *in, int32_t shift,
                               int32_t round)
{
  uint32_t *qm = reinterpret_cast<uint32_t *>(qmi);

  if (*error < 0)
  {
    qm[0] -= static_cast<uint32_t>(dx[0]); qm[1] -= static_cast<uint32_t>(dx[1]);
    qm[2] -= static_cast<uint32_t>(dx[2]); qm[3] -= static_cast<uint32_t>(dx[3]);
    qm[4] -= static_cast<uint32_t>(dx[4]); qm[5] -= static_cast<uint32_t>(dx[5]);
    qm[6] -= static_cast<uint32_t>(dx[6]); qm[7] -= static_cast<uint32_t>(dx[7]);
  }
  else if (*error > 0)
  {
    qm[0] += static_cast<uint32_t>(dx[0]); qm[1] += static_cast<uint32_t>(dx[1]);
    qm[2] += static_cast<uint32_t>(dx[2]); qm[3] += static_cast<uint32_t>(dx[3]);
    qm[4] += static_cast<uint32_t>(dx[4]); qm[5] += static_cast<uint32_t>(dx[5]);
    qm[6] += static_cast<uint32_t>(dx[6]); qm[7] += static_cast<uint32_t>(dx[7]);
  }

  uint32_t acc = static_cast<uint32_t>(round);
  acc += static_cast<uint32_t>(dl[0]) * qm[0] + static_cast<uint32_t>(dl[1]) * qm[1] +
         static_cast<uint32_t>(dl[2]) * qm[2] + static_cast<uint32_t>(dl[3]) * qm[3] +
         static_cast<uint32_t>(dl[4]) * qm[4] + static_cast<uint32_t>(dl[5]) * qm[5] +
         static_cast<uint32_t>(dl[6]) * qm[6] + static_cast<uint32_t>(dl[7]) * qm[7];
  round = static_cast<int32_t>(acc);

  dx[0] = dx[1]; dx[1] = dx[2]; dx[2] = dx[3]; dx[3] = dx[4];
  dl[0] = dl[1]; dl[1] = dl[2]; dl[2] = dl[3]; dl[3] = dl[4];

  dx[4] = ((dl[4] >> 30) | 1);
  dx[5] = ((dl[5] >> 30) | 2) & ~1;
  dx[6] = ((dl[6] >> 30) | 2) & ~1;
  dx[7] = ((dl[7] >> 30) | 4) & ~3;

  *error = *in;
  *in += (round >> shift);

  dl[4] = static_cast<int32_t>(-static_cast<uint32_t>(dl[5]));
  dl[5] = static_cast<int32_t>(-static_cast<uint32_t>(dl[6]));
  dl[6] = static_cast<int32_t>(static_cast<uint32_t>(*in) - static_cast<uint32_t>(dl[7]));
  dl[7] = *in;
  dl[5] = static_cast<int32_t>(static_cast<uint32_t>(dl[5]) + static_cast<uint32_t>(dl[6]));
  dl[4] = static_cast<int32_t>(static_cast<uint32_t>(dl[4]) + static_cast<uint32_t>(dl[5]));
}

/* Fixed-order prediction (tta.c's PRED macro). */
static inline int32_t tta_pred(int32_t x, int k)
{
  return static_cast<int32_t>(((static_cast<uint64_t>(x) << k) -
                               static_cast<uint64_t>(x)) >> k);
}

// ---------------------------------------------------------------------
// Plugin state
// ---------------------------------------------------------------------
struct tta_data
{
  unique_io_stream io_stream;

  // Header fields (tta.c's TTAContext / libavformat/tta.c's header)
  int format = 0;
  int channels = 0;
  int bits_per_sample = 0; /* as stored in the header, in bits */
  int bps = 0;             /* bytes per sample: (bits + 7) / 8 */
  unsigned int samplerate = 0;
  unsigned int data_length = 0; /* total sample-frames in the stream */
  int frame_length = 0;
  int last_frame_length = 0;

  /* Only the compressed size of each frame is kept. Frame positions are
   * a prefix sum of these, recomputed on the rare seek, which costs one
   * pass over a small array and saves 12 bytes per frame against
   * storing an offset too - the seek table dominates this decoder's
   * per-file footprint on long recordings. */
  std::vector<uint32_t> frame_sizes;
  int64_t first_frame_pos = 0;
  uint32_t currentframe = 0;

  /* One compressed frame, read off the stream. Sized once at open to
   * the largest entry in the seek table. */
  std::vector<uint8_t> packet;

  /* One decoded frame, drained across decode() calls. TTA decodes a
   * frame all-or-nothing, so this cannot be made incremental. */
  std::vector<int32_t> decode_buffer;
  int frame_samples = 0; /* valid sample-frames in decode_buffer */
  int buffer_pos = 0;    /* next sample-frame to hand out */
  int seek_skip = 0;     /* sample-frames to drop from the next frame */

  std::vector<TTAChannel> ch_ctx;

  int64_t file_size = -1;
  int duration = -1;    /* seconds */
  int avg_bitrate = -1; /* kbps */

  bool eof = false;
  struct decoder_error error;
  bool ok = false;
};

/* Skip an ID3v2 tag if one is present, leaving the stream positioned at
 * the TTA1 signature. FFmpeg's demuxer gets this for free via
 * FF_INFMT_FLAG_ID3V2_AUTO; without it, every tagged file - which is
 * most of them in the wild - would fail to open here. Trailing ID3v1 and
 * APE tags need no handling: the seek table bounds the audio, so they
 * are never read. */
static void tta_skip_id3v2(struct io_stream *pb)
{
  uint8_t hdr[10];

  if (io_read(pb, hdr, sizeof(hdr)) != static_cast<ssize_t>(sizeof(hdr)) ||
      memcmp(hdr, "ID3", 3) != 0)
  {
    io_seek(pb, 0, SEEK_SET);
    return;
  }

  /* Size is 28 bits, seven per byte, and excludes the 10-byte header. */
  int64_t size = (static_cast<int64_t>(hdr[6] & 0x7f) << 21) |
                 (static_cast<int64_t>(hdr[7] & 0x7f) << 14) |
                 (static_cast<int64_t>(hdr[8] & 0x7f) << 7) |
                 static_cast<int64_t>(hdr[9] & 0x7f);
  size += 10;
  if (hdr[5] & 0x10) size += 10; /* footer present */

  io_seek(pb, size, SEEK_SET);
}

static uint16_t rl16(const uint8_t *b)
{
  return static_cast<uint16_t>(b[0] | (b[1] << 8));
}

static uint32_t rl32(const uint8_t *b)
{
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
}

/* Parse the TTA1 header and seek table. Leaves the stream positioned at
 * the first frame. Returns false if this is not a TTA file we can play. */
static bool tta_read_header(struct tta_data *data)
{
  struct io_stream *pb = data->io_stream.get();

  data->file_size = io_file_size(pb);

  tta_skip_id3v2(pb);

  uint8_t header[TTA_HEADER_SIZE];
  if (io_read(pb, header, sizeof(header)) != static_cast<ssize_t>(sizeof(header)))
  {
    return false;
  }
  if (memcmp(header, "TTA1", 4) != 0) return false;

  data->format = rl16(header + 4);
  data->channels = rl16(header + 6);
  data->bits_per_sample = rl16(header + 8);
  data->samplerate = rl32(header + 10);
  data->data_length = rl32(header + 14);
  /* header + 18 is the header CRC32; not verified, matching FFmpeg's
   * default (CRC checking is opt-in there via AV_EF_CRCCHECK). */

  if (data->format > TTA_FORMAT_ENCRYPTED) return false;
  if (data->channels == 0 || data->channels > 16) return false;
  if (data->samplerate == 0 || data->samplerate > 0x7FFFFFu) return false;
  if (data->data_length == 0) return false;

  data->bps = (data->bits_per_sample + 7) / 8;
  if (data->bps < 1 || data->bps > 3) return false;

  data->frame_length = static_cast<int>(256 * data->samplerate / 245);
  if (data->frame_length <= 0) return false;
  if (static_cast<unsigned>(data->frame_length) >=
      UINT_MAX / (static_cast<unsigned>(data->channels) * sizeof(int32_t)))
  {
    return false;
  }

  /* A whole frame has to be decoded at once, so frame_length * channels
   * int32s stay resident, and both terms come straight off the file.
   * FFmpeg only guards the size computation against overflowing, which
   * on a 32MB target is barely a guard at all - a header claiming 16
   * channels at the highest permitted rate asks for over 500MB. This
   * ceiling clears every realistic file (192kHz 7.1 needs 6.4MB) while
   * keeping a corrupt or hostile header from taking the player out. */
  if (static_cast<int64_t>(data->frame_length) * data->channels *
          static_cast<int64_t>(sizeof(int32_t)) >
      MAX_DECODE_BUFFER_BYTES)
  {
    return false;
  }

  data->last_frame_length =
      static_cast<int>(data->data_length % static_cast<unsigned>(data->frame_length));
  int64_t totalframes = data->data_length / static_cast<unsigned>(data->frame_length) +
                        (data->last_frame_length ? 1 : 0);
  if (totalframes <= 0 || totalframes > INT_MAX / 8) return false;

  /* A frame holds at most frame_length * channels samples, and the Rice
   * coder cannot spend more than about bits_per_sample + 2 bits on any
   * one of them in a legitimately encoded stream, so four bytes each is
   * a generous ceiling. Bounding this matters: the sizes come straight
   * from the file, and an unbounded trust would let a corrupt seek table
   * ask for a multi-gigabyte allocation. */
  const int64_t max_frame_bytes =
      static_cast<int64_t>(data->frame_length) * data->channels * 4 + 4096;

  data->frame_sizes.resize(static_cast<size_t>(totalframes));

  int64_t table_bytes = totalframes * 4;
  std::vector<uint8_t> table(static_cast<size_t>(table_bytes));
  if (io_read(pb, table.data(), table.size()) != static_cast<ssize_t>(table.size()))
  {
    return false;
  }

  int64_t max_size = 0;
  for (int64_t i = 0; i < totalframes; i++)
  {
    uint32_t size = rl32(table.data() + i * 4);
    if (size == 0 || static_cast<int64_t>(size) > max_frame_bytes) return false;
    data->frame_sizes[static_cast<size_t>(i)] = size;
    if (static_cast<int64_t>(size) > max_size) max_size = size;
  }

  /* Skip the seek table's own CRC32 (also unverified). */
  uint8_t table_crc[4];
  if (io_read(pb, table_crc, sizeof(table_crc)) != static_cast<ssize_t>(sizeof(table_crc)))
  {
    return false;
  }

  data->first_frame_pos = io_tell(pb);
  if (data->first_frame_pos < 0) return false;

  /* Deliberately not checking that the frame sizes sum to the bytes
   * actually present: a truncated file still plays up to the cut, since
   * tta_fill_buffer() stops cleanly on a short read, and refusing it
   * outright would lose files FFmpeg happily plays. Only a file with no
   * audio at all is rejected here. Frame sizes are already individually
   * bounded above, so trusting them costs no memory safety. */
  if (data->file_size > 0 && data->first_frame_pos >= data->file_size)
  {
    return false;
  }

  data->packet.resize(static_cast<size_t>(max_size));
  data->currentframe = 0;
  data->duration = static_cast<int>(data->data_length / data->samplerate);

  return true;
}

/* Decode one whole frame out of data->packet into data->decode_buffer.
 * Returns the number of sample-frames produced, or -1 on a corrupt
 * frame. This mirrors tta_decode_frame(); TTA reinitialises every
 * channel's filter, Rice and predictor state here, which is what makes
 * each frame independently decodable. */
static int tta_decode_packet(struct tta_data *data, int packet_size)
{
  BitReaderLE gb;
  gb.init(data->packet.data(), packet_size);

  int framelen = data->frame_length;
  int cur_chan = 0;
  int i;

  for (i = 0; i < data->channels; i++)
  {
    TTAFilter *filter = &data->ch_ctx[static_cast<size_t>(i)].filter;
    data->ch_ctx[static_cast<size_t>(i)].predictor = 0;
    tta_filter_init(filter, tta_filter_configs[data->bps - 1]);
    tta_rice_init(&data->ch_ctx[static_cast<size_t>(i)].rice, 10, 10);
  }

  i = 0;
  int32_t *base = data->decode_buffer.data();
  int32_t *end = base + static_cast<size_t>(framelen) * data->channels;

  for (int32_t *p = base; p < end; p++)
  {
    TTAChannel *chan = &data->ch_ctx[static_cast<size_t>(cur_chan)];
    int32_t *predictor = &chan->predictor;
    TTAFilter *filter = &chan->filter;
    TTARice *rice = &chan->rice;
    uint32_t unary, depth, k;
    int32_t value;

    unary = static_cast<uint32_t>(get_unary(gb, 0, gb.get_bits_left()));

    if (unary == 0)
    {
      depth = 0;
      k = rice->k0;
    }
    else
    {
      depth = 1;
      k = rice->k1;
      unary--;
    }

    /* Signed-to-unsigned conversion here is FFmpeg's, and deliberate:
     * once the reader has overrun, get_bits_left() goes negative and
     * wraps large, so this check stops firing and the frame runs to its
     * natural end rather than being rejected. */
    if (static_cast<unsigned>(gb.get_bits_left()) < k)
    {
      return -1;
    }

    if (k)
    {
      if (k > static_cast<uint32_t>(MIN_CACHE_BITS) ||
          unary > static_cast<uint32_t>(INT32_MAX >> k))
      {
        return -1;
      }
      value = static_cast<int32_t>((unary << k) + gb.get_bits(static_cast<int>(k)));
    }
    else
    {
      value = static_cast<int32_t>(unary);
    }

    switch (depth)
    {
    case 1:
      rice->sum1 += static_cast<uint32_t>(value) - (rice->sum1 >> 4);
      if (rice->k1 > 0 && rice->sum1 < tta_shift_16[rice->k1])
        rice->k1--;
      else if (rice->sum1 > tta_shift_16[rice->k1 + 1])
        rice->k1++;
      value += static_cast<int32_t>(tta_shift_1[rice->k0]);
      [[fallthrough]];
    default:
      rice->sum0 += static_cast<uint32_t>(value) - (rice->sum0 >> 4);
      if (rice->k0 > 0 && rice->sum0 < tta_shift_16[rice->k0])
        rice->k0--;
      else if (rice->sum0 > tta_shift_16[rice->k0 + 1])
        rice->k0++;
    }

    // extract coded value
    *p = 1 + ((value >> 1) ^ ((value & 1) - 1));

    // run hybrid filter
    tta_filter_process(filter->qm, filter->dx, filter->dl, &filter->error, p,
                       filter->shift, filter->round);

    // fixed order prediction
    switch (data->bps)
    {
    case 1:
      *p = static_cast<int32_t>(static_cast<uint32_t>(*p) +
                                static_cast<uint32_t>(tta_pred(*predictor, 4)));
      break;
    case 2:
    case 3:
      *p = static_cast<int32_t>(static_cast<uint32_t>(*p) +
                                static_cast<uint32_t>(tta_pred(*predictor, 5)));
      break;
    }
    *predictor = *p;

    // flip channels
    if (cur_chan < (data->channels - 1))
    {
      cur_chan++;
    }
    else
    {
      // decorrelate in case of multiple channels
      if (data->channels > 1)
      {
        int32_t *r = p - 1;
        for (*p = static_cast<int32_t>(static_cast<uint32_t>(*p) +
                                       static_cast<uint32_t>(*r / 2));
             r > p - data->channels; r--)
        {
          *r = static_cast<int32_t>(static_cast<uint32_t>(*(r + 1)) -
                                    static_cast<uint32_t>(*r));
        }
      }
      cur_chan = 0;
      i++;
      /* The last frame is short, and is recognised by having produced
       * exactly last_frame_length sample-frames with only the 4-byte
       * frame CRC left to read. */
      if (i == data->last_frame_length && gb.get_bits_left() / 8 == 4)
      {
        framelen = data->last_frame_length;
        break;
      }
    }
  }

  gb.align();
  if (gb.get_bits_left() < 32) return -1;
  gb.skip_bits_long(32); // frame crc, not verified

  return framelen;
}

/* Read and decode the next frame. Returns false at clean EOF or on a
 * corrupt frame (error is set in the latter case). */
static bool tta_fill_buffer(struct tta_data *data)
{
  data->frame_samples = 0;
  data->buffer_pos = 0;

  if (data->eof || data->currentframe >= data->frame_sizes.size())
  {
    data->eof = true;
    return false;
  }

  uint32_t size = data->frame_sizes[data->currentframe];

  size_t off = 0;
  while (off < size)
  {
    ssize_t got = io_read(data->io_stream.get(), data->packet.data() + off, size - off);
    if (got <= 0)
    {
      decoder_error(&data->error, ERROR_STREAM, 0, "Truncated TTA frame");
      data->eof = true;
      return false;
    }
    off += static_cast<size_t>(got);
  }

  int samples = tta_decode_packet(data, static_cast<int>(size));
  if (samples <= 0)
  {
    decoder_error(&data->error, ERROR_STREAM, 0, "TTA frame decode failed");
    data->eof = true;
    return false;
  }

  data->currentframe++;
  data->frame_samples = samples;
  return true;
}

/* Convert `count` sample-frames starting at `from` into the output
 * buffer. Mirrors the conversion tail of tta_decode_frame(). */
static void tta_pack_output(struct tta_data *data, int from, int count, char *out)
{
  const int32_t *p = data->decode_buffer.data() +
                     static_cast<size_t>(from) * data->channels;
  int n = count * data->channels;

  switch (data->bps)
  {
  case 1:
  {
    auto *samples = reinterpret_cast<uint8_t *>(out);
    for (int i = 0; i < n; i++)
      samples[i] = static_cast<uint8_t>(p[i] + 0x80);
    break;
  }
  case 2:
  {
    auto *samples = reinterpret_cast<int16_t *>(out);
    for (int i = 0; i < n; i++)
      samples[i] = static_cast<int16_t>(p[i]);
    break;
  }
  case 3:
  {
    auto *samples = reinterpret_cast<int32_t *>(out);
    for (int i = 0; i < n; i++)
      samples[i] = static_cast<int32_t>(static_cast<uint32_t>(p[i]) * 256U);
    break;
  }
  }
}

} // namespace

// ---------------------------------------------------------------------
// mocf decoder/plugin interface
// ---------------------------------------------------------------------

static void *tta_open(const char *file)
{
  auto *data = new tta_data;
  decoder_error_init(&data->error);

  data->io_stream.reset(io_open(file, 1));
  if (!io_ok(data->io_stream.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s",
                  io_strerror(data->io_stream.get()));
    return data;
  }

  if (!tta_read_header(data))
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Not a valid or supported TTA file: %s", file);
    return data;
  }

  /* Encrypted streams need a password, which mocf has no way to ask
   * for; FFmpeg exposes this as a decoder option. */
  if (data->format == TTA_FORMAT_ENCRYPTED)
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Encrypted TTA files are not supported");
    return data;
  }

  data->ch_ctx.resize(static_cast<size_t>(data->channels));
  data->decode_buffer.assign(
      static_cast<size_t>(data->frame_length) * data->channels, 0);

  if (data->duration > 0 && data->file_size > 0)
  {
    data->avg_bitrate =
        static_cast<int>((data->file_size * 8) / data->duration / 1000);
  }

  data->ok = true;

  debug("TTA file opened. Channels %d. Rate %u. Bits %d. Samples %u. "
        "Frames %zu. Duration %d.",
        data->channels, data->samplerate, data->bits_per_sample,
        data->data_length, data->frame_sizes.size(), data->duration);

  return data;
}

static void tta_close(void *prv_data)
{
  auto *data = static_cast<struct tta_data *>(prv_data);
  decoder_error_clear(&data->error);
  delete data;
  logit("File closed");
}

static void tta_get_error(void *prv_data, struct decoder_error *error)
{
  auto *data = static_cast<struct tta_data *>(prv_data);
  decoder_error_copy(error, &data->error);
}

static int tta_get_duration(void *prv_data)
{
  auto *data = static_cast<struct tta_data *>(prv_data);
  return data->duration;
}

static int tta_get_avg_bitrate(void *prv_data)
{
  auto *data = static_cast<struct tta_data *>(prv_data);
  return data->avg_bitrate;
}

/* TTA is variable rate, so report the frame actually being drained
 * rather than the file average. */
static int tta_get_bitrate(void *prv_data)
{
  auto *data = static_cast<struct tta_data *>(prv_data);

  if (data->currentframe == 0 || data->frame_samples <= 0 || data->samplerate == 0)
  {
    return data->avg_bitrate;
  }

  uint32_t size = data->frame_sizes[data->currentframe - 1];
  int64_t bits = static_cast<int64_t>(size) * 8 * data->samplerate;
  return static_cast<int>(bits / data->frame_samples / 1000);
}

static int tta_seek(void *prv_data, int sec)
{
  auto *data = static_cast<struct tta_data *>(prv_data);

  if (sec < 0 || data->duration <= 0 || sec >= data->duration ||
      data->samplerate == 0)
  {
    return -1;
  }

  int64_t target_sample = static_cast<int64_t>(sec) * data->samplerate;
  int64_t frame_index = target_sample / data->frame_length;
  int sample_in_frame = static_cast<int>(target_sample % data->frame_length);

  if (frame_index < 0 || frame_index >= static_cast<int64_t>(data->frame_sizes.size()))
  {
    return -1;
  }

  /* Every frame reinitialises all decoder state, so reaching a frame
   * needs nothing but its file offset - no decode-and-discard run-up of
   * the kind the APE decoder has to do. */
  int64_t pos = data->first_frame_pos;
  for (int64_t i = 0; i < frame_index; i++)
  {
    pos += data->frame_sizes[static_cast<size_t>(i)];
  }

  if (io_seek(data->io_stream.get(), pos, SEEK_SET) == -1)
  {
    logit("tta: seek to frame %" PRId64 " failed", frame_index);
    return -1;
  }

  data->currentframe = static_cast<uint32_t>(frame_index);
  data->frame_samples = 0;
  data->buffer_pos = 0;
  data->seek_skip = sample_in_frame;
  data->eof = false;

  /* The requested second is landed on exactly: the frame is decoded in
   * full anyway, so the samples before it are simply not handed out. */
  return sec;
}

static int tta_decode(void *prv_data, char *buf, int buf_len,
                      struct sound_params *sound_params)
{
  auto *data = static_cast<struct tta_data *>(prv_data);
  decoder_error_clear(&data->error);

  for (;;)
  {
    if (data->buffer_pos >= data->frame_samples)
    {
      if (!tta_fill_buffer(data))
      {
        return 0; /* clean EOF, or error set by tta_fill_buffer() */
      }

      if (data->seek_skip > 0)
      {
        data->buffer_pos = std::min(data->seek_skip, data->frame_samples);
        data->seek_skip = 0;
        if (data->buffer_pos >= data->frame_samples) continue;
      }
    }

    int bytes_per_sample_frame = (data->bps == 3 ? 4 : data->bps) * data->channels;
    int want = buf_len / bytes_per_sample_frame;
    if (want <= 0) return 0;

    int avail = data->frame_samples - data->buffer_pos;
    int n = std::min(avail, want);

    tta_pack_output(data, data->buffer_pos, n, buf);
    data->buffer_pos += n;

    sound_params->channels = data->channels;
    sound_params->rate = static_cast<int>(data->samplerate);
    sound_params->fmt = (data->bps == 1)   ? SFMT_U8
                        : (data->bps == 2) ? (SFMT_S16 | SFMT_NE)
                                           : (SFMT_S32 | SFMT_NE);

    return n * bytes_per_sample_frame;
  }
}

/* TTA carries no metadata of its own; any tags are ID3v2/APE wrappers
 * around the stream, which mocf reads elsewhere. Only the duration is
 * available here, straight out of the header. */
static void tta_info(const char *file_name, struct file_tags *info,
                     const int tags_sel)
{
  if (!(tags_sel & TAGS_TIME)) return;

  auto data = std::make_unique<struct tta_data>();
  decoder_error_init(&data->error);
  data->io_stream.reset(io_open(file_name, 1));

  if (!io_ok(data->io_stream.get())) return;
  if (!tta_read_header(data.get())) return;

  info->time = data->duration;
  info->filled |= TAGS_TIME;
}

static int tta_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "tta");
}

static std::string tta_get_name(const char *)
{
  return "TTA";
}

class TtaDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    TtaDecoder(void *d) : data(d, tta_close) {}
    ~TtaDecoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return tta_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return tta_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return tta_get_bitrate(data.get());
    }

    int get_duration() override {
        return tta_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        tta_get_error(data.get(), error);
    }

    int get_avg_bitrate() override {
        return tta_get_avg_bitrate(data.get());
    }
};

class TtaPlugin : public AudioPlugin {
public:
    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = tta_open(file);
        if (!d) return nullptr;
        return std::make_unique<TtaDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        tta_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return tta_our_format_ext(ext);
    }

    std::string get_name(const char *file) override {
        return tta_get_name(file);
    }
};

extern "C" class AudioPlugin *tta_plugin_init() {
    static TtaPlugin plugin;
    return &plugin;
}

// EOF
