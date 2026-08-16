// src/audio/decoders/mlp/mlpdec.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// MLP (Meridian Lossless Packing) and TrueHD decoder, ported from FFmpeg
// n4.4.8 (LGPLv2.1+). The upstream files this is assembled from and their
// copyright holders:
//
//   libavcodec/mlpdec.c            Copyright (c) 2007-2008 Ian Caulfield
//   libavcodec/mlp.c, mlp.h        Copyright (c) 2007-2008 Ian Caulfield
//   libavcodec/mlp_parse.c/.h      Copyright (c) 2007 Ian Caulfield
//   libavcodec/mlpdsp.c, mlpdsp.h  Copyright (c) 2007-2008 Ian Caulfield
//                                  Copyright (c) 2009 Ramiro Polla
//   libavutil/crc.c (av_crc)       Copyright (c) 2006 Michael Niedermayer
//   libavcodec/get_bits.h          Copyright (c) 2004 Michael Niedermayer
//
// The decode math is upstream's, unchanged. Adapted plumbing: FFmpeg's
// bitstream reader, VLC builder, CRC tables and logging are replaced by the
// self-contained equivalents below, the MLPDSPContext function-pointer
// dispatch is dropped (mocf has no SIMD variants to select between), and the
// AVCodec entry points become the small C++ API in mlpdec.h.
//
// Distributed in mocf under the GNU GPL version 3 or later, as permitted
// by section 3 of upstream's LGPL version 2.1.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
// details.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <climits>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>

#include "core/common.h"
#include "core/log.h"

#include "mlpdec.h"

namespace
{

// ---------------------------------------------------------------------------
// Minimal FFmpeg compatibility layer
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Logging
//
// Upstream logs through av_log(avctx, level, ...). Those call sites are kept
// verbatim and routed here instead of being rewritten, so this file stays
// diffable against libavcodec/mlpdec.c.
// ---------------------------------------------------------------------------

constexpr int AV_LOG_PANIC = 0;
constexpr int AV_LOG_FATAL = 8;
constexpr int AV_LOG_ERROR = 16;
constexpr int AV_LOG_WARNING = 24;
constexpr int AV_LOG_INFO = 32;
constexpr int AV_LOG_VERBOSE = 40;
constexpr int AV_LOG_DEBUG = 48;

/* Routed to mocf's logger; decode-path chatter would flood the log, so only
 * warnings and errors get through. */
inline void mlp_log(int level, const char *fmt, ...)
{
  if (level > AV_LOG_WARNING) return;

  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  /* FFmpeg's messages carry their own newline; mocf's logger adds one. */
  size_t n = strlen(msg);
  while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
  {
    msg[--n] = '\0';
  }

  logit("mlp: %s", msg);
}
/* The context argument only carries FFmpeg's logger identity, which mocf's
 * logger has no use for; it is still evaluated so that callers taking an
 * AVCodecContext purely to log through it do not look unused. */
#define av_log(ctx, level, ...)                                                \
  do                                                                           \
  {                                                                            \
    (void)(ctx);                                                               \
    mlp_log(level, __VA_ARGS__);                                               \
  } while (0)
/* Upstream uses this to report streams exercising paths it has never seen a
 * sample of. Same routing, warning level. */
#define avpriv_request_sample(ctx, ...)                                        \
  do                                                                           \
  {                                                                            \
    (void)(ctx);                                                               \
    mlp_log(AV_LOG_WARNING, __VA_ARGS__);                                      \
  } while (0)
#define av_assert0(x)           do { } while (0)
#define av_assert1(x)           do { } while (0)
#define av_assert2(x)           do { } while (0)

constexpr int AVERROR_INVALIDDATA = -1;
constexpr int AVERROR_PATCHWELCOME = -2;

#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFSWAP(type, a, b)                                                     \
  do                                                                           \
  {                                                                            \
    type SWAP_tmp = b;                                                         \
    b = a;                                                                     \
    a = SWAP_tmp;                                                              \
  } while (0)

#define FF_ARRAY_ELEMS(a) (static_cast<int>(sizeof(a) / sizeof((a)[0])))

/// Big-endian loads. Upstream uses libavutil/intreadwrite.h for these.
inline uint32_t AV_RB32(const uint8_t *p)
{
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
inline uint16_t AV_RB16(const uint8_t *p)
{
  return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint16_t AV_RL16(const uint8_t *p)
{
  return static_cast<uint16_t>((p[1] << 8) | p[0]);
}

// Channel-layout bitmasks (libavutil/channel_layout.h). MLP needs these only
// to identify substream layouts: the downmix selection compares a requested
// layout against each substream's mask, and TrueHD's channel assignment is
// resolved by index within the mask. mocf itself only ever consumes the
// resulting channel count.
constexpr uint64_t AV_CH_FRONT_LEFT = 0x00000001;
constexpr uint64_t AV_CH_FRONT_RIGHT = 0x00000002;
constexpr uint64_t AV_CH_FRONT_CENTER = 0x00000004;
constexpr uint64_t AV_CH_LOW_FREQUENCY = 0x00000008;
constexpr uint64_t AV_CH_BACK_LEFT = 0x00000010;
constexpr uint64_t AV_CH_BACK_RIGHT = 0x00000020;
constexpr uint64_t AV_CH_FRONT_LEFT_OF_CENTER = 0x00000040;
constexpr uint64_t AV_CH_FRONT_RIGHT_OF_CENTER = 0x00000080;
constexpr uint64_t AV_CH_BACK_CENTER = 0x00000100;
constexpr uint64_t AV_CH_SIDE_LEFT = 0x00000200;
constexpr uint64_t AV_CH_SIDE_RIGHT = 0x00000400;
constexpr uint64_t AV_CH_TOP_CENTER = 0x00000800;
constexpr uint64_t AV_CH_TOP_FRONT_LEFT = 0x00001000;
constexpr uint64_t AV_CH_TOP_FRONT_CENTER = 0x00002000;
constexpr uint64_t AV_CH_TOP_FRONT_RIGHT = 0x00004000;
constexpr uint64_t AV_CH_WIDE_LEFT = 0x0000000080000000ULL;
constexpr uint64_t AV_CH_WIDE_RIGHT = 0x0000000100000000ULL;
constexpr uint64_t AV_CH_SURROUND_DIRECT_LEFT = 0x0000000200000000ULL;
constexpr uint64_t AV_CH_SURROUND_DIRECT_RIGHT = 0x0000000400000000ULL;
constexpr uint64_t AV_CH_LOW_FREQUENCY_2 = 0x0000000800000000ULL;

constexpr uint64_t AV_CH_LAYOUT_MONO = AV_CH_FRONT_CENTER;
constexpr uint64_t AV_CH_LAYOUT_STEREO = AV_CH_FRONT_LEFT | AV_CH_FRONT_RIGHT;
constexpr uint64_t AV_CH_LAYOUT_2POINT1 = AV_CH_LAYOUT_STEREO | AV_CH_LOW_FREQUENCY;
constexpr uint64_t AV_CH_LAYOUT_2_1 = AV_CH_LAYOUT_STEREO | AV_CH_BACK_CENTER;
constexpr uint64_t AV_CH_LAYOUT_SURROUND = AV_CH_LAYOUT_STEREO | AV_CH_FRONT_CENTER;
constexpr uint64_t AV_CH_LAYOUT_3POINT1 = AV_CH_LAYOUT_SURROUND | AV_CH_LOW_FREQUENCY;
constexpr uint64_t AV_CH_LAYOUT_4POINT0 = AV_CH_LAYOUT_SURROUND | AV_CH_BACK_CENTER;
constexpr uint64_t AV_CH_LAYOUT_4POINT1 = AV_CH_LAYOUT_4POINT0 | AV_CH_LOW_FREQUENCY;
constexpr uint64_t AV_CH_LAYOUT_QUAD =
    AV_CH_LAYOUT_STEREO | AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT;
constexpr uint64_t AV_CH_LAYOUT_5POINT0_BACK =
    AV_CH_LAYOUT_SURROUND | AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT;
constexpr uint64_t AV_CH_LAYOUT_5POINT1_BACK =
    AV_CH_LAYOUT_5POINT0_BACK | AV_CH_LOW_FREQUENCY;

/// Population count of a layout mask. Upstream calls
/// av_get_channel_layout_nb_channels(), which is this.
inline int av_get_channel_layout_nb_channels(uint64_t layout)
{
  return __builtin_popcountll(layout);
}

/// Index of @p channel within @p layout, counting set bits below it, or -1 if
/// @p channel is not present. Upstream: av_get_channel_layout_channel_index().
inline int av_get_channel_layout_channel_index(uint64_t layout, uint64_t channel)
{
  if (!(layout & channel)) return -1;
  return __builtin_popcountll(layout & (channel - 1));
}

/// Sign-extend the low @p bits of @p val. Verbatim from libavutil/common.h.
inline int sign_extend(int val, unsigned bits)
{
  const unsigned shift = 8 * sizeof(int) - bits;
  union
  {
    unsigned u;
    int s;
  } v = {static_cast<unsigned>(val) << shift};
  return v.s >> shift;
}

// ---------------------------------------------------------------------------
// MSB-first bitstream reader
//
// Exposes the subset of FFmpeg's get_bits.h API that the MLP math calls. Bit
// order, over-read behaviour and the get_vlc2 walk match get_bits.h exactly;
// only the internals are ours. FFmpeg's reader over-reads into a mandatory
// padding area, so reads past the end must yield zero bits rather than touch
// memory -- enforced here with an explicit bound instead of relying on
// caller-supplied padding.
// ---------------------------------------------------------------------------

/// Widest field a single show_bits()/get_bits() call can return. This reader
/// gathers 4 bytes, so with a bit offset of up to 7 the ceiling is 25.
constexpr int MIN_CACHE_BITS = 25;

struct GetBitContext
{
  const uint8_t *buffer = nullptr;
  int buffer_size = 0; ///< bytes backing @ref buffer
  int index = 0;       ///< current bit position
  int size_in_bits = 0;
};

inline int init_get_bits(GetBitContext *s, const uint8_t *buffer, int bit_size)
{
  if (bit_size < 0 || !buffer)
  {
    s->buffer = nullptr;
    s->buffer_size = 0;
    s->index = 0;
    s->size_in_bits = 0;
    return AVERROR_INVALIDDATA;
  }
  s->buffer = buffer;
  s->size_in_bits = bit_size;
  s->buffer_size = (bit_size + 7) >> 3;
  s->index = 0;
  return 0;
}

inline int get_bits_count(const GetBitContext *s) { return s->index; }
inline int get_bits_left(const GetBitContext *s) { return s->size_in_bits - s->index; }

/// Peek @p n bits (1..25) without consuming. Zero-fills past the end.
inline unsigned show_bits(const GetBitContext *s, int n)
{
  if (n <= 0 || n > MIN_CACHE_BITS) return 0;

  const int byte = s->index >> 3;
  const int off = s->index & 7;
  uint32_t cache = 0;

  // Gather 4 bytes big-endian; off + n <= 7 + 25 == 32, so this always holds
  // the requested field.
  for (int i = 0; i < 4; i++)
  {
    const int b = byte + i;
    cache = (cache << 8) | ((b >= 0 && b < s->buffer_size) ? s->buffer[b] : 0u);
  }
  return (cache << off) >> (32 - n);
}

inline void skip_bits(GetBitContext *s, int n) { s->index += n; }
inline void skip_bits1(GetBitContext *s) { s->index += 1; }
inline void skip_bits_long(GetBitContext *s, int n) { s->index += n; }

inline unsigned get_bits(GetBitContext *s, int n)
{
  const unsigned v = show_bits(s, n);
  s->index += n;
  return v;
}

inline unsigned get_bits1(GetBitContext *s)
{
  const int idx = s->index;
  unsigned bit = 0;
  const int byte = idx >> 3;
  if (byte >= 0 && byte < s->buffer_size)
  {
    bit = (s->buffer[byte] >> (7 - (idx & 7))) & 1;
  }
  s->index = idx + 1;
  return bit;
}

/// Reads up to 32 bits; get_bits() alone tops out at MIN_CACHE_BITS.
inline unsigned get_bits_long(GetBitContext *s, int n)
{
  if (n <= 0) return 0;
  if (n <= MIN_CACHE_BITS) return get_bits(s, n);
  const unsigned hi = get_bits(s, 16);
  const unsigned lo = get_bits(s, n - 16);
  return (hi << (n - 16)) | lo;
}

/// Peek up to 32 bits without consuming.
inline unsigned show_bits_long(const GetBitContext *s, int n)
{
  if (n <= MIN_CACHE_BITS) return show_bits(s, n);
  GetBitContext tmp = *s;
  return get_bits_long(&tmp, n);
}

inline int get_sbits(GetBitContext *s, int n)
{
  return sign_extend(static_cast<int>(get_bits(s, n)), n);
}

// ---------------------------------------------------------------------------
// VLC decoding
//
// MLP's three Huffman codebooks hold 18 symbols with a longest code of 9 bits,
// and the lookup uses 9 index bits, so upstream's init_vlc() always produces a
// single-level table with no subtable indirection. Rather than port FFmpeg's
// general multi-level builder, the canonical table is filled directly: every
// index whose leading @c len bits equal @c code maps to that symbol. That is
// what init_vlc() computes for this input, and the byte-exact fixtures confirm
// it.
// ---------------------------------------------------------------------------

constexpr int VLC_BITS = 9;
constexpr int VLC_SIZE = 1 << VLC_BITS;

struct VlcEntry
{
  int8_t sym;  ///< decoded symbol, or -1 for an invalid code
  int8_t len;  ///< code length in bits
};

struct Vlc
{
  VlcEntry table[VLC_SIZE];
};

/// Builds @p vlc from @p n (code, length) pairs given as {code, bits} rows.
void vlc_init(Vlc *vlc, const uint8_t (*codes)[2], int n)
{
  for (int i = 0; i < VLC_SIZE; i++)
  {
    vlc->table[i].sym = -1;
    vlc->table[i].len = 0;
  }

  for (int sym = 0; sym < n; sym++)
  {
    const unsigned code = codes[sym][0];
    const int len = codes[sym][1];
    if (len <= 0 || len > VLC_BITS) continue;

    // Every index sharing this code's leading `len` bits decodes to `sym`.
    const int shift = VLC_BITS - len;
    const unsigned base = code << shift;
    for (unsigned j = 0; j < (1u << shift); j++)
    {
      VlcEntry &e = vlc->table[base + j];
      e.sym = static_cast<int8_t>(sym);
      e.len = static_cast<int8_t>(len);
    }
  }
}

/// Single-level lookup. Mirrors get_bits.h's GET_VLC for a table with no
/// subtables: consume the matched code's length, return the symbol.
inline int get_vlc2(GetBitContext *s, const Vlc *vlc)
{
  const unsigned index = show_bits(s, VLC_BITS);
  const VlcEntry &e = vlc->table[index];
  skip_bits(s, e.len);
  return e.sym;
}

// ---------------------------------------------------------------------------
// CRC
//
// Ported from libavutil/crc.c. MLP only ever checksums headers (a major sync
// or a restart header), never the audio payload, so the 257-entry tables are
// used unconditionally: the 1024-entry variant's word-at-a-time path would add
// ~9KB of tables to save time on buffers a few dozen bytes long.
// ---------------------------------------------------------------------------

using AVCRC = uint32_t;
constexpr int CRC_TABLE_SIZE = 257;

int av_crc_init(AVCRC *ctx, int le, int bits, uint32_t poly)
{
  if (bits < 8 || bits > 32 || poly >= (1ULL << bits)) return -1;

  for (unsigned i = 0; i < 256; i++)
  {
    uint32_t c;
    if (le)
    {
      unsigned j;
      for (c = i, j = 0; j < 8; j++)
      {
        c = (c >> 1) ^ (poly & (-(c & 1)));
      }
      ctx[i] = c;
    }
    else
    {
      unsigned j;
      for (c = i << 24, j = 0; j < 8; j++)
      {
        c = (c << 1) ^ ((poly << (32 - bits)) & ((static_cast<int32_t>(c)) >> 31));
      }
      ctx[i] = __builtin_bswap32(c);
    }
  }
  ctx[256] = 1;
  return 0;
}

uint32_t av_crc(const AVCRC *ctx, uint32_t crc, const uint8_t *buffer, size_t length)
{
  const uint8_t *end = buffer + length;

  while (buffer < end)
  {
    crc = ctx[(static_cast<uint8_t>(crc)) ^ *buffer++] ^ (crc >> 8);
  }

  return crc;
}

} // namespace

// ---------------------------------------------------------------------------
// Format constants and tables (libavcodec/mlp.h, mlp.c, mlp_parse.h)
// ---------------------------------------------------------------------------

namespace
{


/** Which channel modifier a TrueHD substream carries. Upstream: mlp.h. */
enum THDChannelModifier
{
  THD_CH_MODIFIER_NOTINDICATED = 0x0,
  THD_CH_MODIFIER_STEREO = 0x0,        // Stereo (not Dolby Surround)
  THD_CH_MODIFIER_LTRT = 0x1,          // Dolby Surround
  THD_CH_MODIFIER_LBINRBIN = 0x2,      // Dolby Headphone
  THD_CH_MODIFIER_MONO = 0x3,          // Mono or Dual Mono
  THD_CH_MODIFIER_NOTSURROUNDEX = 0x1, // Not Dolby Digital EX
  THD_CH_MODIFIER_SURROUNDEX = 0x2,    // Dolby Digital EX
};

/** Matrix encoding a substream declares. Upstream reports this to callers as
 *  AVFrame side data; mocf has no consumer for it, but the field is kept so
 *  the major sync and restart header parsing stay identical to upstream. */
enum AVMatrixEncoding
{
  AV_MATRIX_ENCODING_NONE,
  AV_MATRIX_ENCODING_DOLBY,
  AV_MATRIX_ENCODING_DPLII,
  AV_MATRIX_ENCODING_DPLIIX,
  AV_MATRIX_ENCODING_DPLIIZ,
  AV_MATRIX_ENCODING_DOLBYEX,
  AV_MATRIX_ENCODING_DOLBYHEADPHONE,
  AV_MATRIX_ENCODING_NB,
};

/** Last possible matrix channel for each codec */
constexpr int MAX_MATRIX_CHANNEL_MLP = 5;
constexpr int MAX_MATRIX_CHANNEL_TRUEHD = 7;
/** Maximum number of channels in a valid stream.
 *  MLP   : 5.1 + 2 noise channels -> 8 channels
 *  TrueHD: 7.1                    -> 8 channels
 */
constexpr int MAX_CHANNELS = 8;

/** Maximum number of matrices used in decoding; most streams have one matrix
 *  per output channel, but some rematrix a channel (usually 0) more than once.
 */
constexpr int MAX_MATRICES_MLP = 6;
constexpr int MAX_MATRICES_TRUEHD = 8;
constexpr int MAX_MATRICES = 8;

/** Maximum number of substreams that can be decoded.
 *  MLP's limit is 2. TrueHD supports at least up to 3.
 */
constexpr int MAX_SUBSTREAMS = 4;

/** which multiple of 48000 the maximum sample rate is */
constexpr int MAX_RATEFACTOR = 4;
/** maximum sample frequency seen in files */
constexpr int MAX_SAMPLERATE = MAX_RATEFACTOR * 48000;

/** maximum number of audio samples within one access unit */
constexpr int MAX_BLOCKSIZE = 40 * MAX_RATEFACTOR;
/** next power of two greater than MAX_BLOCKSIZE */
constexpr int MAX_BLOCKSIZE_POW2 = 64 * MAX_RATEFACTOR;

/** number of allowed filters */
constexpr int NUM_FILTERS = 2;

/** The maximum number of taps in IIR and FIR filters. */
constexpr int MAX_FIR_ORDER = 8;
constexpr int MAX_IIR_ORDER = 4;

/** Code that signals end of a stream. */
constexpr uint32_t END_OF_STREAM = 0xd234d234;

constexpr int FIR = 0;
constexpr int IIR = 1;

/** filter data */
struct FilterParams
{
  uint8_t order = 0; ///< number of taps in filter
  uint8_t shift = 0; ///< Right shift to apply to output of filter.

  int32_t state[MAX_FIR_ORDER] = {};

  int coeff_bits = 0;
  int coeff_shift = 0;
};

/** sample data coding information */
struct ChannelParams
{
  FilterParams filter_params[NUM_FILTERS];
  int32_t coeff[NUM_FILTERS][MAX_FIR_ORDER] = {};

  int16_t huff_offset = 0;      ///< Offset to apply to residual values.
  int32_t sign_huff_offset = 0; ///< sign/rounding-corrected version of huff_offset
  uint8_t codebook = 0;         ///< Which VLC codebook to use to read residuals.
  uint8_t huff_lsbs = 0;        ///< Size of residual suffix not encoded using VLC.
};

/** Tables defining the Huffman codes.
 *  There are three entropy coding methods used in MLP (four if you count
 *  "none" as a method). These use the same sequences for codes starting with
 *  00 or 01, but have different codes starting with 1.
 */
const uint8_t ff_mlp_huffman_tables[3][18][2] = {
    {/* Huffman table 0, -7 - +10 */
     {0x01, 9},
     {0x01, 8},
     {0x01, 7},
     {0x01, 6},
     {0x01, 5},
     {0x01, 4},
     {0x01, 3},
     {0x04, 3},
     {0x05, 3},
     {0x06, 3},
     {0x07, 3},
     {0x03, 3},
     {0x05, 4},
     {0x09, 5},
     {0x11, 6},
     {0x21, 7},
     {0x41, 8},
     {0x81, 9}},
    {/* Huffman table 1, -7 - +8 */
     {0x01, 9},
     {0x01, 8},
     {0x01, 7},
     {0x01, 6},
     {0x01, 5},
     {0x01, 4},
     {0x01, 3},
     {0x02, 2},
     {0x03, 2},
     {0x03, 3},
     {0x05, 4},
     {0x09, 5},
     {0x11, 6},
     {0x21, 7},
     {0x41, 8},
     {0x81, 9}},
    {/* Huffman table 2, -7 - +7 */
     {0x01, 9},
     {0x01, 8},
     {0x01, 7},
     {0x01, 6},
     {0x01, 5},
     {0x01, 4},
     {0x01, 3},
     {0x01, 1},
     {0x03, 3},
     {0x05, 4},
     {0x09, 5},
     {0x11, 6},
     {0x21, 7},
     {0x41, 8},
     {0x81, 9}}};

/// Number of valid symbols in each of the three codebooks above. Upstream
/// always passes 18 to init_vlc() and relies on the trailing rows of the
/// shorter tables being zero-length and therefore skipped.
constexpr int MLP_HUFF_SYMBOLS = 18;

struct ChannelInformation
{
  uint8_t channel_occupancy;
  uint8_t group1_channels;
  uint8_t group2_channels;
  uint8_t summary_info;
};

const ChannelInformation ff_mlp_ch_info[21] = {
    {0x01, 0x01, 0x00, 0x1f}, {0x03, 0x02, 0x00, 0x1b}, {0x07, 0x02, 0x01, 0x1f},
    {0x0F, 0x02, 0x02, 0x19}, {0x07, 0x02, 0x01, 0x03}, {0x0F, 0x02, 0x02, 0x1f},
    {0x1F, 0x02, 0x03, 0x01}, {0x07, 0x02, 0x01, 0x1a}, {0x0F, 0x02, 0x02, 0x1f},
    {0x1F, 0x02, 0x03, 0x18}, {0x0F, 0x02, 0x02, 0x02}, {0x1F, 0x02, 0x03, 0x1f},
    {0x3F, 0x02, 0x04, 0x00}, {0x0F, 0x03, 0x01, 0x1f}, {0x1F, 0x03, 0x02, 0x18},
    {0x0F, 0x03, 0x01, 0x02}, {0x1F, 0x03, 0x02, 0x1f}, {0x3F, 0x03, 0x03, 0x00},
    {0x1F, 0x04, 0x01, 0x01}, {0x1F, 0x04, 0x01, 0x18}, {0x3F, 0x04, 0x02, 0x00},
};

/** XOR four bytes into one. */
inline uint8_t xor_32_to_8(uint32_t value)
{
  value ^= value >> 16;
  value ^= value >> 8;
  return static_cast<uint8_t>(value);
}

AVCRC crc_63[CRC_TABLE_SIZE];
AVCRC crc_1D[CRC_TABLE_SIZE];
AVCRC crc_2D[CRC_TABLE_SIZE];

/** MLP uses checksums that seem to be based on the standard CRC algorithm, but
 *  are not (in implementation terms, the table lookup and XOR are reversed).
 *  We can implement this behavior using a standard av_crc on all but the
 *  last element, then XOR that with the last element.
 */
uint16_t ff_mlp_checksum16(const uint8_t *buf, unsigned int buf_size)
{
  uint16_t crc;

  crc = static_cast<uint16_t>(av_crc(crc_2D, 0, buf, buf_size - 2));
  crc ^= AV_RL16(buf + buf_size - 2);
  return crc;
}

uint8_t ff_mlp_checksum8(const uint8_t *buf, unsigned int buf_size)
{
  uint8_t checksum =
      static_cast<uint8_t>(av_crc(crc_63, 0x3c, buf, buf_size - 1)); // crc_63[0xa2] == 0x3c
  checksum ^= buf[buf_size - 1];
  return checksum;
}

/** Calculate an 8-bit checksum over a restart header -- a non-multiple-of-8
 *  number of bits, starting two bits into the first byte of buf.
 */
uint8_t ff_mlp_restart_checksum(const uint8_t *buf, unsigned int bit_size)
{
  int i;
  int num_bytes = (bit_size + 2) / 8;

  int crc = crc_1D[buf[0] & 0x3f];
  crc = av_crc(crc_1D, crc, buf + 1, num_bytes - 2);
  crc ^= buf[num_bytes - 1];

  for (i = 0; i < (static_cast<int>(bit_size + 2) & 7); i++)
  {
    crc <<= 1;
    if (crc & 0x100) crc ^= 0x11D;
    crc ^= (buf[num_bytes] >> (7 - i)) & 1;
  }

  return static_cast<uint8_t>(crc);
}

/** XOR together all the bytes of a buffer.
 *
 * Upstream hand-unrolls this four bytes at a time through a uint32_t pointer,
 * which needs the buffer to be 4-byte aligned and is undefined behaviour when
 * it is not. The byte-at-a-time loop below computes the same value: XOR is
 * associative and the wide path folds back down through xor_32_to_8() anyway.
 */
uint8_t ff_mlp_calculate_parity(const uint8_t *buf, unsigned int buf_size)
{
  uint8_t scratch = 0;

  for (unsigned int i = 0; i < buf_size; i++)
  {
    scratch ^= buf[i];
  }

  return scratch;
}

// ---------------------------------------------------------------------------
// Major sync header (libavcodec/mlp_parse.c)
// ---------------------------------------------------------------------------

struct MLPHeaderInfo
{
  int stream_type;                     ///< 0xBB for MLP, 0xBA for TrueHD
  int header_size;                     ///< Size of the major sync header, in bytes

  int group1_bits;                     ///< The bit depth of the first substream
  int group2_bits;                     ///< Bit depth of the second substream (MLP only)

  int group1_samplerate;               ///< Sample rate of first substream
  int group2_samplerate;               ///< Sample rate of second substream (MLP only)

  int channel_arrangement;

  int channel_modifier_thd_stream0;    ///< Channel modifier for substream 0 of TrueHD
  int channel_modifier_thd_stream1;    ///< Channel modifier for substream 1 of TrueHD
  int channel_modifier_thd_stream2;    ///< Channel modifier for substream 2 of TrueHD

  int channels_mlp;                    ///< Channel count for MLP streams
  int channels_thd_stream1;            ///< Channel count for substream 1 of TrueHD
  int channels_thd_stream2;            ///< Channel count for substream 2 of TrueHD
  uint64_t channel_layout_mlp;         ///< Channel layout for MLP streams
  uint64_t channel_layout_thd_stream1; ///< Channel layout for substream 1 of TrueHD
  uint64_t channel_layout_thd_stream2; ///< Channel layout for substream 2 of TrueHD

  int access_unit_size;                ///< Number of samples per coded frame
  int access_unit_size_pow2;           ///< Next power of two above samples per frame

  int is_vbr;                          ///< Stream is VBR instead of CBR
  int peak_bitrate;                    ///< Peak bitrate for VBR, actual bitrate for CBR

  int num_substreams;                  ///< Number of substreams within stream
};

const uint8_t mlp_quants[16] = {
    16, 20, 24, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

const uint8_t mlp_channels[32] = {
    1, 2, 3, 4, 3, 4, 5, 3, 4, 5, 4, 5, 6, 4, 5, 4,
    5, 6, 5, 5, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

const uint64_t mlp_layout[32] = {
    AV_CH_LAYOUT_MONO,
    AV_CH_LAYOUT_STEREO,
    AV_CH_LAYOUT_2_1,
    AV_CH_LAYOUT_QUAD,
    AV_CH_LAYOUT_STEREO | AV_CH_LOW_FREQUENCY,
    AV_CH_LAYOUT_2_1 | AV_CH_LOW_FREQUENCY,
    AV_CH_LAYOUT_QUAD | AV_CH_LOW_FREQUENCY,
    AV_CH_LAYOUT_SURROUND,
    AV_CH_LAYOUT_4POINT0,
    AV_CH_LAYOUT_5POINT0_BACK,
    AV_CH_LAYOUT_SURROUND | AV_CH_LOW_FREQUENCY,
    AV_CH_LAYOUT_4POINT0 | AV_CH_LOW_FREQUENCY,
    AV_CH_LAYOUT_5POINT1_BACK,
    AV_CH_LAYOUT_4POINT0,
    AV_CH_LAYOUT_5POINT0_BACK,
    AV_CH_LAYOUT_SURROUND | AV_CH_LOW_FREQUENCY,
    AV_CH_LAYOUT_4POINT0 | AV_CH_LOW_FREQUENCY,
    AV_CH_LAYOUT_5POINT1_BACK,
    AV_CH_LAYOUT_QUAD | AV_CH_LOW_FREQUENCY,
    AV_CH_LAYOUT_5POINT0_BACK,
    AV_CH_LAYOUT_5POINT1_BACK,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

const uint8_t thd_chancount[13] = {
    //  LR    C   LFE  LRs LRvh  LRc LRrs  Cs   Ts  LRsd  LRw  Cvh  LFE2
    2, 1, 1, 2, 2, 2, 2, 1, 1, 2, 2, 1, 1};

const uint64_t thd_layout[13] = {
    AV_CH_FRONT_LEFT | AV_CH_FRONT_RIGHT,                     // LR
    AV_CH_FRONT_CENTER,                                       // C
    AV_CH_LOW_FREQUENCY,                                      // LFE
    AV_CH_SIDE_LEFT | AV_CH_SIDE_RIGHT,                       // LRs
    AV_CH_TOP_FRONT_LEFT | AV_CH_TOP_FRONT_RIGHT,             // LRvh
    AV_CH_FRONT_LEFT_OF_CENTER | AV_CH_FRONT_RIGHT_OF_CENTER, // LRc
    AV_CH_BACK_LEFT | AV_CH_BACK_RIGHT,                       // LRrs
    AV_CH_BACK_CENTER,                                        // Cs
    AV_CH_TOP_CENTER,                                         // Ts
    AV_CH_SURROUND_DIRECT_LEFT | AV_CH_SURROUND_DIRECT_RIGHT, // LRsd
    AV_CH_WIDE_LEFT | AV_CH_WIDE_RIGHT,                       // LRw
    AV_CH_TOP_FRONT_CENTER,                                   // Cvh
    AV_CH_LOW_FREQUENCY_2,                                    // LFE2
};

inline int mlp_samplerate(int in)
{
  if (in == 0xF) return 0;

  return (in & 8 ? 44100 : 48000) << (in & 7);
}

inline int truehd_channels(int chanmap)
{
  int channels = 0, i;

  for (i = 0; i < 13; i++)
  {
    channels += thd_chancount[i] * ((chanmap >> i) & 1);
  }

  return channels;
}

inline uint64_t truehd_layout(int chanmap)
{
  int i;
  uint64_t layout = 0;

  for (i = 0; i < 13; i++)
  {
    layout |= thd_layout[i] * ((chanmap >> i) & 1);
  }

  return layout;
}

int mlp_get_major_sync_size(const uint8_t *buf, int bufsize)
{
  int has_extension, extensions = 0;
  int size = 28;
  if (bufsize < 28) return -1;

  if (AV_RB32(buf) == 0xf8726fba)
  {
    has_extension = buf[25] & 1;
    if (has_extension)
    {
      extensions = buf[26] >> 4;
      size += 2 + extensions * 2;
    }
  }
  return size;
}

/** Read a major sync info header - contains high level information about
 *  the stream - sample rate, channel arrangement etc. Most of this
 *  information is not actually necessary for decoding, only for playback.
 *  gb must be a freshly initialized GetBitContext with no bits read.
 */
int ff_mlp_read_major_sync(MLPHeaderInfo *mh, GetBitContext *gb)
{
  int ratebits, channel_arrangement, header_size;
  uint16_t checksum;

  header_size = mlp_get_major_sync_size(gb->buffer, gb->size_in_bits >> 3);
  if (header_size < 0 || gb->size_in_bits < header_size << 3)
  {
    debug("packet too short, unable to read major sync");
    return -1;
  }

  checksum = ff_mlp_checksum16(gb->buffer, header_size - 2);
  if (checksum != AV_RL16(gb->buffer + header_size - 2))
  {
    debug("major sync info header checksum error");
    return AVERROR_INVALIDDATA;
  }

  if (get_bits(gb, 24) != 0xf8726f) /* Sync words */
  {
    return AVERROR_INVALIDDATA;
  }

  mh->stream_type = get_bits(gb, 8);
  mh->header_size = header_size;

  if (mh->stream_type == 0xbb)
  {
    mh->group1_bits = mlp_quants[get_bits(gb, 4)];
    mh->group2_bits = mlp_quants[get_bits(gb, 4)];

    ratebits = get_bits(gb, 4);
    mh->group1_samplerate = mlp_samplerate(ratebits);
    mh->group2_samplerate = mlp_samplerate(get_bits(gb, 4));

    skip_bits(gb, 11);

    mh->channel_arrangement = channel_arrangement = get_bits(gb, 5);
    mh->channels_mlp = mlp_channels[channel_arrangement];
    mh->channel_layout_mlp = mlp_layout[channel_arrangement];
  }
  else if (mh->stream_type == 0xba)
  {
    mh->group1_bits = 24; // TODO: Is this information actually conveyed anywhere?
    mh->group2_bits = 0;

    ratebits = get_bits(gb, 4);
    mh->group1_samplerate = mlp_samplerate(ratebits);
    mh->group2_samplerate = 0;

    skip_bits(gb, 4);

    mh->channel_modifier_thd_stream0 = get_bits(gb, 2);
    mh->channel_modifier_thd_stream1 = get_bits(gb, 2);

    mh->channel_arrangement = channel_arrangement = get_bits(gb, 5);
    mh->channels_thd_stream1 = truehd_channels(channel_arrangement);
    mh->channel_layout_thd_stream1 = truehd_layout(channel_arrangement);

    mh->channel_modifier_thd_stream2 = get_bits(gb, 2);

    channel_arrangement = get_bits(gb, 13);
    mh->channels_thd_stream2 = truehd_channels(channel_arrangement);
    mh->channel_layout_thd_stream2 = truehd_layout(channel_arrangement);
  }
  else
  {
    return AVERROR_INVALIDDATA;
  }

  mh->access_unit_size = 40 << (ratebits & 7);
  mh->access_unit_size_pow2 = 64 << (ratebits & 7);

  skip_bits_long(gb, 48);

  mh->is_vbr = get_bits1(gb);

  mh->peak_bitrate = (get_bits(gb, 15) * mh->group1_samplerate + 8) >> 4;

  mh->num_substreams = get_bits(gb, 4);

  skip_bits_long(gb, 4 + (header_size - 17) * 8);

  return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// Filter / rematrix / output packing (libavcodec/mlpdsp.c)
//
// Upstream reaches these through MLPDSPContext function pointers so that the
// x86 and ARM builds can substitute SIMD versions. mocf has no such variants,
// so they are called directly and the dispatch table is gone.
// ---------------------------------------------------------------------------

namespace
{

void mlp_filter_channel(int32_t *state, const int32_t *coeff, int firorder,
                        int iirorder, unsigned int filter_shift, int32_t mask,
                        int blocksize, int32_t *sample_buffer)
{
  int32_t *firbuf = state;
  int32_t *iirbuf = state + MAX_BLOCKSIZE + MAX_FIR_ORDER;
  const int32_t *fircoeff = coeff;
  const int32_t *iircoeff = coeff + MAX_FIR_ORDER;
  int i;

  for (i = 0; i < blocksize; i++)
  {
    int32_t residual = *sample_buffer;
    unsigned int order;
    int64_t accum = 0;
    int32_t result;

    for (order = 0; order < static_cast<unsigned>(firorder); order++)
    {
      accum += static_cast<int64_t>(firbuf[order]) * fircoeff[order];
    }
    for (order = 0; order < static_cast<unsigned>(iirorder); order++)
    {
      accum += static_cast<int64_t>(iirbuf[order]) * iircoeff[order];
    }

    accum = accum >> filter_shift;
    result = (accum + residual) & mask;

    *--firbuf = result;
    *--iirbuf = result - accum;

    *sample_buffer = result;
    sample_buffer += MAX_CHANNELS;
  }
}

void ff_mlp_rematrix_channel(int32_t *samples, const int32_t *coeffs,
                             const uint8_t *bypassed_lsbs, const int8_t *noise_buffer,
                             int index, unsigned int dest_ch, uint16_t blockpos,
                             unsigned int maxchan, int matrix_noise_shift,
                             int access_unit_size_pow2, int32_t mask)
{
  unsigned int src_ch, i;
  int index2 = 2 * index + 1;
  for (i = 0; i < blockpos; i++)
  {
    int64_t accum = 0;

    for (src_ch = 0; src_ch <= maxchan; src_ch++)
    {
      accum += static_cast<int64_t>(samples[src_ch]) * coeffs[src_ch];
    }

    if (matrix_noise_shift)
    {
      index &= access_unit_size_pow2 - 1;
      accum += noise_buffer[index] * (1 << (matrix_noise_shift + 7));
      index += index2;
    }

    samples[dest_ch] = ((accum >> 14) & mask) + *bypassed_lsbs;
    bypassed_lsbs += MAX_CHANNELS;
    samples += MAX_CHANNELS;
  }
}

int32_t ff_mlp_pack_output(int32_t lossless_check_data, uint16_t blockpos,
                           int32_t (*sample_buffer)[MAX_CHANNELS], void *data,
                           uint8_t *ch_assign, int8_t *output_shift,
                           uint8_t max_matrix_channel, int is32)
{
  unsigned int i, out_ch = 0;
  int32_t *data_32 = static_cast<int32_t *>(data);
  int16_t *data_16 = static_cast<int16_t *>(data);

  for (i = 0; i < blockpos; i++)
  {
    for (out_ch = 0; out_ch <= max_matrix_channel; out_ch++)
    {
      int mat_ch = ch_assign[out_ch];
      int32_t sample = sample_buffer[i][mat_ch] * (1U << output_shift[mat_ch]);
      lossless_check_data ^= (sample & 0xffffff) << mat_ch;
      if (is32)
      {
        *data_32++ = sample * 256U;
      }
      else
      {
        *data_16++ = sample >> 8;
      }
    }
  }
  return lossless_check_data;
}

} // namespace

// ---------------------------------------------------------------------------
// Static tables, shared by every decoder instance
// ---------------------------------------------------------------------------

namespace
{

Vlc huff_vlc[3];

/** Initialize static data, constant between all invocations of the codec. */
void init_static()
{
  for (int i = 0; i < 3; i++)
  {
    vlc_init(&huff_vlc[i], &ff_mlp_huffman_tables[i][0], MLP_HUFF_SYMBOLS);
  }

  av_crc_init(crc_63, 0, 8, 0x63);
  av_crc_init(crc_1D, 0, 8, 0x1D);
  av_crc_init(crc_2D, 0, 16, 0x002D);
}

std::once_flag init_static_once;

} // namespace

namespace
{

struct SubStream {
    /// Set if a valid restart header has been read. Otherwise the substream cannot be decoded.
    uint8_t     restart_seen;

    //@{
    /** restart header data */
    /// The type of noise to be used in the rematrix stage.
    uint16_t    noise_type;

    /// The index of the first channel coded in this substream.
    uint8_t     min_channel;
    /// The index of the last channel coded in this substream.
    uint8_t     max_channel;
    /// The number of channels input into the rematrix stage.
    uint8_t     max_matrix_channel;
    /// For each channel output by the matrix, the output channel to map it to
    uint8_t     ch_assign[MAX_CHANNELS];
    /// The channel layout for this substream
    uint64_t    mask;
    /// The matrix encoding mode for this substream
    enum AVMatrixEncoding matrix_encoding;

    /// Channel coding parameters for channels in the substream
    ChannelParams channel_params[MAX_CHANNELS];

    /// The left shift applied to random noise in 0x31ea substreams.
    uint8_t     noise_shift;
    /// The current seed value for the pseudorandom noise generator(s).
    uint32_t    noisegen_seed;

    /// Set if the substream contains extra info to check the size of VLC blocks.
    uint8_t     data_check_present;

    /// Bitmask of which parameter sets are conveyed in a decoding parameter block.
    uint8_t     param_presence_flags;
#define PARAM_BLOCKSIZE     (1 << 7)
#define PARAM_MATRIX        (1 << 6)
#define PARAM_OUTSHIFT      (1 << 5)
#define PARAM_QUANTSTEP     (1 << 4)
#define PARAM_FIR           (1 << 3)
#define PARAM_IIR           (1 << 2)
#define PARAM_HUFFOFFSET    (1 << 1)
#define PARAM_PRESENCE      (1 << 0)
    //@}

    //@{
    /** matrix data */

    /// Number of matrices to be applied.
    uint8_t     num_primitive_matrices;

    /// matrix output channel
    uint8_t     matrix_out_ch[MAX_MATRICES];

    /// Whether the LSBs of the matrix output are encoded in the bitstream.
    uint8_t     lsb_bypass[MAX_MATRICES];
    /// Matrix coefficients, stored as 2.14 fixed point.
    alignas(32) int32_t matrix_coeff[MAX_MATRICES][MAX_CHANNELS];
    /// Left shift to apply to noise values in 0x31eb substreams.
    uint8_t     matrix_noise_shift[MAX_MATRICES];
    //@}

    /// Left shift to apply to Huffman-decoded residuals.
    uint8_t     quant_step_size[MAX_CHANNELS];

    /// number of PCM samples in current audio block
    uint16_t    blocksize;
    /// Number of PCM samples decoded so far in this frame.
    uint16_t    blockpos;

    /// Left shift to apply to decoded PCM values to get final 24-bit output.
    int8_t      output_shift[MAX_CHANNELS];

    /// Running XOR of all output samples.
    int32_t     lossless_check_data;

};

struct MLPDecodeContext {
/// Only ever nullptr. Kept so upstream's av_log(m->avctx, ...) call
    /// sites compile unchanged; the shim discards it.
    void        *avctx;

    /// Current access unit being read has a major sync.
    int         is_major_sync_unit;

    /// Size of the major sync unit, in bytes
    int         major_sync_header_size;

    /// Set if a valid major sync block has been read. Otherwise no decoding is possible.
    uint8_t     params_valid;

    /// Number of substreams contained within this stream.
    uint8_t     num_substreams;

    /// Index of the last substream to decode - further substreams are skipped.
    uint8_t     max_decoded_substream;

    /// Stream needs channel reordering to comply with FFmpeg's channel order
    uint8_t     needs_reordering;

    /// number of PCM samples contained in each frame
    int         access_unit_size;
    /// next power of two above the number of samples in each frame
    int         access_unit_size_pow2;

    SubStream   substream[MAX_SUBSTREAMS];

    int         matrix_changed;
    int         filter_changed[MAX_CHANNELS][NUM_FILTERS];

    int8_t      noise_buffer[MAX_BLOCKSIZE_POW2];
    int8_t      bypassed_lsbs[MAX_BLOCKSIZE][MAX_CHANNELS];
    alignas(32) int32_t sample_buffer[MAX_BLOCKSIZE][MAX_CHANNELS];

    /* Stream properties. Upstream keeps these on the AVCodecContext; the
     * decoder both reads and updates them as major syncs and restart headers
     * are parsed, so they live here instead. */

    /// 0 for MLP (sync 0xbb), 1 for TrueHD (sync 0xba). Upstream distinguishes
    /// the two by the AVCodecContext codec_id.
    int         is_truehd;
    /// Output samples are int32_t (24 significant bits) rather than int16_t.
    int         is32;
    int         sample_rate;
    int         channels;
    uint64_t    channel_layout;
    int         bits_per_raw_sample;
    /// Samples per access unit; mirrors access_unit_size.
    int         frame_size;
    /// Requested downmix. When a substream's layout is a superset of this,
    /// decoding stops at that substream and the later ones are skipped -- for
    /// TrueHD that yields the native 2-channel presentation rather than
    /// decoding 5.1/7.1 only to mix it back down.
    uint64_t    request_channel_layout;

    /// Decoded PCM for the last access unit, interleaved. Sized for the
    /// format's maximum: read_major_sync() rejects any stream whose
    /// access_unit_size exceeds MAX_BLOCKSIZE, and max_matrix_channel is
    /// bounded by MAX_CHANNELS, so this never overflows.
    alignas(32) int32_t out_buffer[MAX_BLOCKSIZE * MAX_CHANNELS];
    int         out_nb_samples;
};

static const uint64_t thd_channel_order[] = {
    AV_CH_FRONT_LEFT, AV_CH_FRONT_RIGHT,                     // LR
    AV_CH_FRONT_CENTER,                                      // C
    AV_CH_LOW_FREQUENCY,                                     // LFE
    AV_CH_SIDE_LEFT, AV_CH_SIDE_RIGHT,                       // LRs
    AV_CH_TOP_FRONT_LEFT, AV_CH_TOP_FRONT_RIGHT,             // LRvh
    AV_CH_FRONT_LEFT_OF_CENTER, AV_CH_FRONT_RIGHT_OF_CENTER, // LRc
    AV_CH_BACK_LEFT, AV_CH_BACK_RIGHT,                       // LRrs
    AV_CH_BACK_CENTER,                                       // Cs
    AV_CH_TOP_CENTER,                                        // Ts
    AV_CH_SURROUND_DIRECT_LEFT, AV_CH_SURROUND_DIRECT_RIGHT, // LRsd
    AV_CH_WIDE_LEFT, AV_CH_WIDE_RIGHT,                       // LRw
    AV_CH_TOP_FRONT_CENTER,                                  // Cvh
    AV_CH_LOW_FREQUENCY_2,                                   // LFE2
};

static int mlp_channel_layout_subset(uint64_t channel_layout, uint64_t mask)
{
    return channel_layout && ((channel_layout & mask) == channel_layout);
}

static uint64_t thd_channel_layout_extract_channel(uint64_t channel_layout,
                                                   int index)
{
    int i;

    if (av_get_channel_layout_nb_channels(channel_layout) <= index)
        return 0;

    for (i = 0; i < FF_ARRAY_ELEMS(thd_channel_order); i++)
        if (channel_layout & thd_channel_order[i] && !index--)
            return thd_channel_order[i];
    return 0;
}

static inline int32_t calculate_sign_huff(MLPDecodeContext *m,
                                          unsigned int substr, unsigned int ch)
{
    SubStream *s = &m->substream[substr];
    ChannelParams *cp = &s->channel_params[ch];
    int lsb_bits = cp->huff_lsbs - s->quant_step_size[ch];
    int sign_shift = lsb_bits + (cp->codebook ? 2 - cp->codebook : -1);
    int32_t sign_huff_offset = cp->huff_offset;

    if (cp->codebook > 0)
        sign_huff_offset -= 7 << lsb_bits;

    if (sign_shift >= 0)
        sign_huff_offset -= 1 << sign_shift;

    return sign_huff_offset;
}

/** Read a sample, consisting of either, both or neither of entropy-coded MSBs
 *  and plain LSBs. */

static inline int read_huff_channels(MLPDecodeContext *m, GetBitContext *gbp,
                                     unsigned int substr, unsigned int pos)
{
    SubStream *s = &m->substream[substr];
    unsigned int mat, channel;

    for (mat = 0; mat < s->num_primitive_matrices; mat++)
        if (s->lsb_bypass[mat])
            m->bypassed_lsbs[pos + s->blockpos][mat] = get_bits1(gbp);

    for (channel = s->min_channel; channel <= s->max_channel; channel++) {
        ChannelParams *cp = &s->channel_params[channel];
        int codebook = cp->codebook;
        int quant_step_size = s->quant_step_size[channel];
        int lsb_bits = cp->huff_lsbs - quant_step_size;
        int result = 0;

        if (codebook > 0)
            result = get_vlc2(gbp, &huff_vlc[codebook-1]);

        if (result < 0)
            return AVERROR_INVALIDDATA;

        if (lsb_bits > 0)
            result = (result << lsb_bits) + get_bits_long(gbp, lsb_bits);

        result  += cp->sign_huff_offset;
        result *= 1 << quant_step_size;

        m->sample_buffer[pos + s->blockpos][channel] = result;
    }

    return 0;
}

static int read_major_sync(MLPDecodeContext *m, GetBitContext *gb)
{
    MLPHeaderInfo mh;
    int substr, ret;

    if ((ret = ff_mlp_read_major_sync(&mh, gb)) != 0)
        return ret;

    if (mh.group1_bits == 0) {
        av_log(m->avctx, AV_LOG_ERROR, "invalid/unknown bits per sample\n");
        return AVERROR_INVALIDDATA;
    }
    if (mh.group2_bits > mh.group1_bits) {
        av_log(m->avctx, AV_LOG_ERROR,
               "Channel group 2 cannot have more bits per sample than group 1.\n");
        return AVERROR_INVALIDDATA;
    }

    if (mh.group2_samplerate && mh.group2_samplerate != mh.group1_samplerate) {
        av_log(m->avctx, AV_LOG_ERROR,
               "Channel groups with differing sample rates are not currently supported.\n");
        return AVERROR_INVALIDDATA;
    }

    if (mh.group1_samplerate == 0) {
        av_log(m->avctx, AV_LOG_ERROR, "invalid/unknown sampling rate\n");
        return AVERROR_INVALIDDATA;
    }
    if (mh.group1_samplerate > MAX_SAMPLERATE) {
        av_log(m->avctx, AV_LOG_ERROR,
               "Sampling rate %d is greater than the supported maximum (%d).\n",
               mh.group1_samplerate, MAX_SAMPLERATE);
        return AVERROR_INVALIDDATA;
    }
    if (mh.access_unit_size > MAX_BLOCKSIZE) {
        av_log(m->avctx, AV_LOG_ERROR,
               "Block size %d is greater than the supported maximum (%d).\n",
               mh.access_unit_size, MAX_BLOCKSIZE);
        return AVERROR_INVALIDDATA;
    }
    if (mh.access_unit_size_pow2 > MAX_BLOCKSIZE_POW2) {
        av_log(m->avctx, AV_LOG_ERROR,
               "Block size pow2 %d is greater than the supported maximum (%d).\n",
               mh.access_unit_size_pow2, MAX_BLOCKSIZE_POW2);
        return AVERROR_INVALIDDATA;
    }

    if (mh.num_substreams == 0)
        return AVERROR_INVALIDDATA;
    if (!m->is_truehd && mh.num_substreams > 2) {
        av_log(m->avctx, AV_LOG_ERROR, "MLP only supports up to 2 substreams.\n");
        return AVERROR_INVALIDDATA;
    }
    if (mh.num_substreams > MAX_SUBSTREAMS) {
        avpriv_request_sample(m->avctx,
                              "%d substreams (more than the "
                              "maximum supported by the decoder)",
                              mh.num_substreams);
        return AVERROR_PATCHWELCOME;
    }

    m->major_sync_header_size = mh.header_size;

    m->access_unit_size      = mh.access_unit_size;
    m->access_unit_size_pow2 = mh.access_unit_size_pow2;

    m->num_substreams        = mh.num_substreams;

    /* limit to decoding 3 substreams, as the 4th is used by Dolby Atmos for non-audio data */
    m->max_decoded_substream = FFMIN(m->num_substreams - 1, 2);

    m->sample_rate    = mh.group1_samplerate;
    m->frame_size     = mh.access_unit_size;

    m->bits_per_raw_sample = mh.group1_bits;
    if (mh.group1_bits > 16)
        m->is32 = 1;
    else
        m->is32 = 0;

    m->params_valid = 1;
    for (substr = 0; substr < MAX_SUBSTREAMS; substr++)
        m->substream[substr].restart_seen = 0;

    /* Set the layout for each substream. When there's more than one, the first
     * substream is Stereo. Subsequent substreams' layouts are indicated in the
     * major sync. */
    if (!m->is_truehd) {
        if (mh.stream_type != 0xbb) {
            avpriv_request_sample(m->avctx,
                        "unexpected stream_type %X in MLP",
                        mh.stream_type);
            return AVERROR_PATCHWELCOME;
        }
        if ((substr = (mh.num_substreams > 1)))
            m->substream[0].mask = AV_CH_LAYOUT_STEREO;
        m->substream[substr].mask = mh.channel_layout_mlp;
    } else {
        if (mh.stream_type != 0xba) {
            avpriv_request_sample(m->avctx,
                        "unexpected stream_type %X in !MLP",
                        mh.stream_type);
            return AVERROR_PATCHWELCOME;
        }
        if ((substr = (mh.num_substreams > 1)))
            m->substream[0].mask = AV_CH_LAYOUT_STEREO;
        if (mh.num_substreams > 2) {
            if (mh.channel_layout_thd_stream2)
                m->substream[2].mask = mh.channel_layout_thd_stream2;
            else
                m->substream[2].mask = mh.channel_layout_thd_stream1;
        }
        m->substream[substr].mask = mh.channel_layout_thd_stream1;

        if (m->channels<=2 && m->substream[substr].mask == AV_CH_LAYOUT_MONO && m->max_decoded_substream == 1) {
            av_log(m->avctx, AV_LOG_DEBUG, "Mono stream with 2 substreams, ignoring 2nd\n");
            m->max_decoded_substream = 0;
            if (m->channels==2)
                m->channel_layout = AV_CH_LAYOUT_STEREO;
        }
    }

    m->needs_reordering = mh.channel_arrangement >= 18 && mh.channel_arrangement <= 20;

    /* Parse the TrueHD decoder channel modifiers and set each substream's
     * AVMatrixEncoding accordingly.
     *
     * The meaning of the modifiers depends on the channel layout:
     *
     * - THD_CH_MODIFIER_LTRT, THD_CH_MODIFIER_LBINRBIN only apply to 2-channel
     *
     * - THD_CH_MODIFIER_MONO applies to 1-channel or 2-channel (dual mono)
     *
     * - THD_CH_MODIFIER_SURROUNDEX, THD_CH_MODIFIER_NOTSURROUNDEX only apply to
     *   layouts with an Ls/Rs channel pair
     */
    for (substr = 0; substr < MAX_SUBSTREAMS; substr++)
        m->substream[substr].matrix_encoding = AV_MATRIX_ENCODING_NONE;
    if (m->is_truehd) {
        if (mh.num_substreams > 2 &&
            mh.channel_layout_thd_stream2 & AV_CH_SIDE_LEFT &&
            mh.channel_layout_thd_stream2 & AV_CH_SIDE_RIGHT &&
            mh.channel_modifier_thd_stream2 == THD_CH_MODIFIER_SURROUNDEX)
            m->substream[2].matrix_encoding = AV_MATRIX_ENCODING_DOLBYEX;

        if (mh.num_substreams > 1 &&
            mh.channel_layout_thd_stream1 & AV_CH_SIDE_LEFT &&
            mh.channel_layout_thd_stream1 & AV_CH_SIDE_RIGHT &&
            mh.channel_modifier_thd_stream1 == THD_CH_MODIFIER_SURROUNDEX)
            m->substream[1].matrix_encoding = AV_MATRIX_ENCODING_DOLBYEX;

        if (mh.num_substreams > 0)
            switch (mh.channel_modifier_thd_stream0) {
            case THD_CH_MODIFIER_LTRT:
                m->substream[0].matrix_encoding = AV_MATRIX_ENCODING_DOLBY;
                break;
            case THD_CH_MODIFIER_LBINRBIN:
                m->substream[0].matrix_encoding = AV_MATRIX_ENCODING_DOLBYHEADPHONE;
                break;
            default:
                break;
            }
    }

    return 0;
}

/** Read a restart header from a block in a substream. This contains parameters
 *  required to decode the audio that do not change very often. Generally
 *  (always) present only in blocks following a major sync. */

static int read_restart_header(MLPDecodeContext *m, GetBitContext *gbp,
                               const uint8_t *buf, unsigned int substr)
{
    SubStream *s = &m->substream[substr];
    unsigned int ch;
    int sync_word, tmp;
    uint8_t checksum;
    uint8_t lossless_check;
    int start_count = get_bits_count(gbp);
    int min_channel, max_channel, max_matrix_channel, noise_type;
    const int std_max_matrix_channel = !m->is_truehd
                                     ? MAX_MATRIX_CHANNEL_MLP
                                     : MAX_MATRIX_CHANNEL_TRUEHD;

    sync_word = get_bits(gbp, 13);

    if (sync_word != 0x31ea >> 1) {
        av_log(m->avctx, AV_LOG_ERROR,
               "restart header sync incorrect (got 0x%04x)\n", sync_word);
        return AVERROR_INVALIDDATA;
    }

    noise_type = get_bits1(gbp);

    if (!m->is_truehd && noise_type) {
        av_log(m->avctx, AV_LOG_ERROR, "MLP must have 0x31ea sync word.\n");
        return AVERROR_INVALIDDATA;
    }

    skip_bits(gbp, 16); /* Output timestamp */

    min_channel        = get_bits(gbp, 4);
    max_channel        = get_bits(gbp, 4);
    max_matrix_channel = get_bits(gbp, 4);

    if (max_matrix_channel > std_max_matrix_channel) {
        av_log(m->avctx, AV_LOG_ERROR,
               "Max matrix channel cannot be greater than %d.\n",
               std_max_matrix_channel);
        return AVERROR_INVALIDDATA;
    }

    if (max_channel != max_matrix_channel) {
        av_log(m->avctx, AV_LOG_ERROR,
               "Max channel must be equal max matrix channel.\n");
        return AVERROR_INVALIDDATA;
    }

    /* This should happen for TrueHD streams with >6 channels and MLP's noise
     * type. It is not yet known if this is allowed. */
    if (max_matrix_channel > MAX_MATRIX_CHANNEL_MLP && !noise_type) {
        avpriv_request_sample(m->avctx,
                              "%d channels (more than the "
                              "maximum supported by the decoder)",
                              max_channel + 2);
        return AVERROR_PATCHWELCOME;
    }

    if (min_channel > max_channel) {
        av_log(m->avctx, AV_LOG_ERROR,
               "Substream min channel cannot be greater than max channel.\n");
        return AVERROR_INVALIDDATA;
    }

    if (max_channel + 1 > MAX_CHANNELS || max_channel + 1 < min_channel)
        return AVERROR_INVALIDDATA;


    s->min_channel        = min_channel;
    s->max_channel        = max_channel;
    s->max_matrix_channel = max_matrix_channel;
    s->noise_type         = noise_type;

    if (mlp_channel_layout_subset(m->request_channel_layout, s->mask) &&
        m->max_decoded_substream > substr) {
        av_log(m->avctx, AV_LOG_DEBUG,
               "Extracting %d-channel downmix (0x%" PRIx64 ") from substream %d. "
               "Further substreams will be skipped.\n",
               s->max_channel + 1, s->mask, substr);
        m->max_decoded_substream = substr;
    }

    s->noise_shift   = get_bits(gbp,  4);
    s->noisegen_seed = get_bits(gbp, 23);

    skip_bits(gbp, 19);

    s->data_check_present = get_bits1(gbp);
    lossless_check = get_bits(gbp, 8);
    if (substr == m->max_decoded_substream
        && static_cast<uint32_t>(s->lossless_check_data) != 0xffffffffu) {
        tmp = xor_32_to_8(s->lossless_check_data);
        if (tmp != lossless_check)
            av_log(m->avctx, AV_LOG_WARNING,
                   "Lossless check failed - expected %02x, calculated %02x.\n",
                   lossless_check, tmp);
    }

    skip_bits(gbp, 16);

    memset(s->ch_assign, 0, sizeof(s->ch_assign));

    for (ch = 0; ch <= s->max_matrix_channel; ch++) {
        int ch_assign = get_bits(gbp, 6);
        if (m->is_truehd) {
            uint64_t channel = thd_channel_layout_extract_channel(s->mask,
                                                                  ch_assign);
            ch_assign = av_get_channel_layout_channel_index(s->mask,
                                                            channel);
        }
        if (ch_assign < 0 || ch_assign > s->max_matrix_channel) {
            avpriv_request_sample(m->avctx,
                                  "Assignment of matrix channel %d to invalid output channel %d",
                                  ch, ch_assign);
            return AVERROR_PATCHWELCOME;
        }
        s->ch_assign[ch_assign] = ch;
    }

    checksum = ff_mlp_restart_checksum(buf, get_bits_count(gbp) - start_count);

    if (checksum != get_bits(gbp, 8))
        av_log(m->avctx, AV_LOG_ERROR, "restart header checksum error\n");

    /* Set default decoding parameters. */
    s->param_presence_flags   = 0xff;
    s->num_primitive_matrices = 0;
    s->blocksize              = 8;
    s->lossless_check_data    = 0;

    memset(s->output_shift   , 0, sizeof(s->output_shift   ));
    memset(s->quant_step_size, 0, sizeof(s->quant_step_size));

    for (ch = s->min_channel; ch <= s->max_channel; ch++) {
        ChannelParams *cp = &s->channel_params[ch];
        cp->filter_params[FIR].order = 0;
        cp->filter_params[IIR].order = 0;
        cp->filter_params[FIR].shift = 0;
        cp->filter_params[IIR].shift = 0;

        /* Default audio coding is 24-bit raw PCM. */
        cp->huff_offset      = 0;
        cp->sign_huff_offset = -(1 << 23);
        cp->codebook         = 0;
        cp->huff_lsbs        = 24;
    }

    if (substr == m->max_decoded_substream) {
        m->channels       = s->max_matrix_channel + 1;
        m->channel_layout = s->mask;

        if (!m->is_truehd && m->needs_reordering) {
            if (m->channel_layout == (AV_CH_LAYOUT_QUAD|AV_CH_LOW_FREQUENCY) ||
                m->channel_layout == AV_CH_LAYOUT_5POINT0_BACK) {
                int i = s->ch_assign[4];
                s->ch_assign[4] = s->ch_assign[3];
                s->ch_assign[3] = s->ch_assign[2];
                s->ch_assign[2] = i;
            } else if (m->channel_layout == AV_CH_LAYOUT_5POINT1_BACK) {
                FFSWAP(int, s->ch_assign[2], s->ch_assign[4]);
                FFSWAP(int, s->ch_assign[3], s->ch_assign[5]);
            }
        }

    }

    return 0;
}

/** Read parameters for one of the prediction filters. */

static int read_filter_params(MLPDecodeContext *m, GetBitContext *gbp,
                              unsigned int substr, unsigned int channel,
                              unsigned int filter)
{
    SubStream *s = &m->substream[substr];
    FilterParams *fp = &s->channel_params[channel].filter_params[filter];
    const int max_order = filter ? MAX_IIR_ORDER : MAX_FIR_ORDER;
    const char fchar = filter ? 'I' : 'F';
    int i, order;

    // Filter is 0 for FIR, 1 for IIR.
    av_assert0(filter < 2);

    if (m->filter_changed[channel][filter]++ > 1) {
        av_log(m->avctx, AV_LOG_ERROR, "Filters may change only once per access unit.\n");
        return AVERROR_INVALIDDATA;
    }

    order = get_bits(gbp, 4);
    if (order > max_order) {
        av_log(m->avctx, AV_LOG_ERROR,
               "%cIR filter order %d is greater than maximum %d.\n",
               fchar, order, max_order);
        return AVERROR_INVALIDDATA;
    }
    fp->order = order;

    if (order > 0) {
        int32_t *fcoeff = s->channel_params[channel].coeff[filter];
        int coeff_bits, coeff_shift;

        fp->shift = get_bits(gbp, 4);

        coeff_bits  = get_bits(gbp, 5);
        coeff_shift = get_bits(gbp, 3);
        if (coeff_bits < 1 || coeff_bits > 16) {
            av_log(m->avctx, AV_LOG_ERROR,
                   "%cIR filter coeff_bits must be between 1 and 16.\n",
                   fchar);
            return AVERROR_INVALIDDATA;
        }
        if (coeff_bits + coeff_shift > 16) {
            av_log(m->avctx, AV_LOG_ERROR,
                   "Sum of coeff_bits and coeff_shift for %cIR filter must be 16 or less.\n",
                   fchar);
            return AVERROR_INVALIDDATA;
        }

        for (i = 0; i < order; i++)
            fcoeff[i] = get_sbits(gbp, coeff_bits) * (1 << coeff_shift);

        if (get_bits1(gbp)) {
            int state_bits, state_shift;

            if (filter == FIR) {
                av_log(m->avctx, AV_LOG_ERROR,
                       "FIR filter has state data specified.\n");
                return AVERROR_INVALIDDATA;
            }

            state_bits  = get_bits(gbp, 4);
            state_shift = get_bits(gbp, 4);

            /* TODO: Check validity of state data. */

            for (i = 0; i < order; i++)
                fp->state[i] = state_bits ? get_sbits(gbp, state_bits) * (1 << state_shift) : 0;
        }
    }

    return 0;
}

/** Read parameters for primitive matrices. */

static int read_matrix_params(MLPDecodeContext *m, unsigned int substr, GetBitContext *gbp)
{
    SubStream *s = &m->substream[substr];
    unsigned int mat, ch;
    const int max_primitive_matrices = !m->is_truehd
                                     ? MAX_MATRICES_MLP
                                     : MAX_MATRICES_TRUEHD;

    if (m->matrix_changed++ > 1) {
        av_log(m->avctx, AV_LOG_ERROR, "Matrices may change only once per access unit.\n");
        return AVERROR_INVALIDDATA;
    }

    s->num_primitive_matrices = get_bits(gbp, 4);

    if (s->num_primitive_matrices > max_primitive_matrices) {
        av_log(m->avctx, AV_LOG_ERROR,
               "Number of primitive matrices cannot be greater than %d.\n",
               max_primitive_matrices);
        goto error;
    }

    for (mat = 0; mat < s->num_primitive_matrices; mat++) {
        int frac_bits, max_chan;
        s->matrix_out_ch[mat] = get_bits(gbp, 4);
        frac_bits             = get_bits(gbp, 4);
        s->lsb_bypass   [mat] = get_bits1(gbp);

        if (s->matrix_out_ch[mat] > s->max_matrix_channel) {
            av_log(m->avctx, AV_LOG_ERROR,
                    "Invalid channel %d specified as output from matrix.\n",
                    s->matrix_out_ch[mat]);
            goto error;
        }
        if (frac_bits > 14) {
            av_log(m->avctx, AV_LOG_ERROR,
                    "Too many fractional bits specified.\n");
            goto error;
        }

        max_chan = s->max_matrix_channel;
        if (!s->noise_type)
            max_chan+=2;

        for (ch = 0; ch <= static_cast<unsigned>(max_chan); ch++) {
            int coeff_val = 0;
            if (get_bits1(gbp))
                coeff_val = get_sbits(gbp, frac_bits + 2);

            s->matrix_coeff[mat][ch] = coeff_val * (1 << (14 - frac_bits));
        }

        if (s->noise_type)
            s->matrix_noise_shift[mat] = get_bits(gbp, 4);
        else
            s->matrix_noise_shift[mat] = 0;
    }

    return 0;
error:
    s->num_primitive_matrices = 0;
    memset(s->matrix_out_ch, 0, sizeof(s->matrix_out_ch));

    return AVERROR_INVALIDDATA;
}

/** Read channel parameters. */

static int read_channel_params(MLPDecodeContext *m, unsigned int substr,
                               GetBitContext *gbp, unsigned int ch)
{
    SubStream *s = &m->substream[substr];
    ChannelParams *cp = &s->channel_params[ch];
    FilterParams *fir = &cp->filter_params[FIR];
    FilterParams *iir = &cp->filter_params[IIR];
    int ret;

    if (s->param_presence_flags & PARAM_FIR)
        if (get_bits1(gbp))
            if ((ret = read_filter_params(m, gbp, substr, ch, FIR)) < 0)
                return ret;

    if (s->param_presence_flags & PARAM_IIR)
        if (get_bits1(gbp))
            if ((ret = read_filter_params(m, gbp, substr, ch, IIR)) < 0)
                return ret;

    if (fir->order + iir->order > 8) {
        av_log(m->avctx, AV_LOG_ERROR, "Total filter orders too high.\n");
        return AVERROR_INVALIDDATA;
    }

    if (fir->order && iir->order &&
        fir->shift != iir->shift) {
        av_log(m->avctx, AV_LOG_ERROR,
                "FIR and IIR filters must use the same precision.\n");
        return AVERROR_INVALIDDATA;
    }
    /* The FIR and IIR filters must have the same precision.
     * To simplify the filtering code, only the precision of the
     * FIR filter is considered. If only the IIR filter is employed,
     * the FIR filter precision is set to that of the IIR filter, so
     * that the filtering code can use it. */
    if (!fir->order && iir->order)
        fir->shift = iir->shift;

    if (s->param_presence_flags & PARAM_HUFFOFFSET)
        if (get_bits1(gbp))
            cp->huff_offset = get_sbits(gbp, 15);

    cp->codebook  = get_bits(gbp, 2);
    cp->huff_lsbs = get_bits(gbp, 5);

    if (cp->codebook > 0 && cp->huff_lsbs > 24) {
        av_log(m->avctx, AV_LOG_ERROR, "Invalid huff_lsbs.\n");
        cp->huff_lsbs = 0;
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

/** Read decoding parameters that change more often than those in the restart
 *  header. */

static int read_decoding_params(MLPDecodeContext *m, GetBitContext *gbp,
                                unsigned int substr)
{
    SubStream *s = &m->substream[substr];
    unsigned int ch;
    int ret = 0;
    unsigned recompute_sho = 0;

    if (s->param_presence_flags & PARAM_PRESENCE)
        if (get_bits1(gbp))
            s->param_presence_flags = get_bits(gbp, 8);

    if (s->param_presence_flags & PARAM_BLOCKSIZE)
        if (get_bits1(gbp)) {
            s->blocksize = get_bits(gbp, 9);
            if (s->blocksize < 8 || s->blocksize > m->access_unit_size) {
                av_log(m->avctx, AV_LOG_ERROR, "Invalid blocksize.\n");
                s->blocksize = 0;
                return AVERROR_INVALIDDATA;
            }
        }

    if (s->param_presence_flags & PARAM_MATRIX)
        if (get_bits1(gbp))
            if ((ret = read_matrix_params(m, substr, gbp)) < 0)
                return ret;

    if (s->param_presence_flags & PARAM_OUTSHIFT)
        if (get_bits1(gbp)) {
            for (ch = 0; ch <= s->max_matrix_channel; ch++) {
                s->output_shift[ch] = get_sbits(gbp, 4);
                if (s->output_shift[ch] < 0) {
                    avpriv_request_sample(m->avctx, "Negative output_shift");
                    s->output_shift[ch] = 0;
                }
            }
        }

    if (s->param_presence_flags & PARAM_QUANTSTEP)
        if (get_bits1(gbp))
            for (ch = 0; ch <= s->max_channel; ch++) {
                s->quant_step_size[ch] = get_bits(gbp, 4);

                recompute_sho |= 1<<ch;
            }

    for (ch = s->min_channel; ch <= s->max_channel; ch++)
        if (get_bits1(gbp)) {
            recompute_sho |= 1<<ch;
            if ((ret = read_channel_params(m, substr, gbp, ch)) < 0)
                goto fail;
        }


fail:
    for (ch = 0; ch <= s->max_channel; ch++) {
        if (recompute_sho & (1<<ch)) {
            ChannelParams *cp = &s->channel_params[ch];

            if (cp->codebook > 0 && cp->huff_lsbs < s->quant_step_size[ch]) {
                if (ret >= 0) {
                    av_log(m->avctx, AV_LOG_ERROR, "quant_step_size larger than huff_lsbs\n");
                    ret = AVERROR_INVALIDDATA;
                }
                s->quant_step_size[ch] = 0;
            }

            cp->sign_huff_offset = calculate_sign_huff(m, substr, ch);
        }
    }
    return ret;
}

#define MSB_MASK(bits)  (-1u << (bits))

/** Generate PCM samples using the prediction filters and residual values
 *  read from the data stream, and update the filter state. */

static void filter_channel(MLPDecodeContext *m, unsigned int substr,
                           unsigned int channel)
{
    SubStream *s = &m->substream[substr];
    const int32_t *fircoeff = s->channel_params[channel].coeff[FIR];
    int32_t state_buffer[NUM_FILTERS][MAX_BLOCKSIZE + MAX_FIR_ORDER];
    int32_t *firbuf = state_buffer[FIR] + MAX_BLOCKSIZE;
    int32_t *iirbuf = state_buffer[IIR] + MAX_BLOCKSIZE;
    FilterParams *fir = &s->channel_params[channel].filter_params[FIR];
    FilterParams *iir = &s->channel_params[channel].filter_params[IIR];
    unsigned int filter_shift = fir->shift;
    int32_t mask = MSB_MASK(s->quant_step_size[channel]);

    memcpy(firbuf, fir->state, MAX_FIR_ORDER * sizeof(int32_t));
    memcpy(iirbuf, iir->state, MAX_IIR_ORDER * sizeof(int32_t));

    mlp_filter_channel(firbuf, fircoeff,
                              fir->order, iir->order,
                              filter_shift, mask, s->blocksize,
                              &m->sample_buffer[s->blockpos][channel]);

    memcpy(fir->state, firbuf - s->blocksize, MAX_FIR_ORDER * sizeof(int32_t));
    memcpy(iir->state, iirbuf - s->blocksize, MAX_IIR_ORDER * sizeof(int32_t));
}

/** Read a block of PCM residual data (or actual if no filtering active). */

static int read_block_data(MLPDecodeContext *m, GetBitContext *gbp,
                           unsigned int substr)
{
    SubStream *s = &m->substream[substr];
    unsigned int i, ch, expected_stream_pos = 0;
    int ret;

    if (s->data_check_present) {
        expected_stream_pos  = get_bits_count(gbp);
        expected_stream_pos += get_bits(gbp, 16);
        avpriv_request_sample(m->avctx,
                              "Substreams with VLC block size check info");
    }

    if (s->blockpos + s->blocksize > m->access_unit_size) {
        av_log(m->avctx, AV_LOG_ERROR, "too many audio samples in frame\n");
        return AVERROR_INVALIDDATA;
    }

    memset(&m->bypassed_lsbs[s->blockpos][0], 0,
           s->blocksize * sizeof(m->bypassed_lsbs[0]));

    for (i = 0; i < s->blocksize; i++)
        if ((ret = read_huff_channels(m, gbp, substr, i)) < 0)
            return ret;

    for (ch = s->min_channel; ch <= s->max_channel; ch++)
        filter_channel(m, substr, ch);

    s->blockpos += s->blocksize;

    if (s->data_check_present) {
        if (static_cast<unsigned>(get_bits_count(gbp)) != expected_stream_pos)
            av_log(m->avctx, AV_LOG_ERROR, "block data length mismatch\n");
        skip_bits(gbp, 8);
    }

    return 0;
}

/** Data table used for TrueHD noise generation function. */

static const int8_t noise_table[256] = {
     30,  51,  22,  54,   3,   7,  -4,  38,  14,  55,  46,  81,  22,  58,  -3,   2,
     52,  31,  -7,  51,  15,  44,  74,  30,  85, -17,  10,  33,  18,  80,  28,  62,
     10,  32,  23,  69,  72,  26,  35,  17,  73,  60,   8,  56,   2,   6,  -2,  -5,
     51,   4,  11,  50,  66,  76,  21,  44,  33,  47,   1,  26,  64,  48,  57,  40,
     38,  16, -10, -28,  92,  22, -18,  29, -10,   5, -13,  49,  19,  24,  70,  34,
     61,  48,  30,  14,  -6,  25,  58,  33,  42,  60,  67,  17,  54,  17,  22,  30,
     67,  44,  -9,  50, -11,  43,  40,  32,  59,  82,  13,  49, -14,  55,  60,  36,
     48,  49,  31,  47,  15,  12,   4,  65,   1,  23,  29,  39,  45,  -2,  84,  69,
      0,  72,  37,  57,  27,  41, -15, -16,  35,  31,  14,  61,  24,   0,  27,  24,
     16,  41,  55,  34,  53,   9,  56,  12,  25,  29,  53,   5,  20, -20,  -8,  20,
     13,  28,  -3,  78,  38,  16,  11,  62,  46,  29,  21,  24,  46,  65,  43, -23,
     89,  18,  74,  21,  38, -12,  19,  12, -19,   8,  15,  33,   4,  57,   9,  -8,
     36,  35,  26,  28,   7,  83,  63,  79,  75,  11,   3,  87,  37,  47,  34,  40,
     39,  19,  20,  42,  27,  34,  39,  77,  13,  42,  59,  64,  45,  -1,  32,  37,
     45,  -5,  53,  -6,   7,  36,  50,  23,   6,  32,   9, -21,  18,  71,  27,  52,
    -25,  31,  35,  42,  -1,  68,  63,  52,  26,  43,  66,  37,  41,  25,  40,  70,
};

/** Noise generation functions.
 *  I'm not sure what these are for - they seem to be some kind of pseudorandom
 *  sequence generators, used to generate noise data which is used when the
 *  channels are rematrixed. I'm not sure if they provide a practical benefit
 *  to compression, or just obfuscate the decoder. Are they for some kind of
 *  dithering? */

/** Generate two channels of noise, used in the matrix when
 *  restart sync word == 0x31ea. */

static void generate_2_noise_channels(MLPDecodeContext *m, unsigned int substr)
{
    SubStream *s = &m->substream[substr];
    unsigned int i;
    uint32_t seed = s->noisegen_seed;
    unsigned int maxchan = s->max_matrix_channel;

    for (i = 0; i < s->blockpos; i++) {
        uint16_t seed_shr7 = seed >> 7;
        m->sample_buffer[i][maxchan+1] = ((int8_t)(seed >> 15)) * (1 << s->noise_shift);
        m->sample_buffer[i][maxchan+2] = ((int8_t) seed_shr7)   * (1 << s->noise_shift);

        seed = (seed << 16) ^ seed_shr7 ^ (seed_shr7 << 5);
    }

    s->noisegen_seed = seed;
}

/** Generate a block of noise, used when restart sync word == 0x31eb. */

static void fill_noise_buffer(MLPDecodeContext *m, unsigned int substr)
{
    SubStream *s = &m->substream[substr];
    unsigned int i;
    uint32_t seed = s->noisegen_seed;

    for (i = 0; i < static_cast<unsigned>(m->access_unit_size_pow2); i++) {
        uint8_t seed_shr15 = seed >> 15;
        m->noise_buffer[i] = noise_table[seed_shr15];
        seed = (seed << 8) ^ seed_shr15 ^ (seed_shr15 << 5);
    }

    s->noisegen_seed = seed;
}

/** Write the audio data into the output buffer. */

static int output_data(MLPDecodeContext *m, unsigned int substr,
                       int *got_frame_ptr)
{
    void *avctx = m->avctx;
    SubStream *s = &m->substream[substr];
    unsigned int mat;
    unsigned int maxchan;
    int is32 = (m->is32);

    if (m->channels != s->max_matrix_channel + 1) {
        av_log(m->avctx, AV_LOG_ERROR, "channel count mismatch\n");
        return AVERROR_INVALIDDATA;
    }

    if (!s->blockpos) {
        av_log(avctx, AV_LOG_ERROR, "No samples to output.\n");
        return AVERROR_INVALIDDATA;
    }

    maxchan = s->max_matrix_channel;
    if (!s->noise_type) {
        generate_2_noise_channels(m, substr);
        maxchan += 2;
    } else {
        fill_noise_buffer(m, substr);
    }

    /* Apply the channel matrices in turn to reconstruct the original audio
     * samples. */
    for (mat = 0; mat < s->num_primitive_matrices; mat++) {
        unsigned int dest_ch = s->matrix_out_ch[mat];
        ff_mlp_rematrix_channel(&m->sample_buffer[0][0],
                                    s->matrix_coeff[mat],
                                    reinterpret_cast<const uint8_t *>(&m->bypassed_lsbs[0][mat]),
                                    m->noise_buffer,
                                    s->num_primitive_matrices - mat,
                                    dest_ch,
                                    s->blockpos,
                                    maxchan,
                                    s->matrix_noise_shift[mat],
                                    m->access_unit_size_pow2,
                                    MSB_MASK(s->quant_step_size[dest_ch]));
    }

    /* Pack into the context-owned output buffer. Upstream allocates a
     * refcounted frame here; a fixed buffer sized for the format's maximum
     * access unit covers every stream read_major_sync() accepts, so nothing
     * is allocated per frame. */
    m->out_nb_samples = s->blockpos;
    s->lossless_check_data = ff_mlp_pack_output(s->lossless_check_data,
                                                    s->blockpos,
                                                    m->sample_buffer,
                                                    m->out_buffer,
                                                    s->ch_assign,
                                                    s->output_shift,
                                                    s->max_matrix_channel,
                                                    is32);

    *got_frame_ptr = 1;

    return 0;
}

/** Read an access unit from the stream.
 *  @return negative on error, 0 if not enough data is present in the input stream,
 *  otherwise the number of bytes consumed. */
static int read_access_unit(MLPDecodeContext *m, const uint8_t *buf,
                            int buf_size, int *got_frame_ptr)
{
    /* Discarded by the av_log() shim; kept so upstream's log call sites in
     * this function stay unchanged. */
    void *avctx = m->avctx;
    GetBitContext gb;
    unsigned int length, substr;
    unsigned int substream_start;
    unsigned int header_size = 4;
    unsigned int substr_header_size = 0;
    uint8_t substream_parity_present[MAX_SUBSTREAMS];
    uint16_t substream_data_len[MAX_SUBSTREAMS];
    uint8_t parity_bits;
    int ret;

    if (buf_size < 4)
        return AVERROR_INVALIDDATA;

    length = (AV_RB16(buf) & 0xfff) * 2;

    if (length < 4 || length > static_cast<unsigned>(buf_size))
        return AVERROR_INVALIDDATA;

    init_get_bits(&gb, (buf + 4), (length - 4) * 8);

    m->is_major_sync_unit = 0;
    if (show_bits_long(&gb, 31) == (0xf8726fba >> 1)) {
        if (read_major_sync(m, &gb) < 0)
            goto error;
        m->is_major_sync_unit = 1;
        header_size += m->major_sync_header_size;
    }

    if (!m->params_valid) {
        av_log(m->avctx, AV_LOG_WARNING,
               "Stream parameters not seen; skipping frame.\n");
        *got_frame_ptr = 0;
        return length;
    }

    substream_start = 0;

    for (substr = 0; substr < m->num_substreams; substr++) {
        int extraword_present, checkdata_present, end, nonrestart_substr;

        extraword_present = get_bits1(&gb);
        nonrestart_substr = get_bits1(&gb);
        checkdata_present = get_bits1(&gb);
        skip_bits1(&gb);

        end = get_bits(&gb, 12) * 2;

        substr_header_size += 2;

        if (extraword_present) {
            if (!m->is_truehd) {
                av_log(m->avctx, AV_LOG_ERROR, "There must be no extraword for MLP.\n");
                goto error;
            }
            skip_bits(&gb, 16);
            substr_header_size += 2;
        }

        if (length < header_size + substr_header_size) {
            av_log(m->avctx, AV_LOG_ERROR, "Insufficient data for headers\n");
            goto error;
        }

        if (!(nonrestart_substr ^ m->is_major_sync_unit)) {
            av_log(m->avctx, AV_LOG_ERROR, "Invalid nonrestart_substr.\n");
            goto error;
        }

        if (end + header_size + substr_header_size > length) {
            av_log(m->avctx, AV_LOG_ERROR,
                   "Indicated length of substream %d data goes off end of "
                   "packet.\n", substr);
            end = length - header_size - substr_header_size;
        }

        if (static_cast<unsigned>(end) < substream_start) {
            av_log(avctx, AV_LOG_ERROR,
                   "Indicated end offset of substream %d data "
                   "is smaller than calculated start offset.\n",
                   substr);
            goto error;
        }

        if (substr > m->max_decoded_substream)
            continue;

        substream_parity_present[substr] = checkdata_present;
        substream_data_len[substr] = end - substream_start;
        substream_start = end;
    }

    parity_bits  = ff_mlp_calculate_parity(buf, 4);
    parity_bits ^= ff_mlp_calculate_parity(buf + header_size, substr_header_size);

    if ((((parity_bits >> 4) ^ parity_bits) & 0xF) != 0xF) {
        av_log(avctx, AV_LOG_ERROR, "Parity check failed.\n");
        goto error;
    }

    buf += header_size + substr_header_size;

    for (substr = 0; substr <= m->max_decoded_substream; substr++) {
        SubStream *s = &m->substream[substr];
        init_get_bits(&gb, buf, substream_data_len[substr] * 8);

        m->matrix_changed = 0;
        memset(m->filter_changed, 0, sizeof(m->filter_changed));

        s->blockpos = 0;
        do {
            if (get_bits1(&gb)) {
                if (get_bits1(&gb)) {
                    /* A restart header should be present. */
                    if (read_restart_header(m, &gb, buf, substr) < 0)
                        goto next_substr;
                    s->restart_seen = 1;
                }

                if (!s->restart_seen)
                    goto next_substr;
                if (read_decoding_params(m, &gb, substr) < 0)
                    goto next_substr;
            }

            if (!s->restart_seen)
                goto next_substr;

            if ((ret = read_block_data(m, &gb, substr)) < 0)
                return ret;

            if (get_bits_count(&gb) >= substream_data_len[substr] * 8)
                goto substream_length_mismatch;

        } while (!get_bits1(&gb));

        skip_bits(&gb, (-get_bits_count(&gb)) & 15);

        if (substream_data_len[substr] * 8 - get_bits_count(&gb) >= 32) {
            int shorten_by;

            if (get_bits(&gb, 16) != 0xD234)
                return AVERROR_INVALIDDATA;

            shorten_by = get_bits(&gb, 16);
            if      (m->is_truehd && shorten_by  & 0x2000)
                s->blockpos -= FFMIN(shorten_by & 0x1FFF, s->blockpos);
            else if (!m->is_truehd    && shorten_by != 0xD234)
                return AVERROR_INVALIDDATA;

            if (substr == m->max_decoded_substream)
                av_log(m->avctx, AV_LOG_INFO, "End of stream indicated.\n");
        }

        if (substream_parity_present[substr]) {
            uint8_t parity, checksum;

            if (substream_data_len[substr] * 8 - get_bits_count(&gb) != 16)
                goto substream_length_mismatch;

            parity   = ff_mlp_calculate_parity(buf, substream_data_len[substr] - 2);
            checksum = ff_mlp_checksum8       (buf, substream_data_len[substr] - 2);

            if ((get_bits(&gb, 8) ^ parity) != 0xa9    )
                av_log(m->avctx, AV_LOG_ERROR, "Substream %d parity check failed.\n", substr);
            if ( get_bits(&gb, 8)           != checksum)
                av_log(m->avctx, AV_LOG_ERROR, "Substream %d checksum failed.\n"    , substr);
        }

        if (substream_data_len[substr] * 8 != get_bits_count(&gb))
            goto substream_length_mismatch;

next_substr:
        if (!s->restart_seen)
            av_log(m->avctx, AV_LOG_ERROR,
                   "No restart header present in substream %d.\n", substr);

        buf += substream_data_len[substr];
    }

    if ((ret = output_data(m, m->max_decoded_substream, got_frame_ptr)) < 0)
        return ret;

    return length;

substream_length_mismatch:
    av_log(m->avctx, AV_LOG_ERROR, "substream %d length mismatch\n", substr);
    return AVERROR_INVALIDDATA;

error:
    m->params_valid = 0;
    return AVERROR_INVALIDDATA;
}

} // namespace

// ---------------------------------------------------------------------------
// Public interface (see mlpdec.h)
// ---------------------------------------------------------------------------

struct MlpDecoderCore
{
  MLPDecodeContext ctx{};
};

void MlpCoreDeleter::operator()(MlpDecoderCore *core) const { delete core; }

unique_mlp_core mlp_core_create(int is_truehd, uint64_t request_channel_layout)
{
  std::call_once(init_static_once, init_static);

  auto core = unique_mlp_core(new (std::nothrow) MlpDecoderCore());
  if (!core) return nullptr;

  MLPDecodeContext *m = &core->ctx;
  for (int substr = 0; substr < MAX_SUBSTREAMS; substr++)
  {
    m->substream[substr].lossless_check_data = 0xffffffff;
  }
  m->is_truehd = is_truehd;
  m->request_channel_layout = request_channel_layout;

  return core;
}

int mlp_core_decode(MlpDecoderCore *core, const uint8_t *buf, int size,
                    const void **out, int *nb_samples)
{
  MLPDecodeContext *m = &core->ctx;
  int got_frame = 0;

  *out = nullptr;
  *nb_samples = 0;

  const int ret = read_access_unit(m, buf, size, &got_frame);
  if (ret < 0) return ret;

  if (got_frame)
  {
    *out = m->out_buffer;
    *nb_samples = m->out_nb_samples;
  }
  return ret;
}

void mlp_core_flush(MlpDecoderCore *core)
{
  MLPDecodeContext *m = &core->ctx;

  // A major sync must be seen again before decoding resumes, and every
  // substream must re-read a restart header, which resets all filter and
  // matrix state. That is exactly the state a seek needs to drop.
  m->params_valid = 0;
  for (int substr = 0; substr < MAX_SUBSTREAMS; substr++)
  {
    m->substream[substr].restart_seen = 0;
    m->substream[substr].lossless_check_data = 0xffffffff;
  }
}

int mlp_core_channels(const MlpDecoderCore *core) { return core->ctx.channels; }
int mlp_core_sample_rate(const MlpDecoderCore *core) { return core->ctx.sample_rate; }
int mlp_core_bits_per_sample(const MlpDecoderCore *core)
{
  return core->ctx.bits_per_raw_sample;
}
int mlp_core_is32(const MlpDecoderCore *core) { return core->ctx.is32; }
int mlp_core_params_valid(const MlpDecoderCore *core) { return core->ctx.params_valid; }
int mlp_core_access_unit_size(const MlpDecoderCore *core)
{
  return core->ctx.access_unit_size;
}

int mlp_probe_is_truehd(const uint8_t *buf, int size)
{
  // A major sync sits 4 bytes into the access unit, after the length/check
  // prefix. 0xf8726fbb marks MLP, 0xf8726fba TrueHD.
  if (size < 8) return -1;

  const uint32_t sync = (static_cast<uint32_t>(buf[4]) << 24) |
                        (static_cast<uint32_t>(buf[5]) << 16) |
                        (static_cast<uint32_t>(buf[6]) << 8) |
                        static_cast<uint32_t>(buf[7]);
  if (sync == 0xf8726fba) return 1;
  if (sync == 0xf8726fbb) return 0;
  return -1;
}
