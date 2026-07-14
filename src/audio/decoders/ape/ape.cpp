// src/audio/decoders/ape/ape.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Monkey's Audio (APE) decoder, ported from FFmpeg's libavcodec/apedec.c
// and libavformat/ape.c as a lighter alternative to FFmpeg for this format.
//
// Based on FFmpeg's APE demuxer and lossless audio decoder:
// Copyright (c) 2007 Benjamin Zores <ben@geexbox.org>
// based upon libdemac from Dave Chapman.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/common.h"
#include "core/log.h"
#include "audio/decoder.h"
#include "io/io.h"
#include "audio/audio.h"

namespace {

// ---------------------------------------------------------------------
// Minimal MSB-first bit reader, replacing FFmpeg's GetBitContext.
// Only used for the fileversion < 3900 code paths. Behaviorally
// equivalent to get_bits.h's cached reader (which is purely a perf
// optimization) - reads the same bits in the same order.
// ---------------------------------------------------------------------
struct BitReader
{
  const uint8_t *buf = nullptr;
  int64_t size_bits = 0;
  int64_t pos_bits = 0;

  void init(const uint8_t *b, int64_t size_bytes)
  {
    buf = b;
    size_bits = size_bytes * 8;
    pos_bits = 0;
  }

  int get_bits1()
  {
    if (pos_bits >= size_bits)
    {
      pos_bits++;
      return 0;
    }
    int byte = buf[pos_bits >> 3];
    int bit = 7 - static_cast<int>(pos_bits & 7);
    pos_bits++;
    return (byte >> bit) & 1;
  }

  unsigned int get_bits(int n)
  {
    unsigned int v = 0;
    for (int i = 0; i < n; i++)
    {
      v = (v << 1) | static_cast<unsigned int>(get_bits1());
    }
    return v;
  }

  unsigned int get_bits_long(int n) { return get_bits(n); }

  int64_t get_bits_left() const { return size_bits - pos_bits; }

  void skip_bits_long(int64_t n) { pos_bits += n; }
};

/* Counts bits != stop until a bit == stop is read, or len is reached -
 * same semantics as FFmpeg's unary.h get_unary(). */
static int get_unary(BitReader &gb, int stop, int64_t len)
{
  int i;
  for (i = 0; static_cast<int64_t>(i) < len && gb.get_bits1() != stop; i++)
  {
  }
  return i;
}

constexpr int MIN_CACHE_BITS = 25;

// ---------------------------------------------------------------------
// Scalar reference implementation of FFmpeg's
// scalarproduct_and_madd_int16 (lossless_audiodsp.c's _c fallback).
// Computes the dot product of v1 and v2 using v1's *original* values,
// then updates v1[i] += mul * v3[i] in place. order must be even (as
// in the original - APE filter orders are always even).
// ---------------------------------------------------------------------
static int32_t scalarproduct_and_madd_int16(int16_t *v1, const int16_t *v2,
                                            const int16_t *v3, int order,
                                            int mul)
{
  unsigned res = 0;

  do
  {
    res += *v1 * *v2++;
    *v1++ += static_cast<int16_t>(mul * *v3++);
    res += *v1 * *v2++;
    *v1++ += static_cast<int16_t>(mul * *v3++);
  } while (order -= 2);

  return static_cast<int32_t>(res);
}

static inline int32_t clip_int16(int32_t v)
{
  if (v < -32768) return -32768;
  if (v > 32767) return 32767;
  return v;
}

/** Get inverse sign of integer (-1 for positive, 1 for negative and 0 for zero) */
static inline int APESIGN(int32_t x) { return (x < 0) - (x > 0); }

static inline unsigned FFABSU(int32_t a) { return static_cast<unsigned>(a < 0 ? -static_cast<unsigned>(a) : static_cast<unsigned>(a)); }

// ---------------------------------------------------------------------
// Constants verbatim from apedec.c
// ---------------------------------------------------------------------

constexpr int MAX_CHANNELS = 2;

constexpr int APE_FRAMECODE_MONO_SILENCE = 1;
constexpr int APE_FRAMECODE_STEREO_SILENCE = 3;
constexpr int APE_FRAMECODE_PSEUDO_STEREO = 4;

constexpr int HISTORY_SIZE = 512;
constexpr int PREDICTOR_ORDER = 8;
constexpr int PREDICTOR_SIZE = 50;

constexpr int YDELAYA = 18 + PREDICTOR_ORDER * 4;
constexpr int YDELAYB = 18 + PREDICTOR_ORDER * 3;
constexpr int XDELAYA = 18 + PREDICTOR_ORDER * 2;
constexpr int XDELAYB = 18 + PREDICTOR_ORDER;

constexpr int YADAPTCOEFFSA = 18;
constexpr int XADAPTCOEFFSA = 14;
constexpr int YADAPTCOEFFSB = 10;
constexpr int XADAPTCOEFFSB = 5;

enum APECompressionLevel
{
  COMPRESSION_LEVEL_FAST = 1000,
  COMPRESSION_LEVEL_NORMAL = 2000,
  COMPRESSION_LEVEL_HIGH = 3000,
  COMPRESSION_LEVEL_EXTRA_HIGH = 4000,
  COMPRESSION_LEVEL_INSANE = 5000
};

constexpr int APE_FILTER_LEVELS = 3;

static const uint16_t ape_filter_orders[5][APE_FILTER_LEVELS] = {
    {0, 0, 0}, {16, 0, 0}, {64, 0, 0}, {32, 256, 0}, {16, 256, 1280}};

static const uint8_t ape_filter_fracbits[5][APE_FILTER_LEVELS] = {
    {0, 0, 0}, {11, 0, 0}, {11, 0, 0}, {10, 13, 0}, {11, 13, 15}};

#define APE_MIN_VERSION 3800
#define APE_MAX_VERSION 3990

#define MAC_FORMAT_FLAG_8_BIT 1
#define MAC_FORMAT_FLAG_CRC 2
#define MAC_FORMAT_FLAG_HAS_PEAK_LEVEL 4
#define MAC_FORMAT_FLAG_24_BIT 8
#define MAC_FORMAT_FLAG_HAS_SEEK_ELEMENTS 16
#define MAC_FORMAT_FLAG_CREATE_WAV_HEADER 32

// ---------------------------------------------------------------------
// Structures, verbatim in shape from apedec.c
// ---------------------------------------------------------------------

struct APEFilter
{
  int16_t *coeffs = nullptr;
  int16_t *adaptcoeffs = nullptr;
  int16_t *historybuffer = nullptr;
  int16_t *delay = nullptr;
  uint32_t avg = 0;
};

struct APERice
{
  uint32_t k = 0;
  uint32_t ksum = 0;
};

struct APERangecoder
{
  uint32_t low = 0;
  uint32_t range = 0;
  uint32_t help = 0;
  unsigned int buffer = 0;
};

struct APEPredictor
{
  int32_t *buf = nullptr;
  int32_t lastA[2] = {0, 0};
  int32_t filterA[2] = {0, 0};
  int32_t filterB[2] = {0, 0};
  uint32_t coeffsA[2][4] = {{0}};
  uint32_t coeffsB[2][5] = {{0}};
  int32_t historybuffer[HISTORY_SIZE + PREDICTOR_SIZE] = {0};
  unsigned int sample_pos = 0;
};

struct APEPredictor64
{
  int64_t *buf = nullptr;
  int64_t lastA[2] = {0, 0};
  int64_t filterA[2] = {0, 0};
  int64_t filterB[2] = {0, 0};
  uint64_t coeffsA[2][4] = {{0}};
  uint64_t coeffsB[2][5] = {{0}};
  int64_t historybuffer[HISTORY_SIZE + PREDICTOR_SIZE] = {0};
};

// Forward declarations for the version-dispatch function pointers.
struct ApeCtx;
static void entropy_decode_mono_0000(ApeCtx *ctx, int blockstodecode);
static void entropy_decode_stereo_0000(ApeCtx *ctx, int blockstodecode);
static void entropy_decode_mono_3860(ApeCtx *ctx, int blockstodecode);
static void entropy_decode_stereo_3860(ApeCtx *ctx, int blockstodecode);
static void entropy_decode_mono_3900(ApeCtx *ctx, int blockstodecode);
static void entropy_decode_stereo_3900(ApeCtx *ctx, int blockstodecode);
static void entropy_decode_stereo_3930(ApeCtx *ctx, int blockstodecode);
static void entropy_decode_mono_3990(ApeCtx *ctx, int blockstodecode);
static void entropy_decode_stereo_3990(ApeCtx *ctx, int blockstodecode);
static void predictor_decode_mono_3800(ApeCtx *ctx, int count);
static void predictor_decode_stereo_3800(ApeCtx *ctx, int count);
static void predictor_decode_mono_3930(ApeCtx *ctx, int count);
static void predictor_decode_stereo_3930(ApeCtx *ctx, int count);
static void predictor_decode_mono_3950(ApeCtx *ctx, int count);
static void predictor_decode_stereo_3950(ApeCtx *ctx, int count);
static void ape_apply_filters(ApeCtx *ctx, int32_t *decoded0, int32_t *decoded1,
                              int count);

/** Decoder context - equivalent to APEContext in apedec.c */
struct ApeCtx
{
  int channels = 0;
  int samples = 0; // blocks left to decode in current frame
  int bps = 0;

  int fileversion = 0;
  int compression_level = 0;
  int fset = 0;
  int flags = 0;

  uint32_t CRC = 0;
  uint32_t CRC_state = 0;
  int frameflags = 0;
  APEPredictor predictor;
  APEPredictor64 predictor64;

  std::vector<int32_t> decoded_buffer;
  int32_t *decoded[MAX_CHANNELS] = {nullptr, nullptr};
  std::vector<int32_t> interim_buffer;
  int32_t *interim[MAX_CHANNELS] = {nullptr, nullptr};
  int blocks_per_loop = 4608;

  std::vector<int16_t> filterbuf[APE_FILTER_LEVELS];

  APERangecoder rc;
  APERice riceX, riceY;
  APEFilter filters[APE_FILTER_LEVELS][2];
  BitReader gb;

  std::vector<uint8_t> data; // current frame data (bswapped copy)
  const uint8_t *data_end = nullptr;
  const uint8_t *ptr = nullptr;

  int error = 0;
  int interim_mode = 0;

  void (*entropy_decode_mono)(ApeCtx *, int) = nullptr;
  void (*entropy_decode_stereo)(ApeCtx *, int) = nullptr;
  void (*predictor_decode_mono)(ApeCtx *, int) = nullptr;
  void (*predictor_decode_stereo)(ApeCtx *, int) = nullptr;
};

static uint8_t bytestream_get_byte(const uint8_t **b) { return *(*b)++; }

static uint32_t bytestream_get_be32(const uint8_t **b)
{
  uint32_t v = (static_cast<uint32_t>((*b)[0]) << 24) |
              (static_cast<uint32_t>((*b)[1]) << 16) |
              (static_cast<uint32_t>((*b)[2]) << 8) |
              static_cast<uint32_t>((*b)[3]);
  *b += 4;
  return v;
}

static int av_log2(unsigned v)
{
  int n = 0;
  if (v == 0) return 0;
  while (v >>= 1) n++;
  return n;
}

// ---------------------------------------------------------------------
// Range decoder - verbatim from apedec.c
// ---------------------------------------------------------------------

constexpr int CODE_BITS = 32;
constexpr unsigned int TOP_VALUE = static_cast<unsigned int>(1) << (CODE_BITS - 1);
constexpr int EXTRA_BITS = (CODE_BITS - 2) % 8 + 1;
constexpr unsigned int BOTTOM_VALUE = TOP_VALUE >> 8;

static inline void range_start_decoding(ApeCtx *ctx)
{
  ctx->rc.buffer = bytestream_get_byte(&ctx->ptr);
  ctx->rc.low = ctx->rc.buffer >> (8 - EXTRA_BITS);
  ctx->rc.range = static_cast<uint32_t>(1) << EXTRA_BITS;
}

static inline void range_dec_normalize(ApeCtx *ctx)
{
  while (ctx->rc.range <= BOTTOM_VALUE)
  {
    ctx->rc.buffer <<= 8;
    if (ctx->ptr < ctx->data_end)
    {
      ctx->rc.buffer += *ctx->ptr;
      ctx->ptr++;
    }
    else
    {
      ctx->error = 1;
    }
    ctx->rc.low = (ctx->rc.low << 8) | ((ctx->rc.buffer >> 1) & 0xFF);
    ctx->rc.range <<= 8;
  }
}

static inline int range_decode_culfreq(ApeCtx *ctx, int tot_f)
{
  range_dec_normalize(ctx);
  ctx->rc.help = ctx->rc.range / tot_f;
  return ctx->rc.low / ctx->rc.help;
}

static inline int range_decode_culshift(ApeCtx *ctx, int shift)
{
  range_dec_normalize(ctx);
  ctx->rc.help = ctx->rc.range >> shift;
  return ctx->rc.low / ctx->rc.help;
}

static inline void range_decode_update(ApeCtx *ctx, int sy_f, int lt_f)
{
  ctx->rc.low -= ctx->rc.help * lt_f;
  ctx->rc.range = ctx->rc.help * sy_f;
}

static inline int range_decode_bits(ApeCtx *ctx, int n)
{
  int sym = range_decode_culshift(ctx, n);
  range_decode_update(ctx, 1, sym);
  return sym;
}

constexpr int MODEL_ELEMENTS = 64;

static const uint16_t counts_3970[22] = {
    0,     14824, 28224, 39348, 47855, 53994, 58171, 60926, 62682, 63786,
    64463, 64878, 65126, 65276, 65365, 65419, 65450, 65469, 65480, 65487,
    65491, 65493};

static const uint16_t counts_diff_3970[21] = {
    14824, 13400, 11124, 8507, 6139, 4177, 2755, 1756, 1104, 677,
    415,   248,   150,   89,   54,   31,   19,   11,   7,    4,
    2};

static const uint16_t counts_3980[22] = {
    0,     19578, 36160, 48417, 56323, 60899, 63265, 64435, 64971, 65232,
    65351, 65416, 65447, 65466, 65476, 65482, 65485, 65488, 65490, 65491,
    65492, 65493};

static const uint16_t counts_diff_3980[21] = {
    19578, 16582, 12257, 7906, 4576, 2366, 1170, 536, 261, 119,
    65,    31,    19,    10,   6,    3,    3,    2,   1,   1,
    1};

static inline int range_get_symbol(ApeCtx *ctx, const uint16_t counts[],
                                   const uint16_t counts_diff[])
{
  int symbol, cf;

  cf = range_decode_culshift(ctx, 16);

  if (cf > 65492)
  {
    symbol = cf - 65535 + 63;
    range_decode_update(ctx, 1, cf);
    if (cf > 65535) ctx->error = 1;
    return symbol;
  }
  for (symbol = 0; counts[symbol + 1] <= static_cast<unsigned>(cf); symbol++)
  {
  }

  range_decode_update(ctx, counts_diff[symbol], counts[symbol]);

  return symbol;
}

static inline void update_rice(APERice *rice, unsigned int x)
{
  int lim = rice->k ? (1 << (rice->k + 4)) : 0;
  rice->ksum += ((x + 1) / 2) - ((rice->ksum + 16) >> 5);

  if (rice->ksum < static_cast<unsigned>(lim))
    rice->k--;
  else if (rice->ksum >= (1u << (rice->k + 5)) && rice->k < 24)
    rice->k++;
}

static inline int get_rice_ook(BitReader &gb, int k)
{
  unsigned int x;

  x = get_unary(gb, 1, gb.get_bits_left());

  if (k) x = (x << k) | gb.get_bits(k);

  return static_cast<int>(x);
}

static inline int ape_decode_value_3860(ApeCtx *ctx, BitReader &gb,
                                        APERice *rice)
{
  unsigned int x, overflow;

  overflow = get_unary(gb, 1, gb.get_bits_left());

  if (ctx->fileversion > 3880)
  {
    while (overflow >= 16)
    {
      overflow -= 16;
      rice->k += 4;
    }
  }

  if (!rice->k)
    x = overflow;
  else if (rice->k <= MIN_CACHE_BITS)
  {
    x = (overflow << rice->k) + gb.get_bits(rice->k);
  }
  else
  {
    ctx->error = 1;
    return 0;
  }
  rice->ksum += x - ((rice->ksum + 8) >> 4);
  if (rice->ksum < (rice->k ? (1u << (rice->k + 4)) : 0))
    rice->k--;
  else if (rice->ksum >= (1u << (rice->k + 5)) && rice->k < 24)
    rice->k++;

  return static_cast<int>(((x >> 1) ^ ((x & 1) - 1)) + 1);
}

static inline int ape_decode_value_3900(ApeCtx *ctx, APERice *rice)
{
  unsigned int x, overflow;
  int tmpk;

  overflow = range_get_symbol(ctx, counts_3970, counts_diff_3970);

  if (overflow == (MODEL_ELEMENTS - 1))
  {
    tmpk = range_decode_bits(ctx, 5);
    overflow = 0;
  }
  else
    tmpk = (rice->k < 1) ? 0 : rice->k - 1;

  if (tmpk <= 16 || ctx->fileversion < 3910)
  {
    if (tmpk > 23)
    {
      ctx->error = 1;
      return 0;
    }
    x = range_decode_bits(ctx, tmpk);
  }
  else if (tmpk <= 31)
  {
    x = range_decode_bits(ctx, 16);
    x |= (static_cast<unsigned>(range_decode_bits(ctx, tmpk - 16)) << 16);
  }
  else
  {
    ctx->error = 1;
    return 0;
  }
  x += overflow << tmpk;

  update_rice(rice, x);

  return static_cast<int>(((x >> 1) ^ ((x & 1) - 1)) + 1);
}

static inline int ape_decode_value_3990(ApeCtx *ctx, APERice *rice)
{
  unsigned int x, overflow, pivot;
  int base;

  pivot = std::max<unsigned>(rice->ksum >> 5, 1);

  overflow = range_get_symbol(ctx, counts_3980, counts_diff_3980);

  if (overflow == (MODEL_ELEMENTS - 1))
  {
    overflow = static_cast<unsigned>(range_decode_bits(ctx, 16)) << 16;
    overflow |= range_decode_bits(ctx, 16);
  }

  if (pivot < 0x10000)
  {
    base = range_decode_culfreq(ctx, static_cast<int>(pivot));
    range_decode_update(ctx, 1, base);
  }
  else
  {
    int base_hi = static_cast<int>(pivot), base_lo;
    int bbits = 0;

    while (base_hi & ~0xFFFF)
    {
      base_hi >>= 1;
      bbits++;
    }
    base_hi = range_decode_culfreq(ctx, base_hi + 1);
    range_decode_update(ctx, 1, base_hi);
    base_lo = range_decode_culfreq(ctx, 1 << bbits);
    range_decode_update(ctx, 1, base_lo);

    base = (base_hi << bbits) + base_lo;
  }

  x = static_cast<unsigned>(base) + overflow * pivot;

  update_rice(rice, x);

  return static_cast<int>(((x >> 1) ^ ((x & 1) - 1)) + 1);
}

static int get_k(int ksum) { return av_log2(static_cast<unsigned>(ksum)) + !!ksum; }

static void decode_array_0000(ApeCtx *ctx, BitReader &gb, int32_t *out,
                              APERice *rice, int blockstodecode)
{
  int i;
  unsigned ksummax, ksummin;

  rice->ksum = 0;
  for (i = 0; i < std::min(blockstodecode, 5); i++)
  {
    out[i] = get_rice_ook(gb, 10);
    rice->ksum += out[i];
  }

  if (blockstodecode <= 5) goto end;

  rice->k = static_cast<uint32_t>(get_k(static_cast<int>(rice->ksum / 10)));
  if (rice->k >= 24) return;
  for (; i < std::min(blockstodecode, 64); i++)
  {
    out[i] = get_rice_ook(gb, static_cast<int>(rice->k));
    rice->ksum += out[i];
    rice->k = static_cast<uint32_t>(
        get_k(static_cast<int>(rice->ksum / ((i + 1) * 2))));
    if (rice->k >= 24) return;
  }

  if (blockstodecode <= 64) goto end;

  rice->k = static_cast<uint32_t>(get_k(static_cast<int>(rice->ksum >> 7)));
  ksummax = 1u << (rice->k + 7);
  ksummin = rice->k ? (1u << (rice->k + 6)) : 0;
  for (; i < blockstodecode; i++)
  {
    if (gb.get_bits_left() < 1)
    {
      ctx->error = 1;
      return;
    }
    out[i] = get_rice_ook(gb, static_cast<int>(rice->k));
    rice->ksum += out[i] - static_cast<unsigned>(out[i - 64]);
    while (rice->ksum < ksummin)
    {
      rice->k--;
      ksummin = rice->k ? ksummin >> 1 : 0;
      ksummax >>= 1;
    }
    while (rice->ksum >= ksummax)
    {
      rice->k++;
      if (rice->k > 24) return;
      ksummax <<= 1;
      ksummin = ksummin ? ksummin << 1 : 128;
    }
  }

end:
  for (i = 0; i < blockstodecode; i++)
    out[i] = ((out[i] >> 1) ^ ((out[i] & 1) - 1)) + 1;
}

static void entropy_decode_mono_0000(ApeCtx *ctx, int blockstodecode)
{
  decode_array_0000(ctx, ctx->gb, ctx->decoded[0], &ctx->riceY, blockstodecode);
}

static void entropy_decode_stereo_0000(ApeCtx *ctx, int blockstodecode)
{
  decode_array_0000(ctx, ctx->gb, ctx->decoded[0], &ctx->riceY, blockstodecode);
  decode_array_0000(ctx, ctx->gb, ctx->decoded[1], &ctx->riceX, blockstodecode);
}

static void entropy_decode_mono_3860(ApeCtx *ctx, int blockstodecode)
{
  int32_t *decoded0 = ctx->decoded[0];
  while (blockstodecode--)
    *decoded0++ = ape_decode_value_3860(ctx, ctx->gb, &ctx->riceY);
}

static void entropy_decode_stereo_3860(ApeCtx *ctx, int blockstodecode)
{
  int32_t *decoded0 = ctx->decoded[0];
  int32_t *decoded1 = ctx->decoded[1];
  int blocks = blockstodecode;

  while (blockstodecode--)
    *decoded0++ = ape_decode_value_3860(ctx, ctx->gb, &ctx->riceY);
  while (blocks--) *decoded1++ = ape_decode_value_3860(ctx, ctx->gb, &ctx->riceX);
}

static void entropy_decode_mono_3900(ApeCtx *ctx, int blockstodecode)
{
  int32_t *decoded0 = ctx->decoded[0];
  while (blockstodecode--) *decoded0++ = ape_decode_value_3900(ctx, &ctx->riceY);
}

static void entropy_decode_stereo_3900(ApeCtx *ctx, int blockstodecode)
{
  int32_t *decoded0 = ctx->decoded[0];
  int32_t *decoded1 = ctx->decoded[1];
  int blocks = blockstodecode;

  while (blockstodecode--) *decoded0++ = ape_decode_value_3900(ctx, &ctx->riceY);
  range_dec_normalize(ctx);
  ctx->ptr -= 1;
  range_start_decoding(ctx);
  while (blocks--) *decoded1++ = ape_decode_value_3900(ctx, &ctx->riceX);
}

static void entropy_decode_stereo_3930(ApeCtx *ctx, int blockstodecode)
{
  int32_t *decoded0 = ctx->decoded[0];
  int32_t *decoded1 = ctx->decoded[1];

  while (blockstodecode--)
  {
    *decoded0++ = ape_decode_value_3900(ctx, &ctx->riceY);
    *decoded1++ = ape_decode_value_3900(ctx, &ctx->riceX);
  }
}

static void entropy_decode_mono_3990(ApeCtx *ctx, int blockstodecode)
{
  int32_t *decoded0 = ctx->decoded[0];
  while (blockstodecode--) *decoded0++ = ape_decode_value_3990(ctx, &ctx->riceY);
}

static void entropy_decode_stereo_3990(ApeCtx *ctx, int blockstodecode)
{
  int32_t *decoded0 = ctx->decoded[0];
  int32_t *decoded1 = ctx->decoded[1];

  while (blockstodecode--)
  {
    *decoded0++ = ape_decode_value_3990(ctx, &ctx->riceY);
    *decoded1++ = ape_decode_value_3990(ctx, &ctx->riceX);
  }
}

static int init_entropy_decoder(ApeCtx *ctx)
{
  if (ctx->fileversion >= 3900)
  {
    if (ctx->data_end - ctx->ptr < 6) return -1;
    ctx->CRC = bytestream_get_be32(&ctx->ptr);
  }
  else
  {
    ctx->CRC = ctx->gb.get_bits_long(32);
  }

  ctx->frameflags = 0;
  ctx->CRC_state = UINT32_MAX;
  if ((ctx->fileversion > 3820) && (ctx->CRC & 0x80000000))
  {
    ctx->CRC &= ~0x80000000;

    if (ctx->data_end - ctx->ptr < 6) return -1;
    ctx->frameflags = static_cast<int>(bytestream_get_be32(&ctx->ptr));
  }

  ctx->riceX.k = 10;
  ctx->riceX.ksum = (1 << ctx->riceX.k) * 16;
  ctx->riceY.k = 10;
  ctx->riceY.ksum = (1 << ctx->riceY.k) * 16;

  if (ctx->fileversion >= 3900)
  {
    ctx->ptr++;
    range_start_decoding(ctx);
  }

  return 0;
}

static const int32_t initial_coeffs_fast_3320[1] = {375};
static const int32_t initial_coeffs_a_3800[3] = {64, 115, 64};
static const int32_t initial_coeffs_b_3800[2] = {740, 0};
static const int32_t initial_coeffs_3930[4] = {360, 317, -109, 98};
static const int64_t initial_coeffs_3930_64bit[4] = {360, 317, -109, 98};

static void init_predictor_decoder(ApeCtx *ctx)
{
  APEPredictor *p = &ctx->predictor;
  APEPredictor64 *p64 = &ctx->predictor64;

  memset(p->historybuffer, 0, PREDICTOR_SIZE * sizeof(*p->historybuffer));
  memset(p64->historybuffer, 0, PREDICTOR_SIZE * sizeof(*p64->historybuffer));
  p->buf = p->historybuffer;
  p64->buf = p64->historybuffer;

  if (ctx->fileversion < 3930)
  {
    if (ctx->compression_level == COMPRESSION_LEVEL_FAST)
    {
      memcpy(p->coeffsA[0], initial_coeffs_fast_3320, sizeof(initial_coeffs_fast_3320));
      memcpy(p->coeffsA[1], initial_coeffs_fast_3320, sizeof(initial_coeffs_fast_3320));
    }
    else
    {
      memcpy(p->coeffsA[0], initial_coeffs_a_3800, sizeof(initial_coeffs_a_3800));
      memcpy(p->coeffsA[1], initial_coeffs_a_3800, sizeof(initial_coeffs_a_3800));
    }
  }
  else
  {
    memcpy(p->coeffsA[0], initial_coeffs_3930, sizeof(initial_coeffs_3930));
    memcpy(p->coeffsA[1], initial_coeffs_3930, sizeof(initial_coeffs_3930));
    memcpy(p64->coeffsA[0], initial_coeffs_3930_64bit, sizeof(initial_coeffs_3930_64bit));
    memcpy(p64->coeffsA[1], initial_coeffs_3930_64bit, sizeof(initial_coeffs_3930_64bit));
  }
  memset(p->coeffsB, 0, sizeof(p->coeffsB));
  memset(p64->coeffsB, 0, sizeof(p64->coeffsB));
  if (ctx->fileversion < 3930)
  {
    memcpy(p->coeffsB[0], initial_coeffs_b_3800, sizeof(initial_coeffs_b_3800));
    memcpy(p->coeffsB[1], initial_coeffs_b_3800, sizeof(initial_coeffs_b_3800));
  }

  p->filterA[0] = p->filterA[1] = 0;
  p->filterB[0] = p->filterB[1] = 0;
  p->lastA[0] = p->lastA[1] = 0;

  p64->filterA[0] = p64->filterA[1] = 0;
  p64->filterB[0] = p64->filterB[1] = 0;
  p64->lastA[0] = p64->lastA[1] = 0;

  p->sample_pos = 0;
}

static inline int filter_fast_3320(APEPredictor *p, const int decoded,
                                   const int filter, const int delayA)
{
  int32_t predictionA;

  p->buf[delayA] = p->lastA[filter];
  if (p->sample_pos < 3)
  {
    p->lastA[filter] = decoded;
    p->filterA[filter] = decoded;
    return decoded;
  }

  predictionA = static_cast<int32_t>(p->buf[delayA] * 2U - static_cast<uint32_t>(p->buf[delayA - 1]));
  p->lastA[filter] = decoded + static_cast<uint32_t>((predictionA * p->coeffsA[filter][0]) >> 9);

  if ((decoded ^ predictionA) > 0)
    p->coeffsA[filter][0]++;
  else
    p->coeffsA[filter][0]--;

  p->filterA[filter] += static_cast<uint32_t>(p->lastA[filter]);

  return p->filterA[filter];
}

static inline int filter_3800(APEPredictor *p, const unsigned decoded,
                              const int filter, const int delayA,
                              const int delayB, const int start,
                              const int shift)
{
  int32_t predictionA, predictionB, sign;
  int32_t d0, d1, d2, d3, d4;

  p->buf[delayA] = p->lastA[filter];
  p->buf[delayB] = p->filterB[filter];
  if (p->sample_pos < static_cast<unsigned>(start))
  {
    predictionA = static_cast<int32_t>(decoded) + p->filterA[filter];
    p->lastA[filter] = static_cast<int32_t>(decoded);
    p->filterB[filter] = static_cast<int32_t>(decoded);
    p->filterA[filter] = predictionA;
    return predictionA;
  }
  d2 = p->buf[delayA];
  d1 = static_cast<int32_t>((p->buf[delayA] - static_cast<uint32_t>(p->buf[delayA - 1])) * 2);
  d0 = p->buf[delayA] + static_cast<int32_t>((p->buf[delayA - 2] - static_cast<uint32_t>(p->buf[delayA - 1])) * 8);
  d3 = static_cast<int32_t>(p->buf[delayB] * 2U - static_cast<uint32_t>(p->buf[delayB - 1]));
  d4 = p->buf[delayB];

  predictionA = d0 * static_cast<int32_t>(p->coeffsA[filter][0]) +
                d1 * static_cast<int32_t>(p->coeffsA[filter][1]) +
                d2 * static_cast<int32_t>(p->coeffsA[filter][2]);

  sign = APESIGN(static_cast<int32_t>(decoded));
  p->coeffsA[filter][0] += static_cast<uint32_t>((((d0 >> 30) & 2) - 1) * sign);
  p->coeffsA[filter][1] += static_cast<uint32_t>((((d1 >> 28) & 8) - 4) * sign);
  p->coeffsA[filter][2] += static_cast<uint32_t>((((d2 >> 28) & 8) - 4) * sign);

  predictionB = d3 * static_cast<int32_t>(p->coeffsB[filter][0]) -
                d4 * static_cast<int32_t>(p->coeffsB[filter][1]);
  p->lastA[filter] = static_cast<int32_t>(decoded) + (predictionA >> 11);
  sign = APESIGN(p->lastA[filter]);
  p->coeffsB[filter][0] += static_cast<uint32_t>((((d3 >> 29) & 4) - 2) * sign);
  p->coeffsB[filter][1] -= static_cast<uint32_t>((((d4 >> 30) & 2) - 1) * sign);

  p->filterB[filter] = p->lastA[filter] + static_cast<uint32_t>(predictionB >> shift);
  p->filterA[filter] = p->filterB[filter] + static_cast<uint32_t>((static_cast<int32_t>(p->filterA[filter] * 31U)) >> 5);

  return p->filterA[filter];
}

static void long_filter_high_3800(int32_t *buffer, int order, int shift, int length)
{
  int i, j;
  int32_t dotprod, sign;
  int32_t coeffs[256], delay[256 + 256], *delayp = delay;

  if (order >= length) return;

  memset(coeffs, 0, static_cast<size_t>(order) * sizeof(*coeffs));
  for (i = 0; i < order; i++) delay[i] = buffer[i];
  for (i = order; i < length; i++)
  {
    dotprod = 0;
    sign = APESIGN(buffer[i]);
    if (sign == 1)
    {
      for (j = 0; j < order; j++)
      {
        dotprod += static_cast<int32_t>(delayp[j] * static_cast<uint32_t>(coeffs[j]));
        coeffs[j] += (delayp[j] >> 31) | 1;
      }
    }
    else if (sign == -1)
    {
      for (j = 0; j < order; j++)
      {
        dotprod += static_cast<int32_t>(delayp[j] * static_cast<uint32_t>(coeffs[j]));
        coeffs[j] -= (delayp[j] >> 31) | 1;
      }
    }
    else
    {
      for (j = 0; j < order; j++)
        dotprod += static_cast<int32_t>(delayp[j] * static_cast<uint32_t>(coeffs[j]));
    }
    buffer[i] -= static_cast<uint32_t>(dotprod >> shift);
    delayp++;
    delayp[order - 1] = buffer[i];
    if (delayp - delay == 256)
    {
      memcpy(delay, delayp, sizeof(*delay) * 256);
      delayp = delay;
    }
  }
}

static void long_filter_ehigh_3830(int32_t *buffer, int length)
{
  int i, j;
  int32_t dotprod, sign;
  int32_t delay[8] = {0};
  uint32_t coeffs[8] = {0};

  for (i = 0; i < length; i++)
  {
    dotprod = 0;
    sign = APESIGN(buffer[i]);
    for (j = 7; j >= 0; j--)
    {
      dotprod += static_cast<int32_t>(delay[j] * coeffs[j]);
      coeffs[j] += static_cast<uint32_t>(((delay[j] >> 31) | 1) * sign);
    }
    for (j = 7; j > 0; j--) delay[j] = delay[j - 1];
    delay[0] = buffer[i];
    buffer[i] -= static_cast<uint32_t>(dotprod >> 9);
  }
}

static void predictor_decode_stereo_3800(ApeCtx *ctx, int count)
{
  APEPredictor *p = &ctx->predictor;
  int32_t *decoded0 = ctx->decoded[0];
  int32_t *decoded1 = ctx->decoded[1];
  int start = 4, shift = 10;

  if (ctx->compression_level == COMPRESSION_LEVEL_HIGH)
  {
    start = 16;
    long_filter_high_3800(decoded0, 16, 9, count);
    long_filter_high_3800(decoded1, 16, 9, count);
  }
  else if (ctx->compression_level == COMPRESSION_LEVEL_EXTRA_HIGH)
  {
    int order = 128, shift2 = 11;

    if (ctx->fileversion >= 3830)
    {
      order <<= 1;
      shift++;
      shift2++;
      long_filter_ehigh_3830(decoded0 + order, count - order);
      long_filter_ehigh_3830(decoded1 + order, count - order);
    }
    start = order;
    long_filter_high_3800(decoded0, order, shift2, count);
    long_filter_high_3800(decoded1, order, shift2, count);
  }

  while (count--)
  {
    int X = *decoded0, Y = *decoded1;
    if (ctx->compression_level == COMPRESSION_LEVEL_FAST)
    {
      *decoded0 = filter_fast_3320(p, Y, 0, YDELAYA);
      decoded0++;
      *decoded1 = filter_fast_3320(p, X, 1, XDELAYA);
      decoded1++;
    }
    else
    {
      *decoded0 = filter_3800(p, static_cast<unsigned>(Y), 0, YDELAYA, YDELAYB, start, shift);
      decoded0++;
      *decoded1 = filter_3800(p, static_cast<unsigned>(X), 1, XDELAYA, XDELAYB, start, shift);
      decoded1++;
    }

    p->buf++;
    p->sample_pos++;

    if (p->buf == p->historybuffer + HISTORY_SIZE)
    {
      memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(*p->historybuffer));
      p->buf = p->historybuffer;
    }
  }
}

static void predictor_decode_mono_3800(ApeCtx *ctx, int count)
{
  APEPredictor *p = &ctx->predictor;
  int32_t *decoded0 = ctx->decoded[0];
  int start = 4, shift = 10;

  if (ctx->compression_level == COMPRESSION_LEVEL_HIGH)
  {
    start = 16;
    long_filter_high_3800(decoded0, 16, 9, count);
  }
  else if (ctx->compression_level == COMPRESSION_LEVEL_EXTRA_HIGH)
  {
    int order = 128, shift2 = 11;

    if (ctx->fileversion >= 3830)
    {
      order <<= 1;
      shift++;
      shift2++;
      long_filter_ehigh_3830(decoded0 + order, count - order);
    }
    start = order;
    long_filter_high_3800(decoded0, order, shift2, count);
  }

  while (count--)
  {
    if (ctx->compression_level == COMPRESSION_LEVEL_FAST)
    {
      *decoded0 = filter_fast_3320(p, *decoded0, 0, YDELAYA);
      decoded0++;
    }
    else
    {
      *decoded0 = filter_3800(p, static_cast<unsigned>(*decoded0), 0, YDELAYA, YDELAYB, start, shift);
      decoded0++;
    }

    p->buf++;
    p->sample_pos++;

    if (p->buf == p->historybuffer + HISTORY_SIZE)
    {
      memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(*p->historybuffer));
      p->buf = p->historybuffer;
    }
  }
}

static inline int predictor_update_3930(APEPredictor *p, const int decoded,
                                        const int filter, const int delayA)
{
  int32_t predictionA, sign;
  uint32_t d0, d1, d2, d3;

  p->buf[delayA] = p->lastA[filter];
  d0 = static_cast<uint32_t>(p->buf[delayA]);
  d1 = static_cast<uint32_t>(p->buf[delayA]) - static_cast<uint32_t>(p->buf[delayA - 1]);
  d2 = static_cast<uint32_t>(p->buf[delayA - 1]) - static_cast<uint32_t>(p->buf[delayA - 2]);
  d3 = static_cast<uint32_t>(p->buf[delayA - 2]) - static_cast<uint32_t>(p->buf[delayA - 3]);

  predictionA = static_cast<int32_t>(d0 * static_cast<uint32_t>(p->coeffsA[filter][0]) +
                                     d1 * static_cast<uint32_t>(p->coeffsA[filter][1]) +
                                     d2 * static_cast<uint32_t>(p->coeffsA[filter][2]) +
                                     d3 * static_cast<uint32_t>(p->coeffsA[filter][3]));

  p->lastA[filter] = decoded + (predictionA >> 9);
  p->filterA[filter] = p->lastA[filter] + ((static_cast<int32_t>(p->filterA[filter] * 31U)) >> 5);

  sign = APESIGN(decoded);
  p->coeffsA[filter][0] += static_cast<uint32_t>(((static_cast<int32_t>(d0) < 0) * 2 - 1) * sign);
  p->coeffsA[filter][1] += static_cast<uint32_t>(((static_cast<int32_t>(d1) < 0) * 2 - 1) * sign);
  p->coeffsA[filter][2] += static_cast<uint32_t>(((static_cast<int32_t>(d2) < 0) * 2 - 1) * sign);
  p->coeffsA[filter][3] += static_cast<uint32_t>(((static_cast<int32_t>(d3) < 0) * 2 - 1) * sign);

  return p->filterA[filter];
}

static void predictor_decode_stereo_3930(ApeCtx *ctx, int count)
{
  APEPredictor *p = &ctx->predictor;
  int32_t *decoded0 = ctx->decoded[0];
  int32_t *decoded1 = ctx->decoded[1];

  ape_apply_filters(ctx, ctx->decoded[0], ctx->decoded[1], count);

  while (count--)
  {
    int Y = *decoded1, X = *decoded0;
    *decoded0 = predictor_update_3930(p, Y, 0, YDELAYA);
    decoded0++;
    *decoded1 = predictor_update_3930(p, X, 1, XDELAYA);
    decoded1++;

    p->buf++;

    if (p->buf == p->historybuffer + HISTORY_SIZE)
    {
      memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(*p->historybuffer));
      p->buf = p->historybuffer;
    }
  }
}

static void predictor_decode_mono_3930(ApeCtx *ctx, int count)
{
  APEPredictor *p = &ctx->predictor;
  int32_t *decoded0 = ctx->decoded[0];

  ape_apply_filters(ctx, ctx->decoded[0], nullptr, count);

  while (count--)
  {
    *decoded0 = predictor_update_3930(p, *decoded0, 0, YDELAYA);
    decoded0++;

    p->buf++;

    if (p->buf == p->historybuffer + HISTORY_SIZE)
    {
      memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(*p->historybuffer));
      p->buf = p->historybuffer;
    }
  }
}

static inline int64_t predictor_update_filter(APEPredictor64 *p, const int decoded,
                                              const int filter, const int delayA,
                                              const int delayB, const int adaptA,
                                              const int adaptB, int interim_mode)
{
  int64_t predictionA, predictionB;
  int32_t sign;

  p->buf[delayA] = p->lastA[filter];
  p->buf[adaptA] = APESIGN(p->buf[delayA]);
  p->buf[delayA - 1] = p->buf[delayA] - static_cast<uint64_t>(p->buf[delayA - 1]);
  p->buf[adaptA - 1] = APESIGN(p->buf[delayA - 1]);

  predictionA = p->buf[delayA] * static_cast<int64_t>(p->coeffsA[filter][0]) +
                p->buf[delayA - 1] * static_cast<int64_t>(p->coeffsA[filter][1]) +
                p->buf[delayA - 2] * static_cast<int64_t>(p->coeffsA[filter][2]) +
                p->buf[delayA - 3] * static_cast<int64_t>(p->coeffsA[filter][3]);

  p->buf[delayB] = p->filterA[filter ^ 1] - (static_cast<int64_t>(p->filterB[filter] * 31ULL) >> 5);
  p->buf[adaptB] = APESIGN(p->buf[delayB]);
  p->buf[delayB - 1] = p->buf[delayB] - static_cast<uint64_t>(p->buf[delayB - 1]);
  p->buf[adaptB - 1] = APESIGN(p->buf[delayB - 1]);
  p->filterB[filter] = p->filterA[filter ^ 1];

  predictionB = p->buf[delayB] * static_cast<int64_t>(p->coeffsB[filter][0]) +
                p->buf[delayB - 1] * static_cast<int64_t>(p->coeffsB[filter][1]) +
                p->buf[delayB - 2] * static_cast<int64_t>(p->coeffsB[filter][2]) +
                p->buf[delayB - 3] * static_cast<int64_t>(p->coeffsB[filter][3]) +
                p->buf[delayB - 4] * static_cast<int64_t>(p->coeffsB[filter][4]);

  if (interim_mode < 1)
  {
    predictionA = static_cast<int32_t>(predictionA);
    predictionB = static_cast<int32_t>(predictionB);
    p->lastA[filter] = static_cast<int32_t>(
        decoded + static_cast<uint32_t>(static_cast<int32_t>(predictionA + (predictionB >> 1)) >> 10));
  }
  else
  {
    p->lastA[filter] = decoded + (static_cast<int64_t>(static_cast<uint64_t>(predictionA) + (predictionB >> 1)) >> 10);
  }
  p->filterA[filter] = p->lastA[filter] + (static_cast<int64_t>(p->filterA[filter] * 31ULL) >> 5);

  sign = APESIGN(decoded);
  p->coeffsA[filter][0] += static_cast<uint64_t>(p->buf[adaptA] * sign);
  p->coeffsA[filter][1] += static_cast<uint64_t>(p->buf[adaptA - 1] * sign);
  p->coeffsA[filter][2] += static_cast<uint64_t>(p->buf[adaptA - 2] * sign);
  p->coeffsA[filter][3] += static_cast<uint64_t>(p->buf[adaptA - 3] * sign);
  p->coeffsB[filter][0] += static_cast<uint64_t>(p->buf[adaptB] * sign);
  p->coeffsB[filter][1] += static_cast<uint64_t>(p->buf[adaptB - 1] * sign);
  p->coeffsB[filter][2] += static_cast<uint64_t>(p->buf[adaptB - 2] * sign);
  p->coeffsB[filter][3] += static_cast<uint64_t>(p->buf[adaptB - 3] * sign);
  p->coeffsB[filter][4] += static_cast<uint64_t>(p->buf[adaptB - 4] * sign);

  return p->filterA[filter];
}

static int32_t FFNABS(int32_t a) { return a <= 0 ? a : -a; }

static void predictor_decode_stereo_3950(ApeCtx *ctx, int count)
{
  APEPredictor64 *p_default = &ctx->predictor64;
  APEPredictor64 p_interim;
  int lcount = count;
  int num_passes = 1;

  ape_apply_filters(ctx, ctx->decoded[0], ctx->decoded[1], count);
  if (ctx->interim_mode == -1)
  {
    p_interim = *p_default;
    num_passes++;
    memcpy(ctx->interim[0], ctx->decoded[0], sizeof(*ctx->interim[0]) * static_cast<size_t>(count));
    memcpy(ctx->interim[1], ctx->decoded[1], sizeof(*ctx->interim[1]) * static_cast<size_t>(count));
  }

  for (int pass = 0; pass < num_passes; pass++)
  {
    int32_t *decoded0, *decoded1;
    int interim_mode = ctx->interim_mode > 0 || pass;
    APEPredictor64 *p;

    if (pass)
    {
      p = &p_interim;
      decoded0 = ctx->interim[0];
      decoded1 = ctx->interim[1];
    }
    else
    {
      p = p_default;
      decoded0 = ctx->decoded[0];
      decoded1 = ctx->decoded[1];
    }
    p->buf = p->historybuffer;

    count = lcount;
    while (count--)
    {
      int64_t a0 = predictor_update_filter(p, *decoded0, 0, YDELAYA, YDELAYB,
                                           YADAPTCOEFFSA, YADAPTCOEFFSB, interim_mode);
      int64_t a1 = predictor_update_filter(p, *decoded1, 1, XDELAYA, XDELAYB,
                                           XADAPTCOEFFSA, XADAPTCOEFFSB, interim_mode);
      *decoded0++ = static_cast<int32_t>(a0);
      *decoded1++ = static_cast<int32_t>(a1);
      if (num_passes > 1)
      {
        int32_t left = static_cast<int32_t>(a1 - static_cast<uint32_t>(a0 / 2));
        int32_t right = left + static_cast<int32_t>(a0);

        if (std::min(FFNABS(left), FFNABS(right)) < -(1 << 23))
        {
          ctx->interim_mode = !interim_mode;
          break;
        }
      }

      p->buf++;

      if (p->buf == p->historybuffer + HISTORY_SIZE)
      {
        memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(*p->historybuffer));
        p->buf = p->historybuffer;
      }
    }
  }
  if (num_passes > 1 && ctx->interim_mode > 0)
  {
    memcpy(ctx->decoded[0], ctx->interim[0], sizeof(*ctx->interim[0]) * static_cast<size_t>(lcount));
    memcpy(ctx->decoded[1], ctx->interim[1], sizeof(*ctx->interim[1]) * static_cast<size_t>(lcount));
    *p_default = p_interim;
    p_default->buf = p_default->historybuffer;
  }
}

static void predictor_decode_mono_3950(ApeCtx *ctx, int count)
{
  APEPredictor64 *p = &ctx->predictor64;
  int32_t *decoded0 = ctx->decoded[0];
  int64_t predictionA, currentA, A;
  int32_t sign;

  ape_apply_filters(ctx, ctx->decoded[0], nullptr, count);

  currentA = p->lastA[0];

  while (count--)
  {
    A = *decoded0;

    p->buf[YDELAYA] = currentA;
    p->buf[YDELAYA - 1] = p->buf[YDELAYA] - static_cast<uint64_t>(p->buf[YDELAYA - 1]);

    predictionA = p->buf[YDELAYA] * static_cast<int64_t>(p->coeffsA[0][0]) +
                  p->buf[YDELAYA - 1] * static_cast<int64_t>(p->coeffsA[0][1]) +
                  p->buf[YDELAYA - 2] * static_cast<int64_t>(p->coeffsA[0][2]) +
                  p->buf[YDELAYA - 3] * static_cast<int64_t>(p->coeffsA[0][3]);

    currentA = A + static_cast<uint64_t>(predictionA >> 10);

    p->buf[YADAPTCOEFFSA] = APESIGN(p->buf[YDELAYA]);
    p->buf[YADAPTCOEFFSA - 1] = APESIGN(p->buf[YDELAYA - 1]);

    sign = APESIGN(static_cast<int32_t>(A));
    p->coeffsA[0][0] += static_cast<uint64_t>(p->buf[YADAPTCOEFFSA] * sign);
    p->coeffsA[0][1] += static_cast<uint64_t>(p->buf[YADAPTCOEFFSA - 1] * sign);
    p->coeffsA[0][2] += static_cast<uint64_t>(p->buf[YADAPTCOEFFSA - 2] * sign);
    p->coeffsA[0][3] += static_cast<uint64_t>(p->buf[YADAPTCOEFFSA - 3] * sign);

    p->buf++;

    if (p->buf == p->historybuffer + HISTORY_SIZE)
    {
      memmove(p->historybuffer, p->buf, PREDICTOR_SIZE * sizeof(*p->historybuffer));
      p->buf = p->historybuffer;
    }

    p->filterA[0] = currentA + (static_cast<int64_t>(p->filterA[0] * 31U) >> 5);
    *(decoded0++) = static_cast<int32_t>(p->filterA[0]);
  }

  p->lastA[0] = currentA;
}

static void do_init_filter(APEFilter *f, int16_t *buf, int order)
{
  f->coeffs = buf;
  f->historybuffer = buf + order;
  f->delay = f->historybuffer + order * 2;
  f->adaptcoeffs = f->historybuffer + order;

  memset(f->historybuffer, 0, static_cast<size_t>(order * 2) * sizeof(*f->historybuffer));
  memset(f->coeffs, 0, static_cast<size_t>(order) * sizeof(*f->coeffs));
  f->avg = 0;
}

static void init_filter(APEFilter *f, int16_t *buf, int order)
{
  do_init_filter(&f[0], buf, order);
  do_init_filter(&f[1], buf + order * 3 + HISTORY_SIZE, order);
}

static void do_apply_filter(int version, APEFilter *f, int32_t *data, int count,
                            int order, int fracbits)
{
  int res;
  unsigned absres;

  while (count--)
  {
    res = scalarproduct_and_madd_int16(f->coeffs, f->delay - order,
                                       f->adaptcoeffs - order, order,
                                       APESIGN(*data));
    res = static_cast<int>(static_cast<int64_t>(res + (1LL << (fracbits - 1))) >> fracbits);
    res += static_cast<uint32_t>(*data);
    *data++ = res;

    *f->delay++ = static_cast<int16_t>(clip_int16(res));

    if (version < 3980)
    {
      f->adaptcoeffs[0] = static_cast<int16_t>((res == 0) ? 0 : ((res >> 28) & 8) - 4);
      f->adaptcoeffs[-4] = static_cast<int16_t>(f->adaptcoeffs[-4] >> 1);
      f->adaptcoeffs[-8] = static_cast<int16_t>(f->adaptcoeffs[-8] >> 1);
    }
    else
    {
      absres = FFABSU(res);
      if (absres)
        *f->adaptcoeffs = static_cast<int16_t>(
            APESIGN(res) *
            (8 << ((absres > f->avg * 3ULL) + (absres > (f->avg + f->avg / 3)))));
      else
        *f->adaptcoeffs = 0;

      f->avg += static_cast<uint32_t>(static_cast<int>(absres - f->avg) / 16);

      f->adaptcoeffs[-1] = static_cast<int16_t>(f->adaptcoeffs[-1] >> 1);
      f->adaptcoeffs[-2] = static_cast<int16_t>(f->adaptcoeffs[-2] >> 1);
      f->adaptcoeffs[-8] = static_cast<int16_t>(f->adaptcoeffs[-8] >> 1);
    }

    f->adaptcoeffs++;

    if (f->delay == f->historybuffer + HISTORY_SIZE + (order * 2))
    {
      memmove(f->historybuffer, f->delay - (order * 2),
              static_cast<size_t>(order * 2) * sizeof(*f->historybuffer));
      f->delay = f->historybuffer + order * 2;
      f->adaptcoeffs = f->historybuffer + order;
    }
  }
}

static void apply_filter(ApeCtx *ctx, APEFilter *f, int32_t *data0, int32_t *data1,
                         int count, int order, int fracbits)
{
  do_apply_filter(ctx->fileversion, &f[0], data0, count, order, fracbits);
  if (data1) do_apply_filter(ctx->fileversion, &f[1], data1, count, order, fracbits);
}

static void ape_apply_filters(ApeCtx *ctx, int32_t *decoded0, int32_t *decoded1,
                              int count)
{
  for (int i = 0; i < APE_FILTER_LEVELS; i++)
  {
    if (!ape_filter_orders[ctx->fset][i]) break;
    apply_filter(ctx, ctx->filters[i], decoded0, decoded1, count,
                ape_filter_orders[ctx->fset][i], ape_filter_fracbits[ctx->fset][i]);
  }
}

static int init_frame_decoder(ApeCtx *ctx)
{
  if (init_entropy_decoder(ctx) < 0) return -1;
  init_predictor_decoder(ctx);

  for (int i = 0; i < APE_FILTER_LEVELS; i++)
  {
    if (!ape_filter_orders[ctx->fset][i]) break;
    init_filter(ctx->filters[i], ctx->filterbuf[i].data(), ape_filter_orders[ctx->fset][i]);
  }
  return 0;
}

static void ape_unpack_mono(ApeCtx *ctx, int count)
{
  if (ctx->frameflags & APE_FRAMECODE_STEREO_SILENCE)
  {
    return;
  }

  ctx->entropy_decode_mono(ctx, count);
  if (ctx->error) return;

  ctx->predictor_decode_mono(ctx, count);

  if (ctx->channels == 2)
    memcpy(ctx->decoded[1], ctx->decoded[0], static_cast<size_t>(count) * sizeof(*ctx->decoded[1]));
}

static void ape_unpack_stereo(ApeCtx *ctx, int count)
{
  unsigned left, right;
  int32_t *decoded0 = ctx->decoded[0];
  int32_t *decoded1 = ctx->decoded[1];

  if ((ctx->frameflags & APE_FRAMECODE_STEREO_SILENCE) == APE_FRAMECODE_STEREO_SILENCE)
  {
    return;
  }

  ctx->entropy_decode_stereo(ctx, count);
  if (ctx->error) return;

  ctx->predictor_decode_stereo(ctx, count);

  while (count--)
  {
    left = static_cast<unsigned>(*decoded1) - static_cast<unsigned>(*decoded0 / 2);
    right = left + static_cast<unsigned>(*decoded0);

    *(decoded0++) = static_cast<int32_t>(left);
    *(decoded1++) = static_cast<int32_t>(right);
  }
}

} // namespace

// ===========================================================================
// mocf plugin glue: container parsing (adapted from ape.c) + decode driver
// ===========================================================================

namespace {

struct ApeFrame
{
  int64_t pos = 0;
  int64_t size = 0;
  int nblocks = 0;
  int skip = 0;
};

struct ape_data
{
  unique_io_stream io_stream;

  // Descriptor/header fields (ape.c's APEContext)
  int fileversion = 0;
  uint32_t junklength = 0;
  uint32_t firstframe = 0;
  uint32_t totalsamples = 0;
  uint16_t compressiontype = 0;
  uint16_t formatflags = 0;
  uint32_t blocksperframe = 0;
  uint32_t finalframeblocks = 0;
  uint32_t totalframes = 0;
  uint16_t bps = 0;
  uint16_t channels = 0;
  uint32_t samplerate = 0;

  std::vector<ApeFrame> frames;
  uint32_t currentframe = 0;

  // Decode state
  std::unique_ptr<ApeCtx> ctx;
  bool frame_loaded = false;    // is a frame's raw bytes currently in ctx->data?
  int duration_sec = -1;
  int64_t total_blocks = 0;
  struct decoder_error error;
  bool ok = false;
};

static void ape_setup_dispatch(ApeCtx *ctx)
{
  if (ctx->fileversion < 3860)
  {
    ctx->entropy_decode_mono = entropy_decode_mono_0000;
    ctx->entropy_decode_stereo = entropy_decode_stereo_0000;
  }
  else if (ctx->fileversion < 3900)
  {
    ctx->entropy_decode_mono = entropy_decode_mono_3860;
    ctx->entropy_decode_stereo = entropy_decode_stereo_3860;
  }
  else if (ctx->fileversion < 3930)
  {
    ctx->entropy_decode_mono = entropy_decode_mono_3900;
    ctx->entropy_decode_stereo = entropy_decode_stereo_3900;
  }
  else if (ctx->fileversion < 3990)
  {
    ctx->entropy_decode_mono = entropy_decode_mono_3900;
    ctx->entropy_decode_stereo = entropy_decode_stereo_3930;
  }
  else
  {
    ctx->entropy_decode_mono = entropy_decode_mono_3990;
    ctx->entropy_decode_stereo = entropy_decode_stereo_3990;
  }

  if (ctx->fileversion < 3930)
  {
    ctx->predictor_decode_mono = predictor_decode_mono_3800;
    ctx->predictor_decode_stereo = predictor_decode_stereo_3800;
  }
  else if (ctx->fileversion < 3950)
  {
    ctx->predictor_decode_mono = predictor_decode_mono_3930;
    ctx->predictor_decode_stereo = predictor_decode_stereo_3930;
  }
  else
  {
    ctx->predictor_decode_mono = predictor_decode_mono_3950;
    ctx->predictor_decode_stereo = predictor_decode_stereo_3950;
  }
}

/* Mirrors ape_read_header() in ape.c, reading directly from an
 * io_stream instead of an AVIOContext. Returns false on failure. */
static bool ape_read_header(struct ape_data *data)
{
  io_stream *pb = data->io_stream.get();

  data->junklength = static_cast<uint32_t>(io_tell(pb));

  uint8_t tagbuf[4];
  if (io_read(pb, tagbuf, 4) != 4) return false;
  if (memcmp(tagbuf, "MAC ", 4) != 0) return false;

  auto read_u16 = [&]() -> uint16_t {
    uint8_t b[2];
    io_read(pb, b, 2);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
  };
  auto read_u32 = [&]() -> uint32_t {
    uint8_t b[4];
    io_read(pb, b, 4);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
          (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
  };

  data->fileversion = read_u16();

  if (data->fileversion < APE_MIN_VERSION || data->fileversion > APE_MAX_VERSION)
  {
    return false;
  }

  uint32_t descriptorlength = 0, headerlength = 0, seektablelength = 0,
           wavheaderlength = 0, wavtaillength = 0;

  if (data->fileversion >= 3980)
  {
    read_u16(); // padding1
    descriptorlength = read_u32();
    headerlength = read_u32();
    seektablelength = read_u32();
    wavheaderlength = read_u32();
    read_u32(); // audiodatalength
    read_u32(); // audiodatalength_high
    wavtaillength = read_u32();
    uint8_t md5[16];
    io_read(pb, md5, 16);

    if (descriptorlength > 52) io_seek(pb, descriptorlength - 52, SEEK_CUR);

    data->compressiontype = read_u16();
    data->formatflags = read_u16();
    data->blocksperframe = read_u32();
    data->finalframeblocks = read_u32();
    data->totalframes = read_u32();
    data->bps = read_u16();
    data->channels = read_u16();
    data->samplerate = read_u32();
  }
  else
  {
    descriptorlength = 0;
    headerlength = 32;

    data->compressiontype = read_u16();
    data->formatflags = read_u16();
    data->channels = read_u16();
    data->samplerate = read_u32();
    wavheaderlength = read_u32();
    wavtaillength = read_u32();
    data->totalframes = read_u32();
    data->finalframeblocks = read_u32();

    if (data->formatflags & MAC_FORMAT_FLAG_HAS_PEAK_LEVEL)
    {
      io_seek(pb, 4, SEEK_CUR);
      headerlength += 4;
    }

    if (data->formatflags & MAC_FORMAT_FLAG_HAS_SEEK_ELEMENTS)
    {
      seektablelength = read_u32();
      headerlength += 4;
      seektablelength *= sizeof(int32_t);
    }
    else
    {
      seektablelength = data->totalframes * sizeof(int32_t);
    }

    if (data->formatflags & MAC_FORMAT_FLAG_8_BIT)
      data->bps = 8;
    else if (data->formatflags & MAC_FORMAT_FLAG_24_BIT)
      data->bps = 24;
    else
      data->bps = 16;

    if (data->fileversion >= 3950)
      data->blocksperframe = 73728 * 4;
    else if (data->fileversion >= 3900 || (data->fileversion >= 3800 && data->compressiontype >= 4000))
      data->blocksperframe = 73728;
    else
      data->blocksperframe = 9216;

    /* Old-format files store the WAV header inline, immediately after
     * the fixed header fields/flags and *before* the seektable - not
     * after, despite firstframe's length arithmetic below being a sum
     * that doesn't care about region order. Must actually skip these
     * bytes now so the seektable read further down lands in the right
     * place, matching ape.c's avio_skip(pb, wavheaderlength) here. */
    if (wavheaderlength) io_seek(pb, wavheaderlength, SEEK_CUR);
  }

  if (data->channels > 2 || data->channels < 1) return false;
  if (data->totalframes > UINT_MAX / sizeof(ApeFrame)) return false;
  if (data->totalframes == 0) return false;
  if (seektablelength / sizeof(uint32_t) < data->totalframes) return false;

  data->frames.resize(data->totalframes);
  data->firstframe = data->junklength + descriptorlength + headerlength +
                    seektablelength + wavheaderlength;
  if (data->fileversion < 3810) data->firstframe += data->totalframes;
  data->currentframe = 0;

  data->totalsamples = data->finalframeblocks;
  if (data->totalframes > 1)
    data->totalsamples += data->blocksperframe * (data->totalframes - 1);

  data->frames[0].pos = data->firstframe;
  data->frames[0].nblocks = static_cast<int>(data->blocksperframe);
  data->frames[0].skip = 0;
  read_u32(); // seektable[0]
  for (uint32_t i = 1; i < data->totalframes; i++)
  {
    uint32_t seektable_entry = read_u32();
    data->frames[i].pos = static_cast<int64_t>(seektable_entry) + data->junklength;
    data->frames[i].nblocks = static_cast<int>(data->blocksperframe);
    data->frames[i - 1].size = data->frames[i].pos - data->frames[i - 1].pos;
    data->frames[i].skip = static_cast<int>((data->frames[i].pos - data->frames[0].pos) & 3);
  }
  {
    int64_t skip_remaining = static_cast<int64_t>(seektablelength / sizeof(uint32_t)) -
                             data->totalframes;
    if (skip_remaining > 0) io_seek(pb, skip_remaining * 4, SEEK_CUR);
  }

  data->frames[data->totalframes - 1].nblocks = static_cast<int>(data->finalframeblocks);

  int64_t file_size = io_file_size(pb);
  int64_t final_size = 0;
  if (file_size > 0)
  {
    final_size = file_size - data->frames[data->totalframes - 1].pos - wavtaillength;
    final_size -= final_size & 3;
  }
  if (file_size <= 0 || final_size <= 0)
    final_size = static_cast<int64_t>(data->finalframeblocks) * 8;
  data->frames[data->totalframes - 1].size = final_size;

  for (uint32_t i = 0; i < data->totalframes; i++)
  {
    if (data->frames[i].skip)
    {
      data->frames[i].pos -= data->frames[i].skip;
      data->frames[i].size += data->frames[i].skip;
    }
    if (data->frames[i].size > INT_MAX - 3) return false;
    data->frames[i].size = (data->frames[i].size + 3) & ~3;
  }
  if (data->fileversion < 3810)
  {
    for (uint32_t i = 0; i < data->totalframes; i++)
    {
      uint8_t bits = 0;
      io_read(pb, &bits, 1);
      if (i && bits) data->frames[i - 1].size += 4;

      data->frames[i].skip <<= 3;
      data->frames[i].skip += bits;
    }
  }

  data->total_blocks =
      (data->totalframes == 0)
          ? 0
          : (static_cast<int64_t>(data->totalframes - 1) * data->blocksperframe) +
                data->finalframeblocks;

  return true;
}

/* Load frame `frame_idx`'s raw bytes into ctx->data (with the
 * bswap_buf byte-swizzle apedec.c applies), matching what
 * ape_decode_frame() expects on a fresh packet. */
static bool ape_load_frame(struct ape_data *data, uint32_t frame_idx)
{
  ApeCtx *ctx = data->ctx.get();
  io_stream *pb = data->io_stream.get();

  const ApeFrame &fr = data->frames[frame_idx];
  if (fr.size <= 0) return false;

  if (io_seek(pb, fr.pos, SEEK_SET) == -1) return false;

  /* Mirrors ape_read_packet(): the demuxer builds a packet consisting
   * of an 8-byte synthetic header (nblocks, skip - both little-endian,
   * NOT read from the file) followed by the frame's real file bytes.
   * apedec.c's ape_decode_frame() then byte-swaps that *entire*
   * combined buffer (header included) before parsing it - the
   * synthetic header is written little-endian specifically so that,
   * after the swap, reading it back as big-endian recovers the
   * original values. */
  constexpr int64_t extra_size = 8;
  int32_t nblocks_val = fr.nblocks;
  int32_t skip_val = fr.skip;

  int64_t packet_size = fr.size + extra_size;
  int64_t buf_size = packet_size & ~3;
  if (ctx->fileversion < 3950) buf_size += 2;

  std::vector<uint8_t> raw(static_cast<size_t>(fr.size));
  ssize_t got = io_read(pb, raw.data(), static_cast<size_t>(fr.size));
  if (got < 0) return false;

  std::vector<uint8_t> packet(static_cast<size_t>(packet_size), 0);
  packet[0] = static_cast<uint8_t>(nblocks_val & 0xff);
  packet[1] = static_cast<uint8_t>((nblocks_val >> 8) & 0xff);
  packet[2] = static_cast<uint8_t>((nblocks_val >> 16) & 0xff);
  packet[3] = static_cast<uint8_t>((nblocks_val >> 24) & 0xff);
  packet[4] = static_cast<uint8_t>(skip_val & 0xff);
  packet[5] = static_cast<uint8_t>((skip_val >> 8) & 0xff);
  packet[6] = static_cast<uint8_t>((skip_val >> 16) & 0xff);
  packet[7] = static_cast<uint8_t>((skip_val >> 24) & 0xff);
  memcpy(packet.data() + extra_size, raw.data(), static_cast<size_t>(got));

  ctx->data.assign(static_cast<size_t>(buf_size), 0);

  /* Byte-swap the combined buffer 32 bits at a time (apedec.c's
   * bswap_buf()), reading past `packet_size` into guaranteed-zeroed
   * padding for the fileversion<3950 "overread two bytes" case. */
  size_t nwords = static_cast<size_t>(buf_size) >> 2;
  for (size_t w = 0; w < nwords; w++)
  {
    size_t o = w * 4;
    uint8_t b0 = (o + 0 < packet.size()) ? packet[o + 0] : 0;
    uint8_t b1 = (o + 1 < packet.size()) ? packet[o + 1] : 0;
    uint8_t b2 = (o + 2 < packet.size()) ? packet[o + 2] : 0;
    uint8_t b3 = (o + 3 < packet.size()) ? packet[o + 3] : 0;
    ctx->data[o + 0] = b3;
    ctx->data[o + 1] = b2;
    ctx->data[o + 2] = b1;
    ctx->data[o + 3] = b0;
  }

  ctx->ptr = ctx->data.data();
  ctx->data_end = ctx->data.data() + buf_size;

  uint32_t nblocks = bytestream_get_be32(&ctx->ptr);
  uint32_t offset = bytestream_get_be32(&ctx->ptr);

  if (ctx->fileversion >= 3900)
  {
    if (offset > 3) return false;
    if (ctx->data_end - ctx->ptr < offset) return false;
    ctx->ptr += offset;
  }
  else
  {
    ctx->gb.init(ctx->ptr, ctx->data_end - ctx->ptr);
    if (ctx->fileversion > 3800)
      ctx->gb.skip_bits_long(static_cast<int64_t>(offset) * 8);
    else
      ctx->gb.skip_bits_long(offset);
  }

  if (!nblocks) return false;

  if (init_frame_decoder(ctx) < 0) return false;

  ctx->samples = static_cast<int>(nblocks);
  data->frame_loaded = true;
  return true;
}

/* Decode up to blockstodecode blocks from the currently-loaded frame
 * into ctx->decoded[], mirroring the body of ape_decode_frame() after
 * the "get a fresh packet" branch. Returns blocks actually decoded, or
 * 0 on error/exhaustion. */
static int ape_decode_blocks(struct ape_data *data, int blockstodecode)
{
  ApeCtx *ctx = data->ctx.get();

  blockstodecode = std::min(ctx->blocks_per_loop, std::min(blockstodecode, ctx->samples));
  if (ctx->fileversion < 3930) blockstodecode = ctx->samples;

  int aligned = (blockstodecode + 7) & ~7;
  size_t needed = static_cast<size_t>(aligned) * 2;

  if (ctx->decoded_buffer.size() < needed) ctx->decoded_buffer.assign(needed, 0);
  else std::fill(ctx->decoded_buffer.begin(), ctx->decoded_buffer.begin() + static_cast<long>(needed), 0);
  ctx->decoded[0] = ctx->decoded_buffer.data();
  ctx->decoded[1] = ctx->decoded_buffer.data() + aligned;

  if (ctx->interim_mode < 0)
  {
    if (ctx->interim_buffer.size() < needed) ctx->interim_buffer.assign(needed, 0);
    else std::fill(ctx->interim_buffer.begin(), ctx->interim_buffer.begin() + static_cast<long>(needed), 0);
    ctx->interim[0] = ctx->interim_buffer.data();
    ctx->interim[1] = ctx->interim_buffer.data() + aligned;
  }
  else
  {
    ctx->interim[0] = ctx->interim[1] = nullptr;
  }

  ctx->error = 0;

  if ((ctx->channels == 1) || (ctx->frameflags & APE_FRAMECODE_PSEUDO_STEREO))
    ape_unpack_mono(ctx, blockstodecode);
  else
    ape_unpack_stereo(ctx, blockstodecode);

  if (ctx->error) return 0;

  ctx->samples -= blockstodecode;
  if (ctx->samples <= 0) data->frame_loaded = false;

  return blockstodecode;
}

static void ape_pack_output(struct ape_data *data, int blockstodecode, char *out)
{
  ApeCtx *ctx = data->ctx.get();

  switch (ctx->bps)
  {
  case 8:
  {
    auto *o = reinterpret_cast<uint8_t *>(out);
    for (int ch = 0; ch < ctx->channels; ch++)
      for (int i = 0; i < blockstodecode; i++)
        o[i * ctx->channels + ch] =
            static_cast<uint8_t>((ctx->decoded[ch][i] + 0x80) & 0xff);
    break;
  }
  case 16:
  {
    auto *o = reinterpret_cast<int16_t *>(out);
    for (int ch = 0; ch < ctx->channels; ch++)
      for (int i = 0; i < blockstodecode; i++)
        o[i * ctx->channels + ch] = static_cast<int16_t>(ctx->decoded[ch][i]);
    break;
  }
  case 24:
  {
    auto *o = reinterpret_cast<int32_t *>(out);
    for (int ch = 0; ch < ctx->channels; ch++)
      for (int i = 0; i < blockstodecode; i++)
        o[i * ctx->channels + ch] =
            static_cast<int32_t>(static_cast<uint32_t>(ctx->decoded[ch][i]) * 256U);
    break;
  }
  }
}

} // namespace

// ---------------------------------------------------------------------
// mocf decoder/plugin interface
// ---------------------------------------------------------------------

static void *ape_open(const char *file)
{
  auto *data = new ape_data;
  decoder_error_init(&data->error);

  data->io_stream.reset(io_open(file, 1));
  if (!io_ok(data->io_stream.get()))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Can't open file: %s",
                  io_strerror(data->io_stream.get()));
    return data;
  }

  if (!ape_read_header(data))
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Not a valid or supported Monkey's Audio file: %s", file);
    return data;
  }

  if (data->bps != 8 && data->bps != 16 && data->bps != 24)
  {
    decoder_error(&data->error, ERROR_FATAL, 0,
                  "Unsupported bits per sample: %d", data->bps);
    return data;
  }

  data->ctx = std::make_unique<ApeCtx>();
  ApeCtx *ctx = data->ctx.get();

  ctx->channels = data->channels;
  ctx->bps = data->bps;
  ctx->fileversion = data->fileversion;
  ctx->compression_level = data->compressiontype;
  ctx->flags = data->formatflags;

  if (ctx->compression_level % 1000 || ctx->compression_level > COMPRESSION_LEVEL_INSANE ||
      !ctx->compression_level ||
      (ctx->fileversion < 3930 && ctx->compression_level == COMPRESSION_LEVEL_INSANE))
  {
    decoder_error(&data->error, ERROR_FATAL, 0, "Incorrect compression level %d",
                  ctx->compression_level);
    return data;
  }
  ctx->fset = ctx->compression_level / 1000 - 1;
  for (int i = 0; i < APE_FILTER_LEVELS; i++)
  {
    if (!ape_filter_orders[ctx->fset][i]) break;
    ctx->filterbuf[i].assign(
        static_cast<size_t>(ape_filter_orders[ctx->fset][i]) * 3 * 2 + HISTORY_SIZE * 2, 0);
  }

  ape_setup_dispatch(ctx);

  data->duration_sec = (data->samplerate > 0)
                          ? static_cast<int>(data->total_blocks / data->samplerate)
                          : 0;

  data->ok = true;
  debug("APE file opened. fileversion=%d bps=%d channels=%d rate=%u "
       "compression=%d duration=%d",
       data->fileversion, data->bps, data->channels, data->samplerate,
       data->compressiontype, data->duration_sec);

  return data;
}

static void ape_close(void *prv_data)
{
  auto *data = static_cast<struct ape_data *>(prv_data);
  decoder_error_clear(&data->error);
  delete data;
}

static void ape_get_error(void *prv_data, struct decoder_error *error)
{
  auto *data = static_cast<struct ape_data *>(prv_data);
  decoder_error_copy(error, &data->error);
}

static int ape_get_duration(void *prv_data)
{
  auto *data = static_cast<struct ape_data *>(prv_data);
  return data->duration_sec;
}

static int ape_get_bitrate(void *) { return -1; }

static int ape_seek(void *prv_data, int sec)
{
  auto *data = static_cast<struct ape_data *>(prv_data);
  ApeCtx *ctx = data->ctx.get();

  if (sec < 0 || data->duration_sec <= 0 || sec >= data->duration_sec ||
      data->samplerate == 0)
  {
    return -1;
  }

  int64_t target_block = static_cast<int64_t>(sec) * data->samplerate;

  int64_t block_acc = 0;
  uint32_t frame_idx = 0;
  for (; frame_idx < data->totalframes; frame_idx++)
  {
    int64_t frame_blocks = data->frames[frame_idx].nblocks;
    if (block_acc + frame_blocks > target_block) break;
    block_acc += frame_blocks;
  }
  if (frame_idx >= data->totalframes) return -1;

  data->frame_loaded = false;

  if (!ape_load_frame(data, frame_idx))
  {
    logit("ape: seek failed to load frame %u", frame_idx);
    return -1;
  }

  /* currentframe is the NEXT frame ape_decode() will load once this one
   * is exhausted; must point past frame_idx, not at it. */
  data->currentframe = frame_idx + 1;

  /* Advance within the frame toward target_block by decoding and
   * discarding whole blocks_per_loop chunks, same granularity as
   * ape_decode()'s normal playback path (never a partial chunk, which
   * would corrupt decoder state - see predictor_decode_stereo_3950()).
   * Only for fileversion >= 3930: older frames decode all-or-nothing
   * (ape_decode_blocks() forces blockstodecode = samples), so there's
   * no partial-frame position to advance to. */
  int64_t discarded = 0;
  if (ctx->fileversion >= 3930)
  {
    int64_t remainder = target_block - block_acc;
    int64_t to_discard = (remainder / ctx->blocks_per_loop) * ctx->blocks_per_loop;
    while (discarded < to_discard && data->frame_loaded)
    {
      int chunk =
          static_cast<int>(std::min<int64_t>(to_discard - discarded, ctx->blocks_per_loop));
      int got = ape_decode_blocks(data, chunk);
      if (got <= 0) break;
      discarded += got;
    }
  }

  return static_cast<int>((block_acc + discarded) / data->samplerate);
}

static int ape_decode(void *prv_data, char *buf, int buf_len,
                      struct sound_params *sound_params)
{
  auto *data = static_cast<struct ape_data *>(prv_data);
  decoder_error_clear(&data->error);

  ApeCtx *ctx = data->ctx.get();

  for (;;)
  {
    if (!data->frame_loaded)
    {
      if (data->currentframe >= data->totalframes) return 0; // EOF

      if (!ape_load_frame(data, data->currentframe))
      {
        decoder_error(&data->error, ERROR_STREAM, 0,
                      "ape: failed to load frame %u", data->currentframe);
        data->currentframe++;
        return 0;
      }
      data->currentframe++;
    }

    int bytes_per_sample_frame = (ctx->bps == 24 ? 4 : ctx->bps / 8) * ctx->channels;
    int frames_available = buf_len / bytes_per_sample_frame;
    if (frames_available <= 0) return 0;

    int decoded = ape_decode_blocks(data, frames_available);
    if (decoded == 0)
    {
      if (ctx->error)
      {
        decoder_error(&data->error, ERROR_STREAM, 0, "ape: decode error");
        data->frame_loaded = false;
        continue;
      }
      continue;
    }

    ape_pack_output(data, decoded, buf);

    sound_params->channels = ctx->channels;
    sound_params->rate = static_cast<int>(data->samplerate);
    sound_params->fmt = (ctx->bps == 8) ? SFMT_U8
                       : (ctx->bps == 16) ? (SFMT_S16 | SFMT_NE)
                                          : (SFMT_S32 | SFMT_NE);

    return decoded * bytes_per_sample_frame;
  }
}

static void ape_info(const char *file_name, struct file_tags *info, const int tags_sel)
{
  if (!(tags_sel & TAGS_TIME)) return;

  auto data = std::make_unique<struct ape_data>();
  decoder_error_init(&data->error);
  data->io_stream.reset(io_open(file_name, 1));

  if (!io_ok(data->io_stream.get())) return;
  if (!ape_read_header(data.get())) return;

  if (data->samplerate > 0)
  {
    info->time = static_cast<int>(data->total_blocks / data->samplerate);
    info->filled |= TAGS_TIME;
  }
}

static int ape_our_format_ext(const char *ext)
{
  return !strcasecmp(ext, "ape") || !strcasecmp(ext, "apl");
}

class ApeDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    ApeDecoder(void *d) : data(d, ape_close) {}
    ~ApeDecoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return ape_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return ape_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return ape_get_bitrate(data.get());
    }

    int get_duration() override {
        return ape_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        ape_get_error(data.get(), error);
    }
};

class ApePlugin : public AudioPlugin {
public:
    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = ape_open(file);
        if (!d) return nullptr;
        return std::make_unique<ApeDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        ape_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return ape_our_format_ext(ext);
    }

    std::string get_name(const char *) override {
        return "APE";
    }
};

extern "C" class AudioPlugin *ape_plugin_init() {
    static ApePlugin plugin;
    return &plugin;
}

// EOF
