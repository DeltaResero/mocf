// src/audio/decoders/wma/wmadec.cpp
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// mocf - Music on Console Framebuffer
//
// WMA v1/v2 decoder, ported from FFmpeg n4.4.4 (LGPLv2.1+). The upstream files
// this is assembled from and their copyright holders:
//
//   libavcodec/wma.c, wma.h        Copyright (c) 2002-2007 The FFmpeg Project
//   libavcodec/wmadec.c            Copyright (c) 2002 The FFmpeg Project
//   libavcodec/wma_common.c
//   libavcodec/wma_freqs.c
//   libavcodec/fft.h               Copyright (c) 2000, 2001, 2002 Fabrice Bellard
//                                  Copyright (c) 2002-2004 Michael Niedermayer
//   libavcodec/fft_template.c      Copyright (c) 2008 Loren Merritt
//                                  Copyright (c) 2002 Fabrice Bellard
//                                  Partly based on libdjbfft by D. J. Bernstein
//   libavcodec/mdct_template.c     Copyright (c) 2002 Fabrice Bellard
//   libavcodec/bitstream.c (VLC)   Copyright (c) 2000, 2001 Fabrice Bellard
//                                  Copyright (c) 2002-2004 Michael Niedermayer
//                                  Copyright (c) 2010 Loren Merritt
//   libavcodec/aactab.c (2 tables) Copyright (c) 2005-2006 Oded Shimon
//                                  Copyright (c) 2006-2007 Maxim Gavrilov
//   libavcodec/sinewin.h           Copyright (c) 2008 Robert Swain
//   libavcodec/sinewin_tablegen.h  Copyright (c) 2009 Reimar Doeffinger
//   libavutil/qsort.h (AV_QSORT)   Copyright (c) 2012 Michael Niedermayer
//   libavutil/float_dsp.c (3 fns)  Copyright 2005 Balatoni Denes
//                                  Copyright 2006 Loren Merritt
//
// The table data in wmadata.h is a separate verbatim file and carries its own
// upstream notice.
//
// The decode math is copied verbatim. Adaptations are confined to plumbing and
// are marked where they occur; the notable ones are:
//   - FFmpeg's AVCodecContext/AVFrame shrink to the handful of fields the
//     decoder actually reads, and output buffers are reserved once at init so
//     that nothing allocates on the decode path.
//   - The cosine/FFT and sine-window ladders stop at the largest size WMA v1/v2
//     can ask for instead of FFmpeg's 131072/8192, saving ~556KB of static data.
//     ff_fft_init() hard-fails above the cap rather than running off the table.
//   - SIMD arch dispatch and the fixed-point transform variants are dropped;
//     only the reference C path remains.
//   - bitswap_32() is reimplemented without FFmpeg's 256-byte ff_reverse[]
//     table, so libavcodec/mathops.h is not a dependency.
//   - The EOF drain is backported from FFmpeg master: n4.4.4 discards the
//     trailing MDCT overlap, truncating every file by frame_len samples.
//
// Verified byte-identical to FFmpeg's reference C decoder (ffmpeg -cpuflags 0)
// across wmav1/wmav2 x 8k/22.05k/44.1k/48k x mono/stereo, and on a real
// Windows Media Format SDK 7.01 encoded file.
//
// This program is free software; you can redistribute it and/or modify it under
// the terms of the GNU Lesser General Public License as published by the Free
// Software Foundation; either version 2.1 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public License for more
// details.

#include "wmadec.h"

#include <cerrno>
#include <cstdarg>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include "core/common.h"
#include "core/log.h"

namespace wma_ffmpeg {

// Plumbing shim: the minimum of FFmpeg's libavutil/libavcodec surface that the
// verbatim-copied WMA math depends on. Nothing here is decode logic.

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

#define av_cold
#define av_const
#define av_unused
#define av_restrict __restrict
#define av_always_inline inline __attribute__((always_inline))
#define av_warn_unused_result
#define av_fallthrough __attribute__((fallthrough))

#define AV_INPUT_BUFFER_PADDING_SIZE 64
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
#define FFSWAP(type, a, b) do { type SWAP_tmp = b; b = a; a = SWAP_tmp; } while (0)

#define DECLARE_ALIGNED(n, t, v) t __attribute__((aligned(n))) v

// Little-endian unaligned reads (WMA/ASF are little-endian throughout).
static inline unsigned AV_RL16(const void *p)
{
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static inline unsigned AV_RL32(const void *p)
{
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static inline int av_clip(int a, int amin, int amax)
{
  if (a < amin) return amin;
  if (a > amax) return amax;
  return a;
}

static inline float av_clipf(float a, float amin, float amax)
{
  if (a < amin) return amin;
  if (a > amax) return amax;
  return a;
}

static inline int16_t av_clip_int16(int a)
{
  if ((a + 0x8000U) & ~0xFFFF) return (a >> 31) ^ 0x7FFF;
  return static_cast<int16_t>(a);
}

static inline int av_log2(unsigned v)
{
  return v ? 31 - __builtin_clz(v) : 0;
}

// Allocation. The WMA context is a fixed ~190KB and the only heap users are the
// VLC/run/level tables, so plain malloc is sufficient; no arena tricks needed.
static inline void *av_malloc(size_t s) { return s ? malloc(s) : nullptr; }
static inline void *av_mallocz(size_t s)
{
  void *p = s ? malloc(s) : nullptr;
  if (p) memset(p, 0, s);
  return p;
}
static inline void *av_malloc_array(size_t n, size_t s)
{
  if (n && s > SIZE_MAX / n) return nullptr;
  return av_malloc(n * s);
}
static inline void *av_calloc(size_t n, size_t s)
{
  if (n && s > SIZE_MAX / n) return nullptr;
  return av_mallocz(n * s);
}
static inline void *av_realloc_f(void *p, size_t n, size_t s)
{
  if (n && s > SIZE_MAX / n) { free(p); return nullptr; }
  void *r = realloc(p, n * s);
  if (!r) free(p);
  return r;
}
static inline void av_free(void *p) { free(p); }
static inline void av_freep(void *p)
{
  void *v;
  memcpy(&v, p, sizeof(v));
  free(v);
  memset(p, 0, sizeof(v));
}

// Error codes: the port collapses FFmpeg's AVERROR space to plain negatives,
// since mocf only distinguishes success from failure here.
#define AVERROR(e)            (-(e))
#define AVERROR_INVALIDDATA   (-1094995529)
#define AVERROR_PATCHWELCOME  (-1163346256)
#define AVERROR_EOF           (-541478725)
#define ENOMEM_ERR            (-12)

#define AV_LOG_ERROR   16
#define AV_LOG_WARNING 24
#define AV_LOG_INFO    32
#define AV_LOG_DEBUG   48

/* Routed to mocf's logger; decode-path chatter would flood the log, so only
 * warnings and errors get through. */
void wma_log(int level, const char *fmt, ...);
/* The context argument only carries FFmpeg's logger identity, which mocf's
 * logger has no use for; it is still evaluated so that callers taking an
 * AVCodecContext purely to log through it do not look unused. */
#define av_log(ctx, level, ...) \
  do { (void)(ctx); wma_log(level, __VA_ARGS__); } while (0)
#define ff_dlog(ctx, ...)       do { } while (0)
#define ff_tlog(ctx, ...)       do { } while (0)
#define av_assert0(x)           do { } while (0)
#define av_assert1(x)           do { } while (0)
#define av_assert2(x)           do { } while (0)

typedef float FFTSample;

// MSB-first bitstream reader exposing the subset of FFmpeg's get_bits.h API
// that the WMA math calls. Semantics (bit order, over-read behaviour, the
// get_vlc2 multi-level walk) match get_bits.h exactly; only the internals are
// ours. FFmpeg's reader over-reads into a mandatory padding area, so reads past
// the end must yield zero bits rather than touch memory -- that is enforced here
// with an explicit bound instead of relying on caller-supplied padding.


// Widest field a single show_bits()/get_bits() call can return. FFmpeg varies
// this with its reader variant (25/32/64); this reader gathers 4 bytes, so with
// a bit offset of up to 7 the ceiling is 25. wma.c uses it to reject streams
// whose byte_offset_bits will not fit one read.
#define MIN_CACHE_BITS 25

typedef int16_t VLC_TYPE;

typedef struct GetBitContext
{
  const uint8_t *buffer;
  int buffer_size;   ///< bytes backing @ref buffer
  int index;         ///< current bit position
  int size_in_bits;
} GetBitContext;

static inline int init_get_bits(GetBitContext *s, const uint8_t *buffer,
                                int bit_size)
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

static inline int init_get_bits8(GetBitContext *s, const uint8_t *buffer,
                                 int byte_size)
{
  if (byte_size > INT_MAX / 8 || byte_size < 0) byte_size = -1;
  return init_get_bits(s, buffer, byte_size * 8);
}

static inline int get_bits_count(const GetBitContext *s) { return s->index; }
static inline int get_bits_left(const GetBitContext *s)
{
  return s->size_in_bits - s->index;
}

/// Peek @p n bits (1..25) without consuming. Zero-fills past the end.
static inline unsigned show_bits(const GetBitContext *s, int n)
{
  if (n <= 0 || n > 25) return 0;

  const int byte = s->index >> 3;
  const int off = s->index & 7;
  uint32_t cache = 0;

  // Gather 4 bytes big-endian; off + n <= 7 + 25 == 32, so this always holds
  // the requested field.
  for (int i = 0; i < 4; i++)
  {
    const int b = byte + i;
    cache = (cache << 8) |
            ((b >= 0 && b < s->buffer_size) ? s->buffer[b] : 0u);
  }
  return (cache << off) >> (32 - n);
}

static inline void skip_bits(GetBitContext *s, int n) { s->index += n; }
static inline void skip_bits_long(GetBitContext *s, int n) { s->index += n; }

static inline unsigned get_bits(GetBitContext *s, int n)
{
  const unsigned v = show_bits(s, n);
  s->index += n;
  return v;
}

static inline unsigned get_bits1(GetBitContext *s)
{
  const int idx = s->index;
  unsigned bit = 0;
  const int byte = idx >> 3;
  if (byte >= 0 && byte < s->buffer_size)
    bit = (s->buffer[byte] >> (7 - (idx & 7))) & 1;
  s->index = idx + 1;
  return bit;
}

/// Reads up to 32 bits; get_bits() alone tops out at 25.
static inline unsigned get_bits_long(GetBitContext *s, int n)
{
  if (n <= 0) return 0;
  if (n <= 25) return get_bits(s, n);
  const unsigned hi = get_bits(s, 16);
  const unsigned lo = get_bits(s, n - 16);
  return (hi << (n - 16)) | lo;
}

static inline void align_get_bits(GetBitContext *s)
{
  s->index = (s->index + 7) & ~7;
}

/**
 * Multi-level VLC lookup. Mirrors get_bits.h's GET_VLC macro: a negative length
 * in the table means "this entry is a subtable pointer, re-read -n bits".
 */
static inline int get_vlc2(GetBitContext *s, VLC_TYPE (*table)[2], int bits,
                           int max_depth)
{
  unsigned index = show_bits(s, bits);
  int code = table[index][0];
  int n = table[index][1];

  if (max_depth > 1 && n < 0)
  {
    skip_bits(s, bits);
    int nb_bits = -n;

    index = show_bits(s, nb_bits) + code;
    code = table[index][0];
    n = table[index][1];

    if (max_depth > 2 && n < 0)
    {
      skip_bits(s, nb_bits);
      nb_bits = -n;

      index = show_bits(s, nb_bits) + code;
      code = table[index][0];
      n = table[index][1];
    }
  }
  skip_bits(s, n);
  return code;
}

// VLC table construction, copied verbatim from FFmpeg n4.4.4 libavcodec/bitstream.c
// (LGPLv2.1+) with only plumbing adapted: FFmpeg's logging/alloc wrappers are
// replaced by the shims in wma_compat.h. The table-building logic is untouched.

// FFmpeg's AV_QSORT (libavutil/qsort.h, LGPLv2.1+), copied verbatim. Not
// substituted with std::sort: compare_vlcspec() compares (code >> 1), so codes
// differing only in their last bit tie, and the built table depends on how
// those ties get ordered.



/**
 * Quicksort
 * This sort is fast, and fully inplace but not stable and it is possible
 * to construct input that requires O(n^2) time but this is very unlikely to
 * happen with non constructed input.
 */
#define AV_QSORT(p, num, type, cmp) do {\
    void *stack[64][2];\
    int sp= 1;\
    stack[0][0] = p;\
    stack[0][1] = (p)+(num)-1;\
    while(sp){\
        type *start= static_cast<type *>(stack[--sp][0]);\
        type *end  = static_cast<type *>(stack[  sp][1]);\
        while(start < end){\
            if(start < end-1) {\
                int checksort=0;\
                type *right = end-2;\
                type *left  = start+1;\
                type *mid = start + ((end-start)>>1);\
                if(cmp(start, end) > 0) {\
                    if(cmp(  end, mid) > 0) FFSWAP(type, *start, *mid);\
                    else                    FFSWAP(type, *start, *end);\
                }else{\
                    if(cmp(start, mid) > 0) FFSWAP(type, *start, *mid);\
                    else checksort= 1;\
                }\
                if(cmp(mid, end) > 0){ \
                    FFSWAP(type, *mid, *end);\
                    checksort=0;\
                }\
                if(start == end-2) break;\
                FFSWAP(type, end[-1], *mid);\
                while(left <= right){\
                    while(left<=right && cmp(left, end-1) < 0)\
                        left++;\
                    while(left<=right && cmp(right, end-1) > 0)\
                        right--;\
                    if(left <= right){\
                        FFSWAP(type, *left, *right);\
                        left++;\
                        right--;\
                    }\
                }\
                FFSWAP(type, end[-1], *left);\
                if(checksort && (mid == left-1 || mid == left)){\
                    mid= start;\
                    while(mid<end && cmp(mid, mid+1) <= 0)\
                        mid++;\
                    if(mid==end)\
                        break;\
                }\
                if(end-left < left-start){\
                    stack[sp  ][0]= start;\
                    stack[sp++][1]= right;\
                    start = left+1;\
                }else{\
                    stack[sp  ][0]= left+1;\
                    stack[sp++][1]= end;\
                    end = right;\
                }\
            }else{\
                if(cmp(start, end) > 0)\
                    FFSWAP(type, *start, *end);\
                break;\
            }\
        }\
    }\
} while (0)

/**
 * Merge sort, this sort requires a temporary buffer and is stable, its worst
 * case time is O(n log n)
 * @param p     must be a lvalue pointer, this function may exchange it with tmp
 * @param tmp   must be a lvalue pointer, this function may exchange it with p
 */
#define AV_MSORT(p, tmp, num, type, cmp) do {\
    unsigned i, j, step;\
    for(step=1; step<(num); step+=step){\
        for(i=0; i<(num); i+=2*step){\
            unsigned a[2] = {i, i+step};\
            unsigned end = FFMIN(i+2*step, (num));\
            for(j=i; a[0]<i+step && a[1]<end; j++){\
                int idx= cmp(p+a[0], p+a[1]) > 0;\
                tmp[j] = p[ a[idx]++ ];\
            }\
            if(a[0]>=i+step) a[0] = a[1];\
            for(; j<end; j++){\
                tmp[j] = p[ a[0]++ ];\
            }\
        }\
        FFSWAP(type*, p, tmp);\
    }\
} while (0)


typedef struct VLC {
    int bits;
    VLC_TYPE (*table)[2]; ///< code, bits
    int table_size, table_allocated;
} VLC;

#define INIT_VLC_INPUT_LE        2
#define INIT_VLC_OUTPUT_LE       8
#define INIT_VLC_LE              (INIT_VLC_INPUT_LE | INIT_VLC_OUTPUT_LE)
#define INIT_VLC_USE_NEW_STATIC  4
#define INIT_VLC_STATIC_OVERLONG (1 | INIT_VLC_USE_NEW_STATIC)

// Equivalent to FFmpeg's table-driven bitswap_32() without its 256-byte
// ff_reverse[] lookup table. Only the INIT_VLC_OUTPUT_LE paths reach this and
// WMA never sets that flag, but it must still be correct.
static inline uint32_t bitswap_32(uint32_t x)
{
  x = ((x & 0x55555555u) << 1) | ((x >> 1) & 0x55555555u);
  x = ((x & 0x33333333u) << 2) | ((x >> 2) & 0x33333333u);
  x = ((x & 0x0F0F0F0Fu) << 4) | ((x >> 4) & 0x0F0F0F0Fu);
  return (x << 24) | ((x & 0xFF00u) << 8) | ((x >> 8) & 0xFF00u) | (x >> 24);
}

#define init_vlc(vlc, nb_bits, nb_codes,                \
                 bits, bits_wrap, bits_size,            \
                 codes, codes_wrap, codes_size,         \
                 flags)                                 \
    ff_init_vlc_sparse(vlc, nb_bits, nb_codes,          \
                       bits, bits_wrap, bits_size,      \
                       codes, codes_wrap, codes_size,   \
                       nullptr, 0, 0, flags)

/* VLC decoding */

#define GET_DATA(v, table, i, wrap, size)                   \
{                                                           \
    const uint8_t *ptr = static_cast<const uint8_t *>(table) + i * wrap; \
    switch(size) {                                          \
    case 1:                                                 \
        v = *ptr;                                           \
        break;                                              \
    case 2:                                                 \
        v = *reinterpret_cast<const uint16_t *>(ptr);       \
        break;                                              \
    case 4:                                                 \
    default:                                                \
        av_assert1(size == 4);                              \
        v = *reinterpret_cast<const uint32_t *>(ptr);       \
        break;                                              \
    }                                                       \
}


static int alloc_table(VLC *vlc, int size, int use_static)
{
    int index = vlc->table_size;

    vlc->table_size += size;
    if (vlc->table_size > vlc->table_allocated) {
        if (use_static)
            abort(); // cannot do anything, init_vlc() is used with too little memory
        vlc->table_allocated += (1 << vlc->bits);
        vlc->table = static_cast<VLC_TYPE (*)[2]>(av_realloc_f(vlc->table, vlc->table_allocated, sizeof(VLC_TYPE) * 2));
        if (!vlc->table) {
            vlc->table_allocated = 0;
            vlc->table_size = 0;
            return AVERROR(ENOMEM);
        }
        memset(vlc->table + vlc->table_allocated - (1 << vlc->bits), 0, sizeof(VLC_TYPE) * 2 << vlc->bits);
    }
    return index;
}

#define LOCALBUF_ELEMS 1500 // the maximum currently needed is 1296 by rv34

typedef struct VLCcode {
    uint8_t bits;
    VLC_TYPE symbol;
    /** codeword, with the first bit-to-be-read in the msb
     * (even if intended for a little-endian bitstream reader) */
    uint32_t code;
} VLCcode;

static int vlc_common_init(VLC *vlc_arg, int nb_bits, int nb_codes,
                           VLC **vlc, VLC *localvlc, VLCcode **buf,
                           int flags)
{
    *vlc = vlc_arg;
    (*vlc)->bits = nb_bits;
    if (flags & INIT_VLC_USE_NEW_STATIC) {
        av_assert0(nb_codes <= LOCALBUF_ELEMS);
        *localvlc = *vlc_arg;
        *vlc = localvlc;
        (*vlc)->table_size = 0;
    } else {
        (*vlc)->table           = nullptr;
        (*vlc)->table_allocated = 0;
        (*vlc)->table_size      = 0;
    }
    if (nb_codes > LOCALBUF_ELEMS) {
        *buf = static_cast<VLCcode *>(av_malloc_array(nb_codes, sizeof(VLCcode)));
        if (!*buf)
            return AVERROR(ENOMEM);
    }

    return 0;
}

static int compare_vlcspec(const void *a, const void *b)
{
    const VLCcode *sa = static_cast<const VLCcode *>(a), *sb = static_cast<const VLCcode *>(b);
    return (sa->code >> 1) - (sb->code >> 1);
}
/**
 * Build VLC decoding tables suitable for use with get_vlc().
 *
 * @param vlc            the context to be initialized
 *
 * @param table_nb_bits  max length of vlc codes to store directly in this table
 *                       (Longer codes are delegated to subtables.)
 *
 * @param nb_codes       number of elements in codes[]
 *
 * @param codes          descriptions of the vlc codes
 *                       These must be ordered such that codes going into the same subtable are contiguous.
 *                       Sorting by VLCcode.code is sufficient, though not necessary.
 */
static int build_table(VLC *vlc, int table_nb_bits, int nb_codes,
                       VLCcode *codes, int flags)
{
    int table_size, table_index, index, code_prefix, symbol, subtable_bits;
    int i, j, k, n, nb, inc;
    uint32_t code;
    volatile VLC_TYPE (* volatile table)[2]; // the double volatile is needed to prevent an internal compiler error in gcc 4.2

    if (table_nb_bits > 30)
       return AVERROR(EINVAL);
    table_size = 1 << table_nb_bits;
    table_index = alloc_table(vlc, table_size, flags & INIT_VLC_USE_NEW_STATIC);
    ff_dlog(nullptr, "new table index=%d size=%d\n", table_index, table_size);
    if (table_index < 0)
        return table_index;
    table = (volatile VLC_TYPE (*)[2])&vlc->table[table_index];

    /* first pass: map codes and compute auxiliary table sizes */
    for (i = 0; i < nb_codes; i++) {
        n      = codes[i].bits;
        code   = codes[i].code;
        symbol = codes[i].symbol;
        ff_dlog(nullptr, "i=%d n=%d code=0x%" PRIx32 "\n", i, n, code);
        if (n <= table_nb_bits) {
            /* no need to add another table */
            j = code >> (32 - table_nb_bits);
            nb = 1 << (table_nb_bits - n);
            inc = 1;
            if (flags & INIT_VLC_OUTPUT_LE) {
                j = bitswap_32(code);
                inc = 1 << n;
            }
            for (k = 0; k < nb; k++) {
                int bits = table[j][1];
                int oldsym  = table[j][0];
                ff_dlog(nullptr, "%4x: code=%d n=%d\n", j, i, n);
                if ((bits || oldsym) && (bits != n || oldsym != symbol)) {
                    av_log(nullptr, AV_LOG_ERROR, "incorrect codes\n");
                    return AVERROR_INVALIDDATA;
                }
                table[j][1] = n; //bits
                table[j][0] = symbol;
                j += inc;
            }
        } else {
            /* fill auxiliary table recursively */
            n -= table_nb_bits;
            code_prefix = code >> (32 - table_nb_bits);
            subtable_bits = n;
            codes[i].bits = n;
            codes[i].code = code << table_nb_bits;
            for (k = i+1; k < nb_codes; k++) {
                n = codes[k].bits - table_nb_bits;
                if (n <= 0)
                    break;
                code = codes[k].code;
                if (code >> (32 - table_nb_bits) !=
                    static_cast<uint32_t>(code_prefix))
                    break;
                codes[k].bits = n;
                codes[k].code = code << table_nb_bits;
                subtable_bits = FFMAX(subtable_bits, n);
            }
            subtable_bits = FFMIN(subtable_bits, table_nb_bits);
            j = (flags & INIT_VLC_OUTPUT_LE) ? bitswap_32(code_prefix) >> (32 - table_nb_bits) : code_prefix;
            table[j][1] = -subtable_bits;
            ff_dlog(nullptr, "%4x: n=%d (subtable)\n",
                    j, codes[i].bits + table_nb_bits);
            index = build_table(vlc, subtable_bits, k-i, codes+i, flags);
            if (index < 0)
                return index;
            /* note: realloc has been done, so reload tables */
            table = (volatile VLC_TYPE (*)[2])&vlc->table[table_index];
            table[j][0] = index; //code
            if (table[j][0] != index) {
                av_log(nullptr, AV_LOG_ERROR, "strange codes\n");
                return AVERROR_PATCHWELCOME;
            }
            i = k-1;
        }
    }

    for (i = 0; i < table_size; i++) {
        if (table[i][1] == 0) //bits
            table[i][0] = -1; //codes
    }

    return table_index;
}

static int vlc_common_end(VLC *vlc, int nb_bits, int nb_codes, VLCcode *codes,
                          int flags, VLC *vlc_arg, VLCcode localbuf[LOCALBUF_ELEMS])
{
    int ret = build_table(vlc, nb_bits, nb_codes, codes, flags);

    if (flags & INIT_VLC_USE_NEW_STATIC) {
        if (vlc->table_size != vlc->table_allocated &&
            !(flags & (INIT_VLC_STATIC_OVERLONG & ~INIT_VLC_USE_NEW_STATIC)))
            av_log(nullptr, AV_LOG_ERROR, "needed %d had %d\n", vlc->table_size, vlc->table_allocated);
        av_assert0(ret >= 0);
        *vlc_arg = *vlc;
    } else {
        if (codes != localbuf)
            av_free(codes);
        if (ret < 0) {
            av_freep(&vlc->table);
            return ret;
        }
    }
    return 0;
}

/* Build VLC decoding tables suitable for use with get_vlc().

   'nb_bits' sets the decoding table size (2^nb_bits) entries. The
   bigger it is, the faster is the decoding. But it should not be too
   big to save memory and L1 cache. '9' is a good compromise.

   'nb_codes' : number of vlcs codes

   'bits' : table which gives the size (in bits) of each vlc code.

   'codes' : table which gives the bit pattern of of each vlc code.

   'symbols' : table which gives the values to be returned from get_vlc().

   'xxx_wrap' : give the number of bytes between each entry of the
   'bits' or 'codes' tables.

   'xxx_size' : gives the number of bytes of each entry of the 'bits'
   or 'codes' tables. Currently 1,2 and 4 are supported.

   'wrap' and 'size' make it possible to use any memory configuration and types
   (byte/word/long) to store the 'bits', 'codes', and 'symbols' tables.
*/
static int ff_init_vlc_sparse(VLC *vlc_arg, int nb_bits, int nb_codes,
                       const void *bits, int bits_wrap, int bits_size,
                       const void *codes, int codes_wrap, int codes_size,
                       const void *symbols, int symbols_wrap, int symbols_size,
                       int flags)
{
    VLCcode localbuf[LOCALBUF_ELEMS], *buf = localbuf;
    int i, j, ret;
    VLC localvlc, *vlc;

    ret = vlc_common_init(vlc_arg, nb_bits, nb_codes, &vlc, &localvlc,
                          &buf, flags);
    if (ret < 0)
        return ret;

    av_assert0(symbols_size <= 2 || !symbols);
    j = 0;
#define COPY(condition)\
    for (i = 0; i < nb_codes; i++) {                                        \
        unsigned len;                                                       \
        GET_DATA(len, bits, i, bits_wrap, bits_size);                       \
        if (!(condition))                                                   \
            continue;                                                       \
        if (len > static_cast<unsigned>(3*nb_bits) || len > 32) {                                  \
            av_log(nullptr, AV_LOG_ERROR, "Too long VLC (%u) in init_vlc\n", len);\
            if (buf != localbuf)                                            \
                av_free(buf);                                               \
            return AVERROR(EINVAL);                                         \
        }                                                                   \
        buf[j].bits = len;                                                  \
        GET_DATA(buf[j].code, codes, i, codes_wrap, codes_size);            \
        if (buf[j].code >= (1LL<<buf[j].bits)) {                            \
            av_log(nullptr, AV_LOG_ERROR, "Invalid code %" PRIx32 " for %d in "  \
                   "init_vlc\n", buf[j].code, i);                           \
            if (buf != localbuf)                                            \
                av_free(buf);                                               \
            return AVERROR(EINVAL);                                         \
        }                                                                   \
        if (flags & INIT_VLC_INPUT_LE)                                      \
            buf[j].code = bitswap_32(buf[j].code);                          \
        else                                                                \
            buf[j].code <<= 32 - buf[j].bits;                               \
        if (symbols)                                                        \
            GET_DATA(buf[j].symbol, symbols, i, symbols_wrap, symbols_size) \
        else                                                                \
            buf[j].symbol = i;                                              \
        j++;                                                                \
    }
    COPY(len > static_cast<unsigned>(nb_bits));
    // qsort is the slowest part of init_vlc, and could probably be improved or avoided
    AV_QSORT(buf, j, struct VLCcode, compare_vlcspec);
    COPY(len && len <= static_cast<unsigned>(nb_bits));
    nb_codes = j;

    return vlc_common_end(vlc, nb_bits, nb_codes, buf,
                          flags, vlc_arg, localbuf);
}

static int ff_init_vlc_from_lengths(VLC *vlc_arg, int nb_bits, int nb_codes,
                             const int8_t *lens, int lens_wrap,
                             const void *symbols, int symbols_wrap, int symbols_size,
                             int offset, int flags, void *logctx)
{
    (void)logctx; /* FFmpeg logs through it; this port logs via mocf */
    VLCcode localbuf[LOCALBUF_ELEMS], *buf = localbuf;
    VLC localvlc, *vlc;
    uint64_t code;
    int ret, j, len_max = FFMIN(32, 3 * nb_bits);

    ret = vlc_common_init(vlc_arg, nb_bits, nb_codes, &vlc, &localvlc,
                          &buf, flags);
    if (ret < 0)
        return ret;

    j = code = 0;
    for (int i = 0; i < nb_codes; i++, lens += lens_wrap) {
        int len = *lens;
        if (len > 0) {
            unsigned sym;

            buf[j].bits = len;
            if (symbols)
                GET_DATA(sym, symbols, i, symbols_wrap, symbols_size)
            else
                sym = i;
            buf[j].symbol = sym + offset;
            buf[j++].code = code;
        } else if (len <  0) {
            len = -len;
        } else
            continue;
        if (len > len_max || code & ((1U << (32 - len)) - 1)) {
            av_log(logctx, AV_LOG_ERROR, "Invalid VLC (length %u)\n", len);
            goto fail;
        }
        code += 1U << (32 - len);
        if (code > UINT32_MAX + 1ULL) {
            av_log(logctx, AV_LOG_ERROR, "Overdetermined VLC tree\n");
            goto fail;
        }
    }
    return vlc_common_end(vlc, nb_bits, j, buf,
                          flags, vlc_arg, localbuf);
fail:
    if (buf != localbuf)
        av_free(buf);
    return AVERROR_INVALIDDATA;
}

static void ff_free_vlc(VLC *vlc)
{
    av_freep(&vlc->table);
}

// FFT/MDCT support, derived from FFmpeg n4.4.4 libavcodec/fft.h (LGPLv2.1+).
//
// Two deliberate departures from the original, both plumbing rather than math:
//
//  1. The arch dispatch (ff_fft_init_x86/arm/ppc/aarch64/mips) is dropped; only
//     the reference C path is kept. Those hooks exist purely to swap in SIMD
//     kernels and nothing in the transform itself depends on them.
//
//  2. FFmpeg declares cosine tables for every FFT size up to 131072, which costs
//     roughly 512KB of .bss. WMA v1/v2 caps frame_len_bits at 11, so the largest
//     MDCT is 1<<12 and ff_mdct_init() asks ff_fft_init() for nbits 10 -- a 1024
//     table. Tables stop at 4096 here (~16KB), leaving two levels of headroom,
//     and ff_fft_init() hard-fails above that rather than indexing off the end.

#define FFT_FLOAT 1
#define CONFIG_MDCT 1

// Largest FFT this build supports: log2 of the table cap above.
#define WMA_FFT_MAX_NBITS 12

typedef struct FFTComplex {
  FFTSample re, im;
} FFTComplex;

typedef float FFTDouble;

typedef struct FFTDComplex {
  FFTDouble re, im;
} FFTDComplex;

enum fft_permutation_type {
  FF_FFT_PERM_DEFAULT,
  FF_FFT_PERM_SWAP_LSBS,
  FF_FFT_PERM_AVX,
};

enum mdct_permutation_type {
  FF_MDCT_PERM_NONE,
  FF_MDCT_PERM_INTERLEAVE,
};

typedef struct FFTContext {
  int nbits;
  int inverse;
  uint16_t *revtab;
  FFTComplex *tmp_buf;
  int mdct_size;
  int mdct_bits;
  FFTSample *tcos;
  FFTSample *tsin;
  void (*fft_permute)(struct FFTContext *s, FFTComplex *z);
  void (*fft_calc)(struct FFTContext *s, FFTComplex *z);
  void (*imdct_calc)(struct FFTContext *s, FFTSample *output,
                     const FFTSample *input);
  void (*imdct_half)(struct FFTContext *s, FFTSample *output,
                     const FFTSample *input);
  void (*mdct_calc)(struct FFTContext *s, FFTSample *output,
                    const FFTSample *input);
  enum fft_permutation_type fft_permutation;
  enum mdct_permutation_type mdct_permutation;
  uint32_t *revtab32;
} FFTContext;

#define COSTABLE(size) DECLARE_ALIGNED(32, FFTSample, ff_cos_##size)[size / 2]

extern COSTABLE(16);
extern COSTABLE(32);
extern COSTABLE(64);
extern COSTABLE(128);
extern COSTABLE(256);
extern COSTABLE(512);
extern COSTABLE(1024);
extern COSTABLE(2048);
extern COSTABLE(4096);
extern FFTSample *const ff_cos_tabs[WMA_FFT_MAX_NBITS + 1];

void ff_init_ff_cos_tabs(int index);

int ff_fft_init(FFTContext *s, int nbits, int inverse);
void ff_fft_end(FFTContext *s);

int ff_mdct_init(FFTContext *s, int nbits, int inverse, double scale);
void ff_mdct_end(FFTContext *s);
void ff_imdct_calc_c(FFTContext *s, FFTSample *output, const FFTSample *input);
void ff_imdct_half_c(FFTContext *s, FFTSample *output, const FFTSample *input);
void ff_mdct_calc_c(FFTContext *s, FFTSample *output, const FFTSample *input);

#define ff_mdct_calc(s, o, i)  ((s)->mdct_calc(s, o, i))
#define ff_imdct_calc(s, o, i) ((s)->imdct_calc(s, o, i))
#define ff_imdct_half(s, o, i) ((s)->imdct_half(s, o, i))

// WMA shared context, from FFmpeg n4.4.4 libavcodec/wma.h (LGPLv2.1+).
// Named wma.h so the verbatim wmadata.h/wma.c can include it unchanged.
// Adaptations are plumbing only: FFmpeg's AVCodecContext is replaced by the
// small shim below (the decoder touches just ten of its fields), the
// encoder-only PutBitContext is dropped, and AVFloatDSPContext shrinks to the
// three routines wmadec actually calls.

#define AV_CODEC_ID_WMAV1 1
#define AV_CODEC_ID_WMAV2 2

#define AV_SAMPLE_FMT_FLTP 8
#define AV_CODEC_FLAG_BITEXACT (1 << 23)

typedef struct AVCodecShim {
  int id;
} AVCodecShim;

// Stand-in for AVCodecContext: exactly the fields wma.c/wmadec.c read.
typedef struct AVCodecContext {
  const AVCodecShim *codec;
  int channels;
  int sample_rate;
  int block_align;
  int64_t bit_rate;
  uint8_t *extradata;
  int extradata_size;
  int flags;
  int sample_fmt;
  void *priv_data;
} AVCodecContext;

/* The only three AVFloatDSPContext routines wmadec uses; FFmpeg's versions are
 * SIMD-dispatched, these are its plain C reference implementations. */
typedef struct AVFloatDSPContext {
  void (*vector_fmul_add)(float *dst, const float *src0, const float *src1,
                          const float *src2, int len);
  void (*vector_fmul_reverse)(float *dst, const float *src0, const float *src1,
                              int len);
  void (*butterflies_float)(float *av_restrict v1, float *av_restrict v2,
                            int len);
} AVFloatDSPContext;

AVFloatDSPContext *avpriv_float_dsp_alloc(int bitexact);

/* Stand-ins for AVPacket/AVFrame: the decoder only ever reads these fields. */
typedef struct AVPacket {
  const uint8_t *data;
  int size;
} AVPacket;

typedef struct AVFrame {
  int nb_samples;
  float *extended_data[2];
} AVFrame;

/* Sine windows. FFmpeg declares these to 8192; WMA indexes ff_sine_windows[]
 * with frame_len_bits - i, which for v1/v2 spans 7..11, so the ladder stops at
 * 2048 (saving ~48KB) and ff_init_ff_sine_windows() refuses anything higher. */
#define SINETABLE(size) DECLARE_ALIGNED(32, float, ff_sine_##size)[size]
#define WMA_SINE_MAX_INDEX 11

extern SINETABLE(32);
extern SINETABLE(64);
extern SINETABLE(128);
extern SINETABLE(256);
extern SINETABLE(512);
extern SINETABLE(1024);
extern SINETABLE(2048);
extern float *const ff_sine_windows[WMA_SINE_MAX_INDEX + 1];

void ff_sine_window_init(float *window, int n);
void ff_init_ff_sine_windows(int index);

int ff_wma_get_frame_len_bits(int sample_rate, int version,
                              unsigned int decode_flags);

/* libavutil/ffmath.h */
static av_always_inline double ff_exp10(double x)
{
  return exp2(3.32192809488736234787 * x); /* M_LOG2_10 */
}




/* size of blocks */
#define BLOCK_MIN_BITS 7
#define BLOCK_MAX_BITS 11
#define BLOCK_MAX_SIZE (1 << BLOCK_MAX_BITS)

#define BLOCK_NB_SIZES (BLOCK_MAX_BITS - BLOCK_MIN_BITS + 1)

/* XXX: find exact max size */
#define HIGH_BAND_MAX_SIZE 16

#define NB_LSP_COEFS 10

/* XXX: is it a suitable value ? */
#define MAX_CODED_SUPERFRAME_SIZE 32768

#define MAX_CHANNELS 2

#define NOISE_TAB_SIZE 8192

#define LSP_POW_BITS 7

// FIXME should be in wmadec
#define VLCBITS 9
#define VLCMAX ((22 + VLCBITS - 1) / VLCBITS)

typedef float WMACoef;          ///< type for decoded coefficients, int16_t would be enough for wma 1/2

typedef struct CoefVLCTable {
    int n;                      ///< total number of codes
    int max_level;
    const uint32_t *huffcodes;  ///< VLC bit values
    const uint8_t *huffbits;    ///< VLC bit size
    const uint16_t *levels;     ///< table to build run/level tables
} CoefVLCTable;

typedef struct WMACodecContext {
    AVCodecContext *avctx;
    GetBitContext gb;
    int version;                            ///< 1 = 0x160 (WMAV1), 2 = 0x161 (WMAV2)
    int use_bit_reservoir;
    int use_variable_block_len;
    int use_exp_vlc;                        ///< exponent coding: 0 = lsp, 1 = vlc + delta
    int use_noise_coding;                   ///< true if perceptual noise is added
    int byte_offset_bits;
    VLC exp_vlc;
    int exponent_sizes[BLOCK_NB_SIZES];
    uint16_t exponent_bands[BLOCK_NB_SIZES][25];
    int high_band_start[BLOCK_NB_SIZES];    ///< index of first coef in high band
    int coefs_start;                        ///< first coded coef
    int coefs_end[BLOCK_NB_SIZES];          ///< max number of coded coefficients
    int exponent_high_sizes[BLOCK_NB_SIZES];
    int exponent_high_bands[BLOCK_NB_SIZES][HIGH_BAND_MAX_SIZE];
    VLC hgain_vlc;

    /* coded values in high bands */
    int high_band_coded[MAX_CHANNELS][HIGH_BAND_MAX_SIZE];
    int high_band_values[MAX_CHANNELS][HIGH_BAND_MAX_SIZE];

    /* there are two possible tables for spectral coefficients */
// FIXME the following 3 tables should be shared between decoders
    VLC coef_vlc[2];
    uint16_t *run_table[2];
    float *level_table[2];
    uint16_t *int_table[2];
    const CoefVLCTable *coef_vlcs[2];
    /* frame info */
    int frame_len;                          ///< frame length in samples
    int frame_len_bits;                     ///< frame_len = 1 << frame_len_bits
    int nb_block_sizes;                     ///< number of block sizes
    /* block info */
    int reset_block_lengths;
    int block_len_bits;                     ///< log2 of current block length
    int next_block_len_bits;                ///< log2 of next block length
    int prev_block_len_bits;                ///< log2 of prev block length
    int block_len;                          ///< block length in samples
    int block_num;                          ///< block number in current frame
    int block_pos;                          ///< current position in frame
    uint8_t ms_stereo;                      ///< true if mid/side stereo mode
    uint8_t channel_coded[MAX_CHANNELS];    ///< true if channel is coded
    int exponents_bsize[MAX_CHANNELS];      ///< log2 ratio frame/exp. length
    DECLARE_ALIGNED(32, float, exponents)[MAX_CHANNELS][BLOCK_MAX_SIZE];
    float max_exponent[MAX_CHANNELS];
    WMACoef coefs1[MAX_CHANNELS][BLOCK_MAX_SIZE];
    DECLARE_ALIGNED(32, float, coefs)[MAX_CHANNELS][BLOCK_MAX_SIZE];
    DECLARE_ALIGNED(32, FFTSample, output)[BLOCK_MAX_SIZE * 2];
    FFTContext mdct_ctx[BLOCK_NB_SIZES];
    const float *windows[BLOCK_NB_SIZES];
    /* output buffer for one frame and the last for IMDCT windowing */
    DECLARE_ALIGNED(32, float, frame_out)[MAX_CHANNELS][BLOCK_MAX_SIZE * 2];
    /* last frame info */
    uint8_t last_superframe[MAX_CODED_SUPERFRAME_SIZE + AV_INPUT_BUFFER_PADDING_SIZE]; /* padding added */
    int last_bitoffset;
    int last_superframe_len;
    int eof_done; /* decode flag to output remaining samples after EOF */
    int exponents_initialized[MAX_CHANNELS];
    float noise_table[NOISE_TAB_SIZE];
    int noise_index;
    float noise_mult; /* XXX: suppress that and integrate it in the noise array */
    /* lsp_to_curve tables */
    float lsp_cos_table[BLOCK_MAX_SIZE];
    float lsp_pow_e_table[256];
    float lsp_pow_m_table1[(1 << LSP_POW_BITS)];
    float lsp_pow_m_table2[(1 << LSP_POW_BITS)];
    AVFloatDSPContext *fdsp;
    /* Output buffer, reserved once at init: MAX_CHANNELS planes of
     * WMA_MAX_FRAMES_PER_SUPERFRAME * frame_len samples. */
    float *out_buf;
    int out_samples_max;

#ifdef TRACE
    int frame_count;
#endif /* TRACE */
} WMACodecContext;

extern const uint8_t ff_wma_hgain_hufftab[37][2];
extern const float ff_wma_lsp_codebook[NB_LSP_COEFS][16];
extern const uint32_t ff_aac_scalefactor_code[121];
extern const uint8_t  ff_aac_scalefactor_bits[121];

av_warn_unused_result
int ff_wma_init(AVCodecContext *avctx, int flags2);

int ff_wma_total_gain_to_bits(int total_gain);
int ff_wma_end(AVCodecContext *avctx);
unsigned int ff_wma_get_large_val(GetBitContext *gb);
int ff_wma_run_level_decode(AVCodecContext *avctx, GetBitContext *gb,
                            VLC *vlc, const float *level_table,
                            const uint16_t *run_table, int version,
                            WMACoef *ptr, int offset, int num_coefs,
                            int block_len, int frame_len_bits,
                            int coef_nb_bits);


int wma_dec_init(AVCodecContext *avctx);
int wma_dec_decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame,
                   const uint8_t *buf, int size);
void wma_dec_flush(AVCodecContext *avctx);
int wma_dec_close(AVCodecContext *avctx);

// FFT and MDCT, copied from FFmpeg n4.4.4 libavcodec/fft_template.c and
// mdct_template.c (LGPLv2.1+). The transform math is untouched; adaptations are
// confined to plumbing: the fixed-point variants and SIMD arch dispatch are
// dropped, the cosine/FFT size ladder is capped at 4096 (see wma_fft.h), and
// FFmpeg's per-table AVOnce guards are expressed with std::call_once.

#define FFT_FIXED_32 0
#define CONFIG_HARDCODED_TABLES 0
#define CONFIG_SMALL 0
// The AVX permutation is an x86-only layout; the reference C path never uses it.
#define ARCH_X86 0

#define sqrthalf (float)M_SQRT1_2
#define FIX15(v) (v)
#define RSCALE(x, y) ((x) + (y))

#define BF(x, y, a, b) do {                     \
        x = a - b;                              \
        y = a + b;                              \
    } while (0)

#define CMUL(dre, dim, are, aim, bre, bim) do { \
        (dre) = (are) * (bre) - (aim) * (bim);  \
        (dim) = (are) * (bim) + (aim) * (bre);  \
    } while (0)

#define FFT_NAME(x) x
#define COSTABLE_CONST



/**
 * @file
 * FFT/IFFT transforms.
 */


/* cos(2*pi*x/n) for 0<=x<=n/4, followed by its reverse.
 * FFmpeg declares this ladder up to 131072 (~512KB of .bss). WMA v1/v2 caps
 * frame_len_bits at 11, so the largest MDCT is 1<<12 and ff_fft_init() is asked
 * for nbits 10 -- a 1024-entry table. Stopping at 4096 keeps two levels of
 * headroom for ~16KB. */
COSTABLE(16);
COSTABLE(32);
COSTABLE(64);
COSTABLE(128);
COSTABLE(256);
COSTABLE(512);
COSTABLE(1024);
COSTABLE(2048);
COSTABLE(4096);

static av_cold void init_ff_cos_tabs(int index)
{
    int i;
    int m = 1<<index;
    double freq = 2*M_PI/m;
    FFTSample *tab = ff_cos_tabs[index];
    for(i=0; i<=m/4; i++)
        tab[i] = cos(i*freq);
    for(i=1; i<m/4; i++)
        tab[m/2-i] = tab[i];
}

/* FFmpeg guards each table with its own AVOnce; std::once_flag is the direct
 * equivalent and keeps the tables safe to share across decoder instances. */
static std::once_flag cos_tabs_once[WMA_FFT_MAX_NBITS + 1];

void ff_init_ff_cos_tabs(int index)
{
    if (index < 4 || index > WMA_FFT_MAX_NBITS)
        return;
    std::call_once(cos_tabs_once[index], init_ff_cos_tabs, index);
}

FFTSample * const ff_cos_tabs[WMA_FFT_MAX_NBITS + 1] = {
    nullptr, nullptr, nullptr, nullptr,
    ff_cos_16, ff_cos_32, ff_cos_64, ff_cos_128, ff_cos_256,
    ff_cos_512, ff_cos_1024, ff_cos_2048, ff_cos_4096,
};

static void fft_permute_c(FFTContext *s, FFTComplex *z);
static void fft_calc_c(FFTContext *s, FFTComplex *z);

static int split_radix_permutation(int i, int n, int inverse)
{
    int m;
    if(n <= 2) return i&1;
    m = n >> 1;
    if(!(i&m))            return split_radix_permutation(i, m, inverse)*2;
    m >>= 1;
    if(inverse == !(i&m)) return split_radix_permutation(i, m, inverse)*4 + 1;
    else                  return split_radix_permutation(i, m, inverse)*4 - 1;
}


static const int avx_tab[] = {
    0, 4, 1, 5, 8, 12, 9, 13, 2, 6, 3, 7, 10, 14, 11, 15
};

static int is_second_half_of_fft32(int i, int n)
{
    if (n <= 32)
        return i >= 16;
    else if (i < n/2)
        return is_second_half_of_fft32(i, n/2);
    else if (i < 3*n/4)
        return is_second_half_of_fft32(i - n/2, n/4);
    else
        return is_second_half_of_fft32(i - 3*n/4, n/4);
}

static av_cold void fft_perm_avx(FFTContext *s)
{
    int i;
    int n = 1 << s->nbits;

    for (i = 0; i < n; i += 16) {
        int k;
        if (is_second_half_of_fft32(i, n)) {
            for (k = 0; k < 16; k++)
                s->revtab[-split_radix_permutation(i + k, n, s->inverse) & (n - 1)] =
                    i + avx_tab[k];

        } else {
            for (k = 0; k < 16; k++) {
                int j = i + k;
                j = (j & ~7) | ((j >> 1) & 3) | ((j << 2) & 4);
                s->revtab[-split_radix_permutation(i + k, n, s->inverse) & (n - 1)] = j;
            }
        }
    }
}

av_cold int ff_fft_init(FFTContext *s, int nbits, int inverse)
{
    int i, j, n;

    s->revtab = NULL;
    s->revtab32 = NULL;

    /* Upstream allows up to 17. The cosine/FFT ladder is capped at 4096 here
     * (see wma_fft.h), so anything above WMA_FFT_MAX_NBITS would index past the
     * end of fft_dispatch[] and ff_cos_tabs[]. WMA v1/v2 never asks for more
     * than 10. */
    if (nbits < 2 || nbits > WMA_FFT_MAX_NBITS)
        goto fail;
    s->nbits = nbits;
    n = 1 << nbits;

    if (nbits <= 16) {
        s->revtab = static_cast<uint16_t *>(av_malloc(n * sizeof(uint16_t)));
        if (!s->revtab)
            goto fail;
    } else {
        s->revtab32 = static_cast<uint32_t *>(av_malloc(n * sizeof(uint32_t)));
        if (!s->revtab32)
            goto fail;
    }
    s->tmp_buf = static_cast<FFTComplex *>(av_malloc(n * sizeof(FFTComplex)));
    if (!s->tmp_buf)
        goto fail;
    s->inverse = inverse;
    s->fft_permutation = FF_FFT_PERM_DEFAULT;

    s->fft_permute = fft_permute_c;
    s->fft_calc    = fft_calc_c;
#if CONFIG_MDCT
    s->imdct_calc  = ff_imdct_calc_c;
    s->imdct_half  = ff_imdct_half_c;
    s->mdct_calc   = ff_mdct_calc_c;
#endif

#if FFT_FIXED_32
    ff_fft_lut_init();
#else /* FFT_FIXED_32 */
#if FFT_FLOAT
#endif
    for(j=4; j<=nbits; j++) {
        ff_init_ff_cos_tabs(j);
    }
#endif /* FFT_FIXED_32 */


    if (ARCH_X86 && FFT_FLOAT && s->fft_permutation == FF_FFT_PERM_AVX) {
        fft_perm_avx(s);
    } else {
#define PROCESS_FFT_PERM_SWAP_LSBS(num) do {\
    for(i = 0; i < n; i++) {\
        int k;\
        j = i;\
        j = (j & ~3) | ((j >> 1) & 1) | ((j << 1) & 2);\
        k = -split_radix_permutation(i, n, s->inverse) & (n - 1);\
        s->revtab##num[k] = j;\
    } \
} while(0);

#define PROCESS_FFT_PERM_DEFAULT(num) do {\
    for(i = 0; i < n; i++) {\
        int k;\
        j = i;\
        k = -split_radix_permutation(i, n, s->inverse) & (n - 1);\
        s->revtab##num[k] = j;\
    } \
} while(0);

#define SPLIT_RADIX_PERMUTATION(num) do { \
    if (s->fft_permutation == FF_FFT_PERM_SWAP_LSBS) {\
        PROCESS_FFT_PERM_SWAP_LSBS(num) \
    } else {\
        PROCESS_FFT_PERM_DEFAULT(num) \
    }\
} while(0);

    if (s->revtab)
        SPLIT_RADIX_PERMUTATION()
    if (s->revtab32)
        SPLIT_RADIX_PERMUTATION(32)

#undef PROCESS_FFT_PERM_DEFAULT
#undef PROCESS_FFT_PERM_SWAP_LSBS
#undef SPLIT_RADIX_PERMUTATION
    }

    return 0;
 fail:
    av_freep(&s->revtab);
    av_freep(&s->revtab32);
    av_freep(&s->tmp_buf);
    return -1;
}

static void fft_permute_c(FFTContext *s, FFTComplex *z)
{
    int j, np;
    const uint16_t *revtab = s->revtab;
    const uint32_t *revtab32 = s->revtab32;
    np = 1 << s->nbits;
    /* TODO: handle split-radix permute in a more optimal way, probably in-place */
    if (revtab) {
        for(j=0;j<np;j++) s->tmp_buf[revtab[j]] = z[j];
    } else
        for(j=0;j<np;j++) s->tmp_buf[revtab32[j]] = z[j];

    memcpy(z, s->tmp_buf, np * sizeof(FFTComplex));
}

av_cold void ff_fft_end(FFTContext *s)
{
    av_freep(&s->revtab);
    av_freep(&s->revtab32);
    av_freep(&s->tmp_buf);
}

#if FFT_FIXED_32

static void fft_calc_c(FFTContext *s, FFTComplex *z) {

    int nbits, i, n, num_transforms, offset, step;
    int n4, n2, n34;
    unsigned tmp1, tmp2, tmp3, tmp4, tmp5, tmp6, tmp7, tmp8;
    FFTComplex *tmpz;
    const int fft_size = (1 << s->nbits);
    int64_t accu;

    num_transforms = (0x2aab >> (16 - s->nbits)) | 1;

    for (n=0; n<num_transforms; n++){
        offset = ff_fft_offsets_lut[n] << 2;
        tmpz = z + offset;

        tmp1 = tmpz[0].re + (unsigned)tmpz[1].re;
        tmp5 = tmpz[2].re + (unsigned)tmpz[3].re;
        tmp2 = tmpz[0].im + (unsigned)tmpz[1].im;
        tmp6 = tmpz[2].im + (unsigned)tmpz[3].im;
        tmp3 = tmpz[0].re - (unsigned)tmpz[1].re;
        tmp8 = tmpz[2].im - (unsigned)tmpz[3].im;
        tmp4 = tmpz[0].im - (unsigned)tmpz[1].im;
        tmp7 = tmpz[2].re - (unsigned)tmpz[3].re;

        tmpz[0].re = tmp1 + tmp5;
        tmpz[2].re = tmp1 - tmp5;
        tmpz[0].im = tmp2 + tmp6;
        tmpz[2].im = tmp2 - tmp6;
        tmpz[1].re = tmp3 + tmp8;
        tmpz[3].re = tmp3 - tmp8;
        tmpz[1].im = tmp4 - tmp7;
        tmpz[3].im = tmp4 + tmp7;
    }

    if (fft_size < 8)
        return;

    num_transforms = (num_transforms >> 1) | 1;

    for (n=0; n<num_transforms; n++){
        offset = ff_fft_offsets_lut[n] << 3;
        tmpz = z + offset;

        tmp1 = tmpz[4].re + (unsigned)tmpz[5].re;
        tmp3 = tmpz[6].re + (unsigned)tmpz[7].re;
        tmp2 = tmpz[4].im + (unsigned)tmpz[5].im;
        tmp4 = tmpz[6].im + (unsigned)tmpz[7].im;
        tmp5 = tmp1 + tmp3;
        tmp7 = tmp1 - tmp3;
        tmp6 = tmp2 + tmp4;
        tmp8 = tmp2 - tmp4;

        tmp1 = tmpz[4].re - (unsigned)tmpz[5].re;
        tmp2 = tmpz[4].im - (unsigned)tmpz[5].im;
        tmp3 = tmpz[6].re - (unsigned)tmpz[7].re;
        tmp4 = tmpz[6].im - (unsigned)tmpz[7].im;

        tmpz[4].re = tmpz[0].re - tmp5;
        tmpz[0].re = tmpz[0].re + tmp5;
        tmpz[4].im = tmpz[0].im - tmp6;
        tmpz[0].im = tmpz[0].im + tmp6;
        tmpz[6].re = tmpz[2].re - tmp8;
        tmpz[2].re = tmpz[2].re + tmp8;
        tmpz[6].im = tmpz[2].im + tmp7;
        tmpz[2].im = tmpz[2].im - tmp7;

        accu = (int64_t)Q31(M_SQRT1_2)*(int)(tmp1 + tmp2);
        tmp5 = (int32_t)((accu + 0x40000000) >> 31);
        accu = (int64_t)Q31(M_SQRT1_2)*(int)(tmp3 - tmp4);
        tmp7 = (int32_t)((accu + 0x40000000) >> 31);
        accu = (int64_t)Q31(M_SQRT1_2)*(int)(tmp2 - tmp1);
        tmp6 = (int32_t)((accu + 0x40000000) >> 31);
        accu = (int64_t)Q31(M_SQRT1_2)*(int)(tmp3 + tmp4);
        tmp8 = (int32_t)((accu + 0x40000000) >> 31);
        tmp1 = tmp5 + tmp7;
        tmp3 = tmp5 - tmp7;
        tmp2 = tmp6 + tmp8;
        tmp4 = tmp6 - tmp8;

        tmpz[5].re = tmpz[1].re - tmp1;
        tmpz[1].re = tmpz[1].re + tmp1;
        tmpz[5].im = tmpz[1].im - tmp2;
        tmpz[1].im = tmpz[1].im + tmp2;
        tmpz[7].re = tmpz[3].re - tmp4;
        tmpz[3].re = tmpz[3].re + tmp4;
        tmpz[7].im = tmpz[3].im + tmp3;
        tmpz[3].im = tmpz[3].im - tmp3;
    }

    step = 1 << ((MAX_LOG2_NFFT-4) - 4);
    n4 = 4;

    for (nbits=4; nbits<=s->nbits; nbits++){
        n2  = 2*n4;
        n34 = 3*n4;
        num_transforms = (num_transforms >> 1) | 1;

        for (n=0; n<num_transforms; n++){
            const FFTSample *w_re_ptr = ff_w_tab_sr + step;
            const FFTSample *w_im_ptr = ff_w_tab_sr + MAX_FFT_SIZE/(4*16) - step;
            offset = ff_fft_offsets_lut[n] << nbits;
            tmpz = z + offset;

            tmp5 = tmpz[ n2].re + (unsigned)tmpz[n34].re;
            tmp1 = tmpz[ n2].re - (unsigned)tmpz[n34].re;
            tmp6 = tmpz[ n2].im + (unsigned)tmpz[n34].im;
            tmp2 = tmpz[ n2].im - (unsigned)tmpz[n34].im;

            tmpz[ n2].re = tmpz[ 0].re - tmp5;
            tmpz[  0].re = tmpz[ 0].re + tmp5;
            tmpz[ n2].im = tmpz[ 0].im - tmp6;
            tmpz[  0].im = tmpz[ 0].im + tmp6;
            tmpz[n34].re = tmpz[n4].re - tmp2;
            tmpz[ n4].re = tmpz[n4].re + tmp2;
            tmpz[n34].im = tmpz[n4].im + tmp1;
            tmpz[ n4].im = tmpz[n4].im - tmp1;

            for (i=1; i<n4; i++){
                FFTSample w_re = w_re_ptr[0];
                FFTSample w_im = w_im_ptr[0];
                accu  = (int64_t)w_re*tmpz[ n2+i].re;
                accu += (int64_t)w_im*tmpz[ n2+i].im;
                tmp1 = (int32_t)((accu + 0x40000000) >> 31);
                accu  = (int64_t)w_re*tmpz[ n2+i].im;
                accu -= (int64_t)w_im*tmpz[ n2+i].re;
                tmp2 = (int32_t)((accu + 0x40000000) >> 31);
                accu  = (int64_t)w_re*tmpz[n34+i].re;
                accu -= (int64_t)w_im*tmpz[n34+i].im;
                tmp3 = (int32_t)((accu + 0x40000000) >> 31);
                accu  = (int64_t)w_re*tmpz[n34+i].im;
                accu += (int64_t)w_im*tmpz[n34+i].re;
                tmp4 = (int32_t)((accu + 0x40000000) >> 31);

                tmp5 = tmp1 + tmp3;
                tmp1 = tmp1 - tmp3;
                tmp6 = tmp2 + tmp4;
                tmp2 = tmp2 - tmp4;

                tmpz[ n2+i].re = tmpz[   i].re - tmp5;
                tmpz[    i].re = tmpz[   i].re + tmp5;
                tmpz[ n2+i].im = tmpz[   i].im - tmp6;
                tmpz[    i].im = tmpz[   i].im + tmp6;
                tmpz[n34+i].re = tmpz[n4+i].re - tmp2;
                tmpz[ n4+i].re = tmpz[n4+i].re + tmp2;
                tmpz[n34+i].im = tmpz[n4+i].im + tmp1;
                tmpz[ n4+i].im = tmpz[n4+i].im - tmp1;

                w_re_ptr += step;
                w_im_ptr -= step;
            }
        }
        step >>= 1;
        n4   <<= 1;
    }
}

#else /* FFT_FIXED_32 */

#define BUTTERFLIES(a0,a1,a2,a3) {\
    BF(t3, t5, t5, t1);\
    BF(a2.re, a0.re, a0.re, t5);\
    BF(a3.im, a1.im, a1.im, t3);\
    BF(t4, t6, t2, t6);\
    BF(a3.re, a1.re, a1.re, t4);\
    BF(a2.im, a0.im, a0.im, t6);\
}

// force loading all the inputs before storing any.
// this is slightly slower for small data, but avoids store->load aliasing
// for addresses separated by large powers of 2.
#define BUTTERFLIES_BIG(a0,a1,a2,a3) {\
    FFTSample r0=a0.re, i0=a0.im, r1=a1.re, i1=a1.im;\
    BF(t3, t5, t5, t1);\
    BF(a2.re, a0.re, r0, t5);\
    BF(a3.im, a1.im, i1, t3);\
    BF(t4, t6, t2, t6);\
    BF(a3.re, a1.re, r1, t4);\
    BF(a2.im, a0.im, i0, t6);\
}

#define TRANSFORM(a0,a1,a2,a3,wre,wim) {\
    CMUL(t1, t2, a2.re, a2.im, wre, -wim);\
    CMUL(t5, t6, a3.re, a3.im, wre,  wim);\
    BUTTERFLIES(a0,a1,a2,a3)\
}

#define TRANSFORM_ZERO(a0,a1,a2,a3) {\
    t1 = a2.re;\
    t2 = a2.im;\
    t5 = a3.re;\
    t6 = a3.im;\
    BUTTERFLIES(a0,a1,a2,a3)\
}

/* z[0...8n-1], w[1...2n-1] */
#define PASS(name)\
static void name(FFTComplex *z, const FFTSample *wre, unsigned int n)\
{\
    FFTDouble t1, t2, t3, t4, t5, t6;\
    int o1 = 2*n;\
    int o2 = 4*n;\
    int o3 = 6*n;\
    const FFTSample *wim = wre+o1;\
    n--;\
\
    TRANSFORM_ZERO(z[0],z[o1],z[o2],z[o3]);\
    TRANSFORM(z[1],z[o1+1],z[o2+1],z[o3+1],wre[1],wim[-1]);\
    do {\
        z += 2;\
        wre += 2;\
        wim -= 2;\
        TRANSFORM(z[0],z[o1],z[o2],z[o3],wre[0],wim[0]);\
        TRANSFORM(z[1],z[o1+1],z[o2+1],z[o3+1],wre[1],wim[-1]);\
    } while(--n);\
}

PASS(pass)
#if !CONFIG_SMALL
#undef BUTTERFLIES
#define BUTTERFLIES BUTTERFLIES_BIG
PASS(pass_big)
#endif

#define DECL_FFT(n,n2,n4)\
static void fft##n(FFTComplex *z)\
{\
    fft##n2(z);\
    fft##n4(z+n4*2);\
    fft##n4(z+n4*3);\
    pass(z,FFT_NAME(ff_cos_##n),n4/2);\
}

static void fft4(FFTComplex *z)
{
    FFTDouble t1, t2, t3, t4, t5, t6, t7, t8;

    BF(t3, t1, z[0].re, z[1].re);
    BF(t8, t6, z[3].re, z[2].re);
    BF(z[2].re, z[0].re, t1, t6);
    BF(t4, t2, z[0].im, z[1].im);
    BF(t7, t5, z[2].im, z[3].im);
    BF(z[3].im, z[1].im, t4, t8);
    BF(z[3].re, z[1].re, t3, t7);
    BF(z[2].im, z[0].im, t2, t5);
}

static void fft8(FFTComplex *z)
{
    FFTDouble t1, t2, t3, t4, t5, t6;

    fft4(z);

    BF(t1, z[5].re, z[4].re, -z[5].re);
    BF(t2, z[5].im, z[4].im, -z[5].im);
    BF(t5, z[7].re, z[6].re, -z[7].re);
    BF(t6, z[7].im, z[6].im, -z[7].im);

    BUTTERFLIES(z[0],z[2],z[4],z[6]);
    TRANSFORM(z[1],z[3],z[5],z[7],sqrthalf,sqrthalf);
}

#if !CONFIG_SMALL
static void fft16(FFTComplex *z)
{
    FFTDouble t1, t2, t3, t4, t5, t6;
    FFTSample cos_16_1 = FFT_NAME(ff_cos_16)[1];
    FFTSample cos_16_3 = FFT_NAME(ff_cos_16)[3];

    fft8(z);
    fft4(z+8);
    fft4(z+12);

    TRANSFORM_ZERO(z[0],z[4],z[8],z[12]);
    TRANSFORM(z[2],z[6],z[10],z[14],sqrthalf,sqrthalf);
    TRANSFORM(z[1],z[5],z[9],z[13],cos_16_1,cos_16_3);
    TRANSFORM(z[3],z[7],z[11],z[15],cos_16_3,cos_16_1);
}
#else
DECL_FFT(16,8,4)
#endif
DECL_FFT(32,16,8)
DECL_FFT(64,32,16)
DECL_FFT(128,64,32)
DECL_FFT(256,128,64)
DECL_FFT(512,256,128)
#if !CONFIG_SMALL
#define pass pass_big
#endif
DECL_FFT(1024,512,256)
DECL_FFT(2048,1024,512)
DECL_FFT(4096,2048,1024)

static void (* const fft_dispatch[])(FFTComplex*) = {
    fft4, fft8, fft16, fft32, fft64, fft128, fft256, fft512, fft1024,
    fft2048, fft4096
};

static void fft_calc_c(FFTContext *s, FFTComplex *z)
{
    fft_dispatch[s->nbits-2](z);
}
#endif /* FFT_FIXED_32 */




/**
 * @file
 * MDCT/IMDCT transforms.
 */

#if FFT_FLOAT
#   define RSCALE(x, y) ((x) + (y))
#else
#if FFT_FIXED_32
#   define RSCALE(x, y) ((int)((x) + (unsigned)(y) + 32) >> 6)
#else /* FFT_FIXED_32 */
#   define RSCALE(x, y) ((int)((x) + (unsigned)(y)) >> 1)
#endif /* FFT_FIXED_32 */
#endif

/**
 * init MDCT or IMDCT computation.
 */
av_cold int ff_mdct_init(FFTContext *s, int nbits, int inverse, double scale)
{
    int n, n4, i;
    double alpha, theta;
    int tstep;

    memset(s, 0, sizeof(*s));
    n = 1 << nbits;
    s->mdct_bits = nbits;
    s->mdct_size = n;
    n4 = n >> 2;
    s->mdct_permutation = FF_MDCT_PERM_NONE;

    if (ff_fft_init(s, s->mdct_bits - 2, inverse) < 0)
        goto fail;

    s->tcos = static_cast<FFTSample *>(av_malloc_array(n/2, sizeof(FFTSample)));
    if (!s->tcos)
        goto fail;

    switch (s->mdct_permutation) {
    case FF_MDCT_PERM_NONE:
        s->tsin = s->tcos + n4;
        tstep = 1;
        break;
    case FF_MDCT_PERM_INTERLEAVE:
        s->tsin = s->tcos + 1;
        tstep = 2;
        break;
    default:
        goto fail;
    }

    theta = 1.0 / 8.0 + (scale < 0 ? n4 : 0);
    scale = sqrt(fabs(scale));
    for(i=0;i<n4;i++) {
        alpha = 2 * M_PI * (i + theta) / n;
#if FFT_FIXED_32
        s->tcos[i*tstep] = lrint(-cos(alpha) * 2147483648.0);
        s->tsin[i*tstep] = lrint(-sin(alpha) * 2147483648.0);
#else
        s->tcos[i*tstep] = FIX15(-cos(alpha) * scale);
        s->tsin[i*tstep] = FIX15(-sin(alpha) * scale);
#endif
    }
    return 0;
 fail:
    ff_mdct_end(s);
    return -1;
}

/**
 * Compute the middle half of the inverse MDCT of size N = 2^nbits,
 * thus excluding the parts that can be derived by symmetry
 * @param output N/2 samples
 * @param input N/2 samples
 */
void ff_imdct_half_c(FFTContext *s, FFTSample *output, const FFTSample *input)
{
    int k, n8, n4, n2, n, j;
    const uint16_t *revtab = s->revtab;
    const FFTSample *tcos = s->tcos;
    const FFTSample *tsin = s->tsin;
    const FFTSample *in1, *in2;
    FFTComplex *z = reinterpret_cast<FFTComplex *>(output);

    n = 1 << s->mdct_bits;
    n2 = n >> 1;
    n4 = n >> 2;
    n8 = n >> 3;

    /* pre rotation */
    in1 = input;
    in2 = input + n2 - 1;
    for(k = 0; k < n4; k++) {
        j=revtab[k];
        CMUL(z[j].re, z[j].im, *in2, *in1, tcos[k], tsin[k]);
        in1 += 2;
        in2 -= 2;
    }
    s->fft_calc(s, z);

    /* post rotation + reordering */
    for(k = 0; k < n8; k++) {
        FFTSample r0, i0, r1, i1;
        CMUL(r0, i1, z[n8-k-1].im, z[n8-k-1].re, tsin[n8-k-1], tcos[n8-k-1]);
        CMUL(r1, i0, z[n8+k  ].im, z[n8+k  ].re, tsin[n8+k  ], tcos[n8+k  ]);
        z[n8-k-1].re = r0;
        z[n8-k-1].im = i0;
        z[n8+k  ].re = r1;
        z[n8+k  ].im = i1;
    }
}

/**
 * Compute inverse MDCT of size N = 2^nbits
 * @param output N samples
 * @param input N/2 samples
 */
void ff_imdct_calc_c(FFTContext *s, FFTSample *output, const FFTSample *input)
{
    int k;
    int n = 1 << s->mdct_bits;
    int n2 = n >> 1;
    int n4 = n >> 2;

    ff_imdct_half_c(s, output+n4, input);

    for(k = 0; k < n4; k++) {
        output[k] = -output[n2-k-1];
        output[n-k-1] = output[n2+k];
    }
}

/**
 * Compute MDCT of size N = 2^nbits
 * @param input N samples
 * @param out N/2 samples
 */
void ff_mdct_calc_c(FFTContext *s, FFTSample *out, const FFTSample *input)
{
    int i, j, n, n8, n4, n2, n3;
    FFTDouble re, im;
    const uint16_t *revtab = s->revtab;
    const FFTSample *tcos = s->tcos;
    const FFTSample *tsin = s->tsin;
    FFTComplex *x = reinterpret_cast<FFTComplex *>(out);

    n = 1 << s->mdct_bits;
    n2 = n >> 1;
    n4 = n >> 2;
    n8 = n >> 3;
    n3 = 3 * n4;

    /* pre rotation */
    for(i=0;i<n8;i++) {
        re = RSCALE(-input[2*i+n3], - input[n3-1-2*i]);
        im = RSCALE(-input[n4+2*i], + input[n4-1-2*i]);
        j = revtab[i];
        CMUL(x[j].re, x[j].im, re, im, -tcos[i], tsin[i]);

        re = RSCALE( input[2*i]   , - input[n2-1-2*i]);
        im = RSCALE(-input[n2+2*i], - input[ n-1-2*i]);
        j = revtab[n8 + i];
        CMUL(x[j].re, x[j].im, re, im, -tcos[n8 + i], tsin[n8 + i]);
    }

    s->fft_calc(s, x);

    /* post rotation */
    for(i=0;i<n8;i++) {
        FFTSample r0, i0, r1, i1;
        CMUL(i1, r0, x[n8-i-1].re, x[n8-i-1].im, -tsin[n8-i-1], -tcos[n8-i-1]);
        CMUL(i0, r1, x[n8+i  ].re, x[n8+i  ].im, -tsin[n8+i  ], -tcos[n8+i  ]);
        x[n8-i-1].re = r0;
        x[n8-i-1].im = i0;
        x[n8+i  ].re = r1;
        x[n8+i  ].im = i1;
    }
}

av_cold void ff_mdct_end(FFTContext *s)
{
    av_freep(&s->tcos);
    ff_fft_end(s);
}
// WMA shared init/tables, from FFmpeg n4.4.4 libavcodec/wma.c, wma_common.c and
// wma_freqs.c (LGPLv2.1+). Decode logic is untouched; adaptations are plumbing:
// FFmpeg's logging/alloc wrappers, and the sine-window and float_dsp helpers
// which are inlined here as their reference C implementations.

/* ---- sine windows (libavcodec/sinewin_tablegen.h, LGPLv2.1+) ---- */
SINETABLE(32);
SINETABLE(64);
SINETABLE(128);
SINETABLE(256);
SINETABLE(512);
SINETABLE(1024);
SINETABLE(2048);

float *const ff_sine_windows[WMA_SINE_MAX_INDEX + 1] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, // unused
    ff_sine_32, ff_sine_64, ff_sine_128,
    ff_sine_256, ff_sine_512, ff_sine_1024, ff_sine_2048,
};

av_cold void ff_sine_window_init(float *window, int n)
{
    int i;
    for(i = 0; i < n; i++)
        window[i] = sinf((i + 0.5) * (M_PI / (2.0 * n)));
}

static std::once_flag sine_once[WMA_SINE_MAX_INDEX + 1];

void ff_init_ff_sine_windows(int index)
{
    if (index < 5 || index > WMA_SINE_MAX_INDEX)
        return;
    std::call_once(sine_once[index], [index]() {
        ff_sine_window_init(ff_sine_windows[index], 1 << index);
    });
}

/* ---- float_dsp reference C paths (libavutil/float_dsp.c, LGPLv2.1+) ---- */
static void vector_fmul_add_c(float *dst, const float *src0, const float *src1,
                              const float *src2, int len)
{
    int i;
    for (i = 0; i < len; i++)
        dst[i] = src0[i] * src1[i] + src2[i];
}

static void vector_fmul_reverse_c(float *dst, const float *src0,
                                  const float *src1, int len)
{
    int i;
    src1 += len-1;
    for (i = 0; i < len; i++)
        dst[i] = src0[i] * src1[-i];
}

static void butterflies_float_c(float *av_restrict v1, float *av_restrict v2,
                                int len)
{
    int i;
    for (i = 0; i < len; i++) {
        float t = v1[i] - v2[i];
        v1[i] += v2[i];
        v2[i] = t;
    }
}

AVFloatDSPContext *avpriv_float_dsp_alloc(int bitexact)
{
    (void)bitexact; // only affects routines WMA does not use
    AVFloatDSPContext *fdsp =
        static_cast<AVFloatDSPContext *>(av_mallocz(sizeof(AVFloatDSPContext)));
    if (!fdsp)
        return nullptr;
    fdsp->vector_fmul_add     = vector_fmul_add_c;
    fdsp->vector_fmul_reverse = vector_fmul_reverse_c;
    fdsp->butterflies_float   = butterflies_float_c;
    return fdsp;
}





const uint16_t ff_wma_critical_freqs[25] = {
      100,   200,  300,  400,  510,  630,   770,   920,
     1080,  1270, 1480, 1720, 2000, 2320,  2700,  3150,
     3700,  4400, 5300, 6400, 7700, 9500, 12000, 15500,
    24500,
};





/**
 *@brief Get the samples per frame for this stream.
 *@param sample_rate output sample_rate
 *@param version wma version
 *@param decode_flags codec compression features
 *@return log2 of the number of output samples per frame
 */
av_cold int ff_wma_get_frame_len_bits(int sample_rate, int version,
                                      unsigned int decode_flags)
{
    int frame_len_bits;

    if (sample_rate <= 16000)
        frame_len_bits = 9;
    else if (sample_rate <= 22050 || (sample_rate <= 32000 && version == 1))
        frame_len_bits = 10;
    else if (sample_rate <= 48000 || version < 3)
        frame_len_bits = 11;
    else if (sample_rate <= 96000)
        frame_len_bits = 12;
    else
        frame_len_bits = 13;

    if (version == 3) {
        int tmp = decode_flags & 0x6;
        if (tmp == 0x2)
            ++frame_len_bits;
        else if (tmp == 0x4)
            --frame_len_bits;
        else if (tmp == 0x6)
            frame_len_bits -= 2;
    }

    return frame_len_bits;
}




#include "wmadata.h"

/* XXX: use same run/length optimization as mpeg decoders */
// FIXME maybe split decode / encode or pass flag
static av_cold int init_coef_vlc(VLC *vlc, uint16_t **prun_table,
                                 float **plevel_table, uint16_t **pint_table,
                                 const CoefVLCTable *vlc_table)
{
    int n                        = vlc_table->n;
    const uint8_t  *table_bits   = vlc_table->huffbits;
    const uint32_t *table_codes  = vlc_table->huffcodes;
    const uint16_t *levels_table = vlc_table->levels;
    uint16_t *run_table, *int_table;
    float *flevel_table;
    int i, l, j, k, level;

    init_vlc(vlc, VLCBITS, n, table_bits, 1, 1, table_codes, 4, 4, 0);

    run_table    = static_cast<uint16_t *>(av_malloc_array(n, sizeof(uint16_t)));
    flevel_table = static_cast<float *>(av_malloc_array(n, sizeof(*flevel_table)));
    int_table    = static_cast<uint16_t *>(av_malloc_array(n, sizeof(uint16_t)));
    if (!run_table || !flevel_table || !int_table) {
        av_freep(&run_table);
        av_freep(&flevel_table);
        av_freep(&int_table);
        return AVERROR(ENOMEM);
    }
    i            = 2;
    level        = 1;
    k            = 0;
    while (i < n) {
        int_table[k] = i;
        l            = levels_table[k++];
        for (j = 0; j < l; j++) {
            run_table[i]    = j;
            flevel_table[i] = level;
            i++;
        }
        level++;
    }
    *prun_table   = run_table;
    *plevel_table = flevel_table;
    *pint_table   = int_table;

    return 0;
}

av_cold int ff_wma_init(AVCodecContext *avctx, int flags2)
{
    WMACodecContext *s = static_cast<WMACodecContext *>(avctx->priv_data);
    int i, ret;
    float bps1, high_freq;
    volatile float bps;
    int sample_rate1;
    int coef_vlc_table;

    if (avctx->sample_rate <= 0 || avctx->sample_rate > 50000 ||
        avctx->channels    <= 0 || avctx->channels    > 2     ||
        avctx->bit_rate    <= 0)
        return -1;


    if (avctx->codec->id == AV_CODEC_ID_WMAV1)
        s->version = 1;
    else
        s->version = 2;

    /* compute MDCT block size */
    s->frame_len_bits = ff_wma_get_frame_len_bits(avctx->sample_rate,
                                                  s->version, 0);
    s->next_block_len_bits = s->frame_len_bits;
    s->prev_block_len_bits = s->frame_len_bits;
    s->block_len_bits      = s->frame_len_bits;

    s->frame_len = 1 << s->frame_len_bits;
    if (s->use_variable_block_len) {
        int nb_max, nb;
        nb = ((flags2 >> 3) & 3) + 1;
        if ((avctx->bit_rate / avctx->channels) >= 32000)
            nb += 2;
        nb_max = s->frame_len_bits - BLOCK_MIN_BITS;
        if (nb > nb_max)
            nb = nb_max;
        s->nb_block_sizes = nb + 1;
    } else
        s->nb_block_sizes = 1;

    /* init rate dependent parameters */
    s->use_noise_coding = 1;
    high_freq           = avctx->sample_rate * 0.5;

    /* if version 2, then the rates are normalized */
    sample_rate1 = avctx->sample_rate;
    if (s->version == 2) {
        if (sample_rate1 >= 44100)
            sample_rate1 = 44100;
        else if (sample_rate1 >= 22050)
            sample_rate1 = 22050;
        else if (sample_rate1 >= 16000)
            sample_rate1 = 16000;
        else if (sample_rate1 >= 11025)
            sample_rate1 = 11025;
        else if (sample_rate1 >= 8000)
            sample_rate1 = 8000;
    }

    bps                 = (float) avctx->bit_rate /
                          (float) (avctx->channels * avctx->sample_rate);
    s->byte_offset_bits = av_log2((int) (bps * s->frame_len / 8.0 + 0.5)) + 2;
    if (s->byte_offset_bits + 3 > MIN_CACHE_BITS) {
        av_log(avctx, AV_LOG_ERROR, "byte_offset_bits %d is too large\n", s->byte_offset_bits);
        return AVERROR_PATCHWELCOME;
    }

    /* compute high frequency value and choose if noise coding should
     * be activated */
    bps1 = bps;
    if (avctx->channels == 2)
        bps1 = bps * 1.6;
    if (sample_rate1 == 44100) {
        if (bps1 >= 0.61)
            s->use_noise_coding = 0;
        else
            high_freq = high_freq * 0.4;
    } else if (sample_rate1 == 22050) {
        if (bps1 >= 1.16)
            s->use_noise_coding = 0;
        else if (bps1 >= 0.72)
            high_freq = high_freq * 0.7;
        else
            high_freq = high_freq * 0.6;
    } else if (sample_rate1 == 16000) {
        if (bps > 0.5)
            high_freq = high_freq * 0.5;
        else
            high_freq = high_freq * 0.3;
    } else if (sample_rate1 == 11025)
        high_freq = high_freq * 0.7;
    else if (sample_rate1 == 8000) {
        if (bps <= 0.625)
            high_freq = high_freq * 0.5;
        else if (bps > 0.75)
            s->use_noise_coding = 0;
        else
            high_freq = high_freq * 0.65;
    } else {
        if (bps >= 0.8)
            high_freq = high_freq * 0.75;
        else if (bps >= 0.6)
            high_freq = high_freq * 0.6;
        else
            high_freq = high_freq * 0.5;
    }
    ff_dlog(s->avctx, "flags2=0x%x\n", flags2);
    ff_dlog(s->avctx, "version=%d channels=%d sample_rate=%d bitrate=%" PRId64 " block_align=%d\n",
            s->version, avctx->channels, avctx->sample_rate, avctx->bit_rate,
            avctx->block_align);
    ff_dlog(s->avctx, "bps=%f bps1=%f high_freq=%f bitoffset=%d\n",
            bps, bps1, high_freq, s->byte_offset_bits);
    ff_dlog(s->avctx, "use_noise_coding=%d use_exp_vlc=%d nb_block_sizes=%d\n",
            s->use_noise_coding, s->use_exp_vlc, s->nb_block_sizes);

    /* compute the scale factor band sizes for each MDCT block size */
    {
        int a, b, pos, lpos, k, block_len, i, j, n;
        const uint8_t *table;

        if (s->version == 1)
            s->coefs_start = 3;
        else
            s->coefs_start = 0;
        for (k = 0; k < s->nb_block_sizes; k++) {
            block_len = s->frame_len >> k;

            if (s->version == 1) {
                lpos = 0;
                for (i = 0; i < 25; i++) {
                    a   = ff_wma_critical_freqs[i];
                    b   = avctx->sample_rate;
                    pos = ((block_len * 2 * a) + (b >> 1)) / b;
                    if (pos > block_len)
                        pos = block_len;
                    s->exponent_bands[0][i] = pos - lpos;
                    if (pos >= block_len) {
                        i++;
                        break;
                    }
                    lpos = pos;
                }
                s->exponent_sizes[0] = i;
            } else {
                /* hardcoded tables */
                table = NULL;
                a     = s->frame_len_bits - BLOCK_MIN_BITS - k;
                if (a < 3) {
                    if (avctx->sample_rate >= 44100)
                        table = exponent_band_44100[a];
                    else if (avctx->sample_rate >= 32000)
                        table = exponent_band_32000[a];
                    else if (avctx->sample_rate >= 22050)
                        table = exponent_band_22050[a];
                }
                if (table) {
                    n = *table++;
                    for (i = 0; i < n; i++)
                        s->exponent_bands[k][i] = table[i];
                    s->exponent_sizes[k] = n;
                } else {
                    j    = 0;
                    lpos = 0;
                    for (i = 0; i < 25; i++) {
                        a     = ff_wma_critical_freqs[i];
                        b     = avctx->sample_rate;
                        pos   = ((block_len * 2 * a) + (b << 1)) / (4 * b);
                        pos <<= 2;
                        if (pos > block_len)
                            pos = block_len;
                        if (pos > lpos)
                            s->exponent_bands[k][j++] = pos - lpos;
                        if (pos >= block_len)
                            break;
                        lpos = pos;
                    }
                    s->exponent_sizes[k] = j;
                }
            }

            /* max number of coefs */
            s->coefs_end[k] = (s->frame_len - ((s->frame_len * 9) / 100)) >> k;
            /* high freq computation */
            s->high_band_start[k] = (int) ((block_len * 2 * high_freq) /
                                           avctx->sample_rate + 0.5);
            n   = s->exponent_sizes[k];
            j   = 0;
            pos = 0;
            for (i = 0; i < n; i++) {
                int start, end;
                start = pos;
                pos  += s->exponent_bands[k][i];
                end   = pos;
                if (start < s->high_band_start[k])
                    start = s->high_band_start[k];
                if (end > s->coefs_end[k])
                    end = s->coefs_end[k];
                if (end > start)
                    s->exponent_high_bands[k][j++] = end - start;
            }
            s->exponent_high_sizes[k] = j;
        }
    }

#ifdef TRACE
    {
        int i, j;
        for (i = 0; i < s->nb_block_sizes; i++) {
            ff_tlog(s->avctx, "%5d: n=%2d:",
                    s->frame_len >> i,
                    s->exponent_sizes[i]);
            for (j = 0; j < s->exponent_sizes[i]; j++)
                ff_tlog(s->avctx, " %d", s->exponent_bands[i][j]);
            ff_tlog(s->avctx, "\n");
        }
    }
#endif /* TRACE */

    /* init MDCT windows : simple sine window */
    for (i = 0; i < s->nb_block_sizes; i++) {
        ff_init_ff_sine_windows(s->frame_len_bits - i);
        s->windows[i] = ff_sine_windows[s->frame_len_bits - i];
    }

    s->reset_block_lengths = 1;

    if (s->use_noise_coding) {
        /* init the noise generator */
        if (s->use_exp_vlc)
            s->noise_mult = 0.02;
        else
            s->noise_mult = 0.04;

#ifdef TRACE
        for (i = 0; i < NOISE_TAB_SIZE; i++)
            s->noise_table[i] = 1.0 * s->noise_mult;
#else
        {
            unsigned int seed;
            float norm;
            seed = 1;
            norm = (1.0 / (float) (1LL << 31)) * sqrt(3) * s->noise_mult;
            for (i = 0; i < NOISE_TAB_SIZE; i++) {
                seed              = seed * 314159 + 1;
                s->noise_table[i] = (float) ((int) seed) * norm;
            }
        }
#endif /* TRACE */
    }

    s->fdsp = avpriv_float_dsp_alloc(avctx->flags & AV_CODEC_FLAG_BITEXACT);
    if (!s->fdsp)
        return AVERROR(ENOMEM);

    /* choose the VLC tables for the coefficients */
    coef_vlc_table = 2;
    if (avctx->sample_rate >= 32000) {
        if (bps1 < 0.72)
            coef_vlc_table = 0;
        else if (bps1 < 1.16)
            coef_vlc_table = 1;
    }
    s->coef_vlcs[0] = &coef_vlcs[coef_vlc_table * 2];
    s->coef_vlcs[1] = &coef_vlcs[coef_vlc_table * 2 + 1];
    ret = init_coef_vlc(&s->coef_vlc[0], &s->run_table[0], &s->level_table[0],
                        &s->int_table[0], s->coef_vlcs[0]);
    if (ret < 0)
        return ret;

    return init_coef_vlc(&s->coef_vlc[1], &s->run_table[1], &s->level_table[1],
                         &s->int_table[1], s->coef_vlcs[1]);
}

int ff_wma_total_gain_to_bits(int total_gain)
{
    if (total_gain < 15)
        return 13;
    else if (total_gain < 32)
        return 12;
    else if (total_gain < 40)
        return 11;
    else if (total_gain < 45)
        return 10;
    else
        return  9;
}

int ff_wma_end(AVCodecContext *avctx)
{
    WMACodecContext *s = static_cast<WMACodecContext *>(avctx->priv_data);
    int i;

    for (i = 0; i < s->nb_block_sizes; i++)
        ff_mdct_end(&s->mdct_ctx[i]);

    if (s->use_exp_vlc)
        ff_free_vlc(&s->exp_vlc);
    if (s->use_noise_coding)
        ff_free_vlc(&s->hgain_vlc);
    for (i = 0; i < 2; i++) {
        ff_free_vlc(&s->coef_vlc[i]);
        av_freep(&s->run_table[i]);
        av_freep(&s->level_table[i]);
        av_freep(&s->int_table[i]);
    }
    av_freep(&s->fdsp);
    av_freep(&s->out_buf);

    return 0;
}

/**
 * Decode an uncompressed coefficient.
 * @param gb GetBitContext
 * @return the decoded coefficient
 */
unsigned int ff_wma_get_large_val(GetBitContext *gb)
{
    /** consumes up to 34 bits */
    int n_bits = 8;
    /** decode length */
    if (get_bits1(gb)) {
        n_bits += 8;
        if (get_bits1(gb)) {
            n_bits += 8;
            if (get_bits1(gb))
                n_bits += 7;
        }
    }
    return get_bits_long(gb, n_bits);
}

/**
 * Decode run level compressed coefficients.
 * @param avctx codec context
 * @param gb bitstream reader context
 * @param vlc vlc table for get_vlc2
 * @param level_table level codes
 * @param run_table run codes
 * @param version 0 for wma1,2 1 for wmapro
 * @param ptr output buffer
 * @param offset offset in the output buffer
 * @param num_coefs number of input coefficients
 * @param block_len input buffer length (2^n)
 * @param frame_len_bits number of bits for escaped run codes
 * @param coef_nb_bits number of bits for escaped level codes
 * @return 0 on success, -1 otherwise
 */
int ff_wma_run_level_decode(AVCodecContext *avctx, GetBitContext *gb,
                            VLC *vlc, const float *level_table,
                            const uint16_t *run_table, int version,
                            WMACoef *ptr, int offset, int num_coefs,
                            int block_len, int frame_len_bits,
                            int coef_nb_bits)
{
    int code, level, sign;
    const uint32_t *ilvl = reinterpret_cast<const uint32_t *>(level_table);
    uint32_t *iptr = reinterpret_cast<uint32_t *>(ptr);
    const unsigned int coef_mask = block_len - 1;
    for (; offset < num_coefs; offset++) {
        code = get_vlc2(gb, vlc->table, VLCBITS, VLCMAX);
        if (code > 1) {
            /** normal code */
            offset                  += run_table[code];
            sign                     = get_bits1(gb) - 1;
            iptr[offset & coef_mask] = ilvl[code] ^ (sign & 0x80000000);
        } else if (code == 1) {
            /** EOB */
            break;
        } else {
            /** escape */
            if (!version) {
                level = get_bits(gb, coef_nb_bits);
                /** NOTE: this is rather suboptimal. reading
                 *  block_len_bits would be better */
                offset += get_bits(gb, frame_len_bits);
            } else {
                level = ff_wma_get_large_val(gb);
                /** escape decode */
                if (get_bits1(gb)) {
                    if (get_bits1(gb)) {
                        if (get_bits1(gb)) {
                            av_log(avctx, AV_LOG_ERROR,
                                   "broken escape sequence\n");
                            return AVERROR_INVALIDDATA;
                        } else
                            offset += get_bits(gb, frame_len_bits) + 4;
                    } else
                        offset += get_bits(gb, 2) + 1;
                }
            }
            sign                    = get_bits1(gb) - 1;
            ptr[offset & coef_mask] = (level ^ sign) - sign;
        }
    }
    /** NOTE: EOB can be omitted */
    if (offset > num_coefs) {
        av_log(avctx, AV_LOG_ERROR,
               "overflow (%d > %d) in spectral RLE, ignoring\n",
               offset,
               num_coefs
              );
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

/* AAC scalefactor VLC tables, from FFmpeg n4.4.4 libavcodec/aactab.c
 * (LGPLv2.1+), copied verbatim. wmadec builds its exponent VLC from these. */
const uint32_t ff_aac_scalefactor_code[121] = {
    0x3ffe8, 0x3ffe6, 0x3ffe7, 0x3ffe5, 0x7fff5, 0x7fff1, 0x7ffed, 0x7fff6,
    0x7ffee, 0x7ffef, 0x7fff0, 0x7fffc, 0x7fffd, 0x7ffff, 0x7fffe, 0x7fff7,
    0x7fff8, 0x7fffb, 0x7fff9, 0x3ffe4, 0x7fffa, 0x3ffe3, 0x1ffef, 0x1fff0,
    0x0fff5, 0x1ffee, 0x0fff2, 0x0fff3, 0x0fff4, 0x0fff1, 0x07ff6, 0x07ff7,
    0x03ff9, 0x03ff5, 0x03ff7, 0x03ff3, 0x03ff6, 0x03ff2, 0x01ff7, 0x01ff5,
    0x00ff9, 0x00ff7, 0x00ff6, 0x007f9, 0x00ff4, 0x007f8, 0x003f9, 0x003f7,
    0x003f5, 0x001f8, 0x001f7, 0x000fa, 0x000f8, 0x000f6, 0x00079, 0x0003a,
    0x00038, 0x0001a, 0x0000b, 0x00004, 0x00000, 0x0000a, 0x0000c, 0x0001b,
    0x00039, 0x0003b, 0x00078, 0x0007a, 0x000f7, 0x000f9, 0x001f6, 0x001f9,
    0x003f4, 0x003f6, 0x003f8, 0x007f5, 0x007f4, 0x007f6, 0x007f7, 0x00ff5,
    0x00ff8, 0x01ff4, 0x01ff6, 0x01ff8, 0x03ff8, 0x03ff4, 0x0fff0, 0x07ff4,
    0x0fff6, 0x07ff5, 0x3ffe2, 0x7ffd9, 0x7ffda, 0x7ffdb, 0x7ffdc, 0x7ffdd,
    0x7ffde, 0x7ffd8, 0x7ffd2, 0x7ffd3, 0x7ffd4, 0x7ffd5, 0x7ffd6, 0x7fff2,
    0x7ffdf, 0x7ffe7, 0x7ffe8, 0x7ffe9, 0x7ffea, 0x7ffeb, 0x7ffe6, 0x7ffe0,
    0x7ffe1, 0x7ffe2, 0x7ffe3, 0x7ffe4, 0x7ffe5, 0x7ffd7, 0x7ffec, 0x7fff4,
    0x7fff3,
};

const uint8_t ff_aac_scalefactor_bits[121] = {
    18, 18, 18, 18, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 18, 19, 18, 17, 17, 16, 17, 16, 16, 16, 16, 15, 15,
    14, 14, 14, 14, 14, 14, 13, 13, 12, 12, 12, 11, 12, 11, 10, 10,
    10,  9,  9,  8,  8,  8,  7,  6,  6,  5,  4,  3,  1,  4,  4,  5,
     6,  6,  7,  7,  8,  8,  9,  9, 10, 10, 10, 11, 11, 11, 11, 12,
    12, 13, 13, 13, 14, 14, 16, 15, 16, 15, 18, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19, 19,
    19, 19, 19, 19, 19, 19, 19, 19, 19,
};
// WMA v1/v2 decoder, from FFmpeg n4.4.4 libavcodec/wmadec.c (LGPLv2.1+).
// The decode math is untouched. Adaptations are plumbing only:
//   - AVFrame/ff_get_buffer() become a fixed output buffer sized once at init.
//     FFmpeg allocates per packet; a superframe can claim up to 15 frames, so
//     sizing for that once keeps allocation off the decode path entirely.
//   - The AVCodec registration tables are dropped; mocf registers its own.

#define EXPVLCBITS 8
#define EXPMAX ((19 + EXPVLCBITS - 1) / EXPVLCBITS)

#define HGAINVLCBITS 9
#define HGAINMAX ((13 + HGAINVLCBITS - 1) / HGAINVLCBITS)

// A superframe header carries a 4-bit frame count.
#define WMA_MAX_FRAMES_PER_SUPERFRAME 15

static int wma_decode_frame(WMACodecContext *s, float **samples,
                            int samples_offset);

/* Stands in for ff_get_buffer(): hands back the buffer reserved at init rather
 * than allocating, and refuses a frame count that would overrun it. */
static int wma_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags)
{
    WMACodecContext *s = static_cast<WMACodecContext *>(avctx->priv_data);
    int ch;

    (void)flags;
    if (frame->nb_samples < 0 || frame->nb_samples > s->out_samples_max)
        return AVERROR_INVALIDDATA;

    for (ch = 0; ch < avctx->channels; ch++)
        frame->extended_data[ch] = s->out_buf + ch * s->out_samples_max;
    return 0;
}
#define ff_get_buffer(avctx, frame, flags) wma_get_buffer(avctx, frame, flags)



/**
 * @file
 * WMA compatible decoder.
 * This decoder handles Microsoft Windows Media Audio data, versions 1 & 2.
 * WMA v1 is identified by audio format 0x160 in Microsoft media files
 * (ASF/AVI/WAV). WMA v2 is identified by audio format 0x161.
 *
 * To use this decoder, a calling application must supply the extra data
 * bytes provided with the WMA data. These are the extra, codec-specific
 * bytes at the end of a WAVEFORMATEX data structure. Transmit these bytes
 * to the decoder using the extradata[_size] fields in AVCodecContext. There
 * should be 4 extra bytes for v1 data and 6 extra bytes for v2 data.
 */



#define EXPVLCBITS 8
#define EXPMAX     ((19 + EXPVLCBITS - 1) / EXPVLCBITS)

#define HGAINVLCBITS 9
#define HGAINMAX     ((13 + HGAINVLCBITS - 1) / HGAINVLCBITS)

static void wma_lsp_to_curve_init(WMACodecContext *s, int frame_len);

#ifdef TRACE
static void dump_floats(WMACodecContext *s, const char *name,
                        int prec, const float *tab, int n)
{
    int i;

    ff_tlog(s->avctx, "%s[%d]:\n", name, n);
    for (i = 0; i < n; i++) {
        if ((i & 7) == 0)
            ff_tlog(s->avctx, "%4d: ", i);
        ff_tlog(s->avctx, " %8.*f", prec, tab[i]);
        if ((i & 7) == 7)
            ff_tlog(s->avctx, "\n");
    }
    if ((i & 7) != 0)
        ff_tlog(s->avctx, "\n");
}
#endif /* TRACE */

static av_cold int wma_decode_init(AVCodecContext *avctx)
{
    WMACodecContext *s = static_cast<WMACodecContext *>(avctx->priv_data);
    int i, flags2;
    uint8_t *extradata;

    if (!avctx->block_align) {
        av_log(avctx, AV_LOG_ERROR, "block_align is not set\n");
        return AVERROR(EINVAL);
    }

    s->avctx = avctx;

    /* extract flag info */
    flags2    = 0;
    extradata = avctx->extradata;
    if (avctx->codec->id == AV_CODEC_ID_WMAV1 && avctx->extradata_size >= 4)
        flags2 = AV_RL16(extradata + 2);
    else if (avctx->codec->id == AV_CODEC_ID_WMAV2 && avctx->extradata_size >= 6)
        flags2 = AV_RL16(extradata + 4);

    s->use_exp_vlc            = flags2 & 0x0001;
    s->use_bit_reservoir      = flags2 & 0x0002;
    s->use_variable_block_len = flags2 & 0x0004;

    if (avctx->codec->id == AV_CODEC_ID_WMAV2 && avctx->extradata_size >= 8){
        if (AV_RL16(extradata+4)==0xd && s->use_variable_block_len){
            av_log(avctx, AV_LOG_WARNING, "Disabling use_variable_block_len, if this fails contact the ffmpeg developers and send us the file\n");
            s->use_variable_block_len= 0; // this fixes issue1503
        }
    }

    for (i=0; i<MAX_CHANNELS; i++)
        s->max_exponent[i] = 1.0;

    if (ff_wma_init(avctx, flags2) < 0)
        return -1;

    /* init MDCT */
    for (i = 0; i < s->nb_block_sizes; i++)
        ff_mdct_init(&s->mdct_ctx[i], s->frame_len_bits - i + 1, 1, 1.0 / 32768.0);

    if (s->use_noise_coding) {
        ff_init_vlc_from_lengths(&s->hgain_vlc, HGAINVLCBITS, FF_ARRAY_ELEMS(ff_wma_hgain_hufftab),
                                 reinterpret_cast<const int8_t *>(&ff_wma_hgain_hufftab[0][1]), 2,
                                 &ff_wma_hgain_hufftab[0][0], 2, 1, -18, 0, avctx);
    }

    if (s->use_exp_vlc)
        init_vlc(&s->exp_vlc, EXPVLCBITS, sizeof(ff_aac_scalefactor_bits), // FIXME move out of context
                 ff_aac_scalefactor_bits, 1, 1,
                 ff_aac_scalefactor_code, 4, 4, 0);
    else
        wma_lsp_to_curve_init(s, s->frame_len);

    avctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

    /* Reserve the output buffer once: a superframe may declare up to 15 frames
     * of frame_len samples per channel. Doing it here keeps malloc off the
     * decode path entirely. */
    s->out_samples_max = WMA_MAX_FRAMES_PER_SUPERFRAME * s->frame_len;
    s->out_buf = static_cast<float *>(
        av_malloc_array(static_cast<size_t>(s->out_samples_max) * MAX_CHANNELS,
                        sizeof(float)));
    if (!s->out_buf)
        return AVERROR(ENOMEM);

    return 0;
}

/**
 * compute x^-0.25 with an exponent and mantissa table. We use linear
 * interpolation to reduce the mantissa table size at a small speed
 * expense (linear interpolation approximately doubles the number of
 * bits of precision).
 */
static inline float pow_m1_4(WMACodecContext *s, float x)
{
    union {
        float f;
        unsigned int v;
    } u, t;
    unsigned int e, m;
    float a, b;

    u.f = x;
    e   =  u.v >>  23;
    m   = (u.v >> (23 - LSP_POW_BITS)) & ((1 << LSP_POW_BITS) - 1);
    /* build interpolation scale: 1 <= t < 2. */
    t.v = ((u.v << LSP_POW_BITS) & ((1 << 23) - 1)) | (127 << 23);
    a   = s->lsp_pow_m_table1[m];
    b   = s->lsp_pow_m_table2[m];
    return s->lsp_pow_e_table[e] * (a + b * t.f);
}

static av_cold void wma_lsp_to_curve_init(WMACodecContext *s, int frame_len)
{
    float wdel, a, b;
    int i, e, m;

    wdel = M_PI / frame_len;
    for (i = 0; i < frame_len; i++)
        s->lsp_cos_table[i] = 2.0f * cos(wdel * i);

    /* tables for x^-0.25 computation */
    for (i = 0; i < 256; i++) {
        e                     = i - 126;
        s->lsp_pow_e_table[i] = exp2f(e * -0.25);
    }

    /* NOTE: these two tables are needed to avoid two operations in
     * pow_m1_4 */
    b = 1.0;
    for (i = (1 << LSP_POW_BITS) - 1; i >= 0; i--) {
        m                      = (1 << LSP_POW_BITS) + i;
        a                      = (float) m * (0.5 / (1 << LSP_POW_BITS));
        a                      = 1/sqrt(sqrt(a));
        s->lsp_pow_m_table1[i] = 2 * a - b;
        s->lsp_pow_m_table2[i] = b - a;
        b                      = a;
    }
}

/**
 * NOTE: We use the same code as Vorbis here
 * @todo optimize it further with SSE/3Dnow
 */
static void wma_lsp_to_curve(WMACodecContext *s, float *out, float *val_max_ptr,
                             int n, float *lsp)
{
    int i, j;
    float p, q, w, v, val_max;

    val_max = 0;
    for (i = 0; i < n; i++) {
        p = 0.5f;
        q = 0.5f;
        w = s->lsp_cos_table[i];
        for (j = 1; j < NB_LSP_COEFS; j += 2) {
            q *= w - lsp[j - 1];
            p *= w - lsp[j];
        }
        p *= p * (2.0f - w);
        q *= q * (2.0f + w);
        v  = p + q;
        v  = pow_m1_4(s, v);
        if (v > val_max)
            val_max = v;
        out[i] = v;
    }
    *val_max_ptr = val_max;
}

/**
 * decode exponents coded with LSP coefficients (same idea as Vorbis)
 */
static void decode_exp_lsp(WMACodecContext *s, int ch)
{
    float lsp_coefs[NB_LSP_COEFS];
    int val, i;

    for (i = 0; i < NB_LSP_COEFS; i++) {
        if (i == 0 || i >= 8)
            val = get_bits(&s->gb, 3);
        else
            val = get_bits(&s->gb, 4);
        lsp_coefs[i] = ff_wma_lsp_codebook[i][val];
    }

    wma_lsp_to_curve(s, s->exponents[ch], &s->max_exponent[ch],
                     s->block_len, lsp_coefs);
}

/** pow(10, i / 16.0) for i in -60..95 */
static const float pow_tab[] = {
    1.7782794100389e-04, 2.0535250264571e-04,
    2.3713737056617e-04, 2.7384196342644e-04,
    3.1622776601684e-04, 3.6517412725484e-04,
    4.2169650342858e-04, 4.8696752516586e-04,
    5.6234132519035e-04, 6.4938163157621e-04,
    7.4989420933246e-04, 8.6596432336006e-04,
    1.0000000000000e-03, 1.1547819846895e-03,
    1.3335214321633e-03, 1.5399265260595e-03,
    1.7782794100389e-03, 2.0535250264571e-03,
    2.3713737056617e-03, 2.7384196342644e-03,
    3.1622776601684e-03, 3.6517412725484e-03,
    4.2169650342858e-03, 4.8696752516586e-03,
    5.6234132519035e-03, 6.4938163157621e-03,
    7.4989420933246e-03, 8.6596432336006e-03,
    1.0000000000000e-02, 1.1547819846895e-02,
    1.3335214321633e-02, 1.5399265260595e-02,
    1.7782794100389e-02, 2.0535250264571e-02,
    2.3713737056617e-02, 2.7384196342644e-02,
    3.1622776601684e-02, 3.6517412725484e-02,
    4.2169650342858e-02, 4.8696752516586e-02,
    5.6234132519035e-02, 6.4938163157621e-02,
    7.4989420933246e-02, 8.6596432336007e-02,
    1.0000000000000e-01, 1.1547819846895e-01,
    1.3335214321633e-01, 1.5399265260595e-01,
    1.7782794100389e-01, 2.0535250264571e-01,
    2.3713737056617e-01, 2.7384196342644e-01,
    3.1622776601684e-01, 3.6517412725484e-01,
    4.2169650342858e-01, 4.8696752516586e-01,
    5.6234132519035e-01, 6.4938163157621e-01,
    7.4989420933246e-01, 8.6596432336007e-01,
    1.0000000000000e+00, 1.1547819846895e+00,
    1.3335214321633e+00, 1.5399265260595e+00,
    1.7782794100389e+00, 2.0535250264571e+00,
    2.3713737056617e+00, 2.7384196342644e+00,
    3.1622776601684e+00, 3.6517412725484e+00,
    4.2169650342858e+00, 4.8696752516586e+00,
    5.6234132519035e+00, 6.4938163157621e+00,
    7.4989420933246e+00, 8.6596432336007e+00,
    1.0000000000000e+01, 1.1547819846895e+01,
    1.3335214321633e+01, 1.5399265260595e+01,
    1.7782794100389e+01, 2.0535250264571e+01,
    2.3713737056617e+01, 2.7384196342644e+01,
    3.1622776601684e+01, 3.6517412725484e+01,
    4.2169650342858e+01, 4.8696752516586e+01,
    5.6234132519035e+01, 6.4938163157621e+01,
    7.4989420933246e+01, 8.6596432336007e+01,
    1.0000000000000e+02, 1.1547819846895e+02,
    1.3335214321633e+02, 1.5399265260595e+02,
    1.7782794100389e+02, 2.0535250264571e+02,
    2.3713737056617e+02, 2.7384196342644e+02,
    3.1622776601684e+02, 3.6517412725484e+02,
    4.2169650342858e+02, 4.8696752516586e+02,
    5.6234132519035e+02, 6.4938163157621e+02,
    7.4989420933246e+02, 8.6596432336007e+02,
    1.0000000000000e+03, 1.1547819846895e+03,
    1.3335214321633e+03, 1.5399265260595e+03,
    1.7782794100389e+03, 2.0535250264571e+03,
    2.3713737056617e+03, 2.7384196342644e+03,
    3.1622776601684e+03, 3.6517412725484e+03,
    4.2169650342858e+03, 4.8696752516586e+03,
    5.6234132519035e+03, 6.4938163157621e+03,
    7.4989420933246e+03, 8.6596432336007e+03,
    1.0000000000000e+04, 1.1547819846895e+04,
    1.3335214321633e+04, 1.5399265260595e+04,
    1.7782794100389e+04, 2.0535250264571e+04,
    2.3713737056617e+04, 2.7384196342644e+04,
    3.1622776601684e+04, 3.6517412725484e+04,
    4.2169650342858e+04, 4.8696752516586e+04,
    5.6234132519035e+04, 6.4938163157621e+04,
    7.4989420933246e+04, 8.6596432336007e+04,
    1.0000000000000e+05, 1.1547819846895e+05,
    1.3335214321633e+05, 1.5399265260595e+05,
    1.7782794100389e+05, 2.0535250264571e+05,
    2.3713737056617e+05, 2.7384196342644e+05,
    3.1622776601684e+05, 3.6517412725484e+05,
    4.2169650342858e+05, 4.8696752516586e+05,
    5.6234132519035e+05, 6.4938163157621e+05,
    7.4989420933246e+05, 8.6596432336007e+05,
};

/**
 * decode exponents coded with VLC codes
 */
static int decode_exp_vlc(WMACodecContext *s, int ch)
{
    int last_exp, n, code;
    const uint16_t *ptr;
    float v, max_scale;
    uint32_t *q, *q_end, iv;
    const float *ptab = pow_tab + 60;
    const uint32_t *iptab = reinterpret_cast<const uint32_t *>(ptab);

    ptr       = s->exponent_bands[s->frame_len_bits - s->block_len_bits];
    q         = reinterpret_cast<uint32_t *>(s->exponents[ch]);
    q_end     = q + s->block_len;
    max_scale = 0;
    if (s->version == 1) {
        last_exp  = get_bits(&s->gb, 5) + 10;
        v         = ptab[last_exp];
        iv        = iptab[last_exp];
        max_scale = v;
        n         = *ptr++;
        switch (n & 3) do {
        av_fallthrough;
        case 0: *q++ = iv; av_fallthrough;
        case 3: *q++ = iv; av_fallthrough;
        case 2: *q++ = iv; av_fallthrough;
        case 1: *q++ = iv;
        } while ((n -= 4) > 0);
    } else
        last_exp = 36;

    while (q < q_end) {
        code = get_vlc2(&s->gb, s->exp_vlc.table, EXPVLCBITS, EXPMAX);
        /* NOTE: this offset is the same as MPEG-4 AAC! */
        last_exp += code - 60;
        if ((unsigned) last_exp + 60 >= FF_ARRAY_ELEMS(pow_tab)) {
            av_log(s->avctx, AV_LOG_ERROR, "Exponent out of range: %d\n",
                   last_exp);
            return -1;
        }
        v  = ptab[last_exp];
        iv = iptab[last_exp];
        if (v > max_scale)
            max_scale = v;
        n = *ptr++;
        switch (n & 3) do {
        av_fallthrough;
        case 0: *q++ = iv; av_fallthrough;
        case 3: *q++ = iv; av_fallthrough;
        case 2: *q++ = iv; av_fallthrough;
        case 1: *q++ = iv;
        } while ((n -= 4) > 0);
    }
    s->max_exponent[ch] = max_scale;
    return 0;
}

/**
 * Apply MDCT window and add into output.
 *
 * We ensure that when the windows overlap their squared sum
 * is always 1 (MDCT reconstruction rule).
 */
static void wma_window(WMACodecContext *s, float *out)
{
    float *in = s->output;
    int block_len, bsize, n;

    /* left part */
    if (s->block_len_bits <= s->prev_block_len_bits) {
        block_len = s->block_len;
        bsize     = s->frame_len_bits - s->block_len_bits;

        s->fdsp->vector_fmul_add(out, in, s->windows[bsize],
                                out, block_len);
    } else {
        block_len = 1 << s->prev_block_len_bits;
        n         = (s->block_len - block_len) / 2;
        bsize     = s->frame_len_bits - s->prev_block_len_bits;

        s->fdsp->vector_fmul_add(out + n, in + n, s->windows[bsize],
                                out + n, block_len);

        memcpy(out + n + block_len, in + n + block_len, n * sizeof(float));
    }

    out += s->block_len;
    in  += s->block_len;

    /* right part */
    if (s->block_len_bits <= s->next_block_len_bits) {
        block_len = s->block_len;
        bsize     = s->frame_len_bits - s->block_len_bits;

        s->fdsp->vector_fmul_reverse(out, in, s->windows[bsize], block_len);
    } else {
        block_len = 1 << s->next_block_len_bits;
        n         = (s->block_len - block_len) / 2;
        bsize     = s->frame_len_bits - s->next_block_len_bits;

        memcpy(out, in, n * sizeof(float));

        s->fdsp->vector_fmul_reverse(out + n, in + n, s->windows[bsize],
                                    block_len);

        memset(out + n + block_len, 0, n * sizeof(float));
    }
}

/**
 * @return 0 if OK. 1 if last block of frame. return -1 if
 * unrecoverable error.
 */
static int wma_decode_block(WMACodecContext *s)
{
    int n, v, a, ch, bsize;
    int coef_nb_bits, total_gain;
    int nb_coefs[MAX_CHANNELS];
    float mdct_norm;
    FFTContext *mdct;

#ifdef TRACE
    ff_tlog(s->avctx, "***decode_block: %d:%d\n",
            s->frame_count - 1, s->block_num);
#endif /* TRACE */

    /* compute current block length */
    if (s->use_variable_block_len) {
        n = av_log2(s->nb_block_sizes - 1) + 1;

        if (s->reset_block_lengths) {
            s->reset_block_lengths = 0;
            v                      = get_bits(&s->gb, n);
            if (v >= s->nb_block_sizes) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "prev_block_len_bits %d out of range\n",
                       s->frame_len_bits - v);
                return -1;
            }
            s->prev_block_len_bits = s->frame_len_bits - v;
            v                      = get_bits(&s->gb, n);
            if (v >= s->nb_block_sizes) {
                av_log(s->avctx, AV_LOG_ERROR,
                       "block_len_bits %d out of range\n",
                       s->frame_len_bits - v);
                return -1;
            }
            s->block_len_bits = s->frame_len_bits - v;
        } else {
            /* update block lengths */
            s->prev_block_len_bits = s->block_len_bits;
            s->block_len_bits      = s->next_block_len_bits;
        }
        v = get_bits(&s->gb, n);
        if (v >= s->nb_block_sizes) {
            av_log(s->avctx, AV_LOG_ERROR,
                   "next_block_len_bits %d out of range\n",
                   s->frame_len_bits - v);
            return -1;
        }
        s->next_block_len_bits = s->frame_len_bits - v;
    } else {
        /* fixed block len */
        s->next_block_len_bits = s->frame_len_bits;
        s->prev_block_len_bits = s->frame_len_bits;
        s->block_len_bits      = s->frame_len_bits;
    }

    if (s->frame_len_bits - s->block_len_bits >= s->nb_block_sizes){
        av_log(s->avctx, AV_LOG_ERROR, "block_len_bits not initialized to a valid value\n");
        return -1;
    }

    /* now check if the block length is coherent with the frame length */
    s->block_len = 1 << s->block_len_bits;
    if ((s->block_pos + s->block_len) > s->frame_len) {
        av_log(s->avctx, AV_LOG_ERROR, "frame_len overflow\n");
        return -1;
    }

    if (s->avctx->channels == 2)
        s->ms_stereo = get_bits1(&s->gb);
    v = 0;
    for (ch = 0; ch < s->avctx->channels; ch++) {
        a                    = get_bits1(&s->gb);
        s->channel_coded[ch] = a;
        v                   |= a;
    }

    bsize = s->frame_len_bits - s->block_len_bits;

    /* if no channel coded, no need to go further */
    /* XXX: fix potential framing problems */
    if (!v)
        goto next;

    /* read total gain and extract corresponding number of bits for
     * coef escape coding */
    total_gain = 1;
    for (;;) {
        if (get_bits_left(&s->gb) < 7) {
            av_log(s->avctx, AV_LOG_ERROR, "total_gain overread\n");
            return AVERROR_INVALIDDATA;
        }
        a           = get_bits(&s->gb, 7);
        total_gain += a;
        if (a != 127)
            break;
    }

    coef_nb_bits = ff_wma_total_gain_to_bits(total_gain);

    /* compute number of coefficients */
    n = s->coefs_end[bsize] - s->coefs_start;
    for (ch = 0; ch < s->avctx->channels; ch++)
        nb_coefs[ch] = n;

    /* complex coding */
    if (s->use_noise_coding) {
        for (ch = 0; ch < s->avctx->channels; ch++) {
            if (s->channel_coded[ch]) {
                int i, n, a;
                n = s->exponent_high_sizes[bsize];
                for (i = 0; i < n; i++) {
                    a                         = get_bits1(&s->gb);
                    s->high_band_coded[ch][i] = a;
                    /* if noise coding, the coefficients are not transmitted */
                    if (a)
                        nb_coefs[ch] -= s->exponent_high_bands[bsize][i];
                }
            }
        }
        for (ch = 0; ch < s->avctx->channels; ch++) {
            if (s->channel_coded[ch]) {
                int i, n, val;

                n   = s->exponent_high_sizes[bsize];
                val = (int) 0x80000000;
                for (i = 0; i < n; i++) {
                    if (s->high_band_coded[ch][i]) {
                        if (val == (int) 0x80000000) {
                            val = get_bits(&s->gb, 7) - 19;
                        } else {
                            val += get_vlc2(&s->gb, s->hgain_vlc.table,
                                            HGAINVLCBITS, HGAINMAX);
                        }
                        s->high_band_values[ch][i] = val;
                    }
                }
            }
        }
    }

    /* exponents can be reused in short blocks. */
    if ((s->block_len_bits == s->frame_len_bits) || get_bits1(&s->gb)) {
        for (ch = 0; ch < s->avctx->channels; ch++) {
            if (s->channel_coded[ch]) {
                if (s->use_exp_vlc) {
                    if (decode_exp_vlc(s, ch) < 0)
                        return -1;
                } else {
                    decode_exp_lsp(s, ch);
                }
                s->exponents_bsize[ch] = bsize;
                s->exponents_initialized[ch] = 1;
            }
        }
    }

    for (ch = 0; ch < s->avctx->channels; ch++) {
        if (s->channel_coded[ch] && !s->exponents_initialized[ch])
            return AVERROR_INVALIDDATA;
    }

    /* parse spectral coefficients : just RLE encoding */
    for (ch = 0; ch < s->avctx->channels; ch++) {
        if (s->channel_coded[ch]) {
            int tindex;
            WMACoef *ptr = &s->coefs1[ch][0];
            int ret;

            /* special VLC tables are used for ms stereo because
             * there is potentially less energy there */
            tindex = (ch == 1 && s->ms_stereo);
            memset(ptr, 0, s->block_len * sizeof(WMACoef));
            ret = ff_wma_run_level_decode(s->avctx, &s->gb, &s->coef_vlc[tindex],
                                          s->level_table[tindex], s->run_table[tindex],
                                          0, ptr, 0, nb_coefs[ch],
                                          s->block_len, s->frame_len_bits, coef_nb_bits);
            if (ret < 0)
                return ret;
        }
        if (s->version == 1 && s->avctx->channels >= 2)
            align_get_bits(&s->gb);
    }

    /* normalize */
    {
        int n4 = s->block_len / 2;
        mdct_norm = 1.0 / (float) n4;
        if (s->version == 1)
            mdct_norm *= sqrt(n4);
    }

    /* finally compute the MDCT coefficients */
    for (ch = 0; ch < s->avctx->channels; ch++) {
        if (s->channel_coded[ch]) {
            WMACoef *coefs1;
            float *coefs, *exponents, mult, mult1, noise;
            int i, j, n, n1, last_high_band, esize;
            float exp_power[HIGH_BAND_MAX_SIZE];

            coefs1    = s->coefs1[ch];
            exponents = s->exponents[ch];
            esize     = s->exponents_bsize[ch];
            mult      = ff_exp10(total_gain * 0.05) / s->max_exponent[ch];
            mult     *= mdct_norm;
            coefs     = s->coefs[ch];
            if (s->use_noise_coding) {
                mult1 = mult;
                /* very low freqs : noise */
                for (i = 0; i < s->coefs_start; i++) {
                    *coefs++ = s->noise_table[s->noise_index] *
                               exponents[i << bsize >> esize] * mult1;
                    s->noise_index = (s->noise_index + 1) &
                                     (NOISE_TAB_SIZE - 1);
                }

                n1 = s->exponent_high_sizes[bsize];

                /* compute power of high bands */
                exponents = s->exponents[ch] +
                            (s->high_band_start[bsize] << bsize >> esize);
                last_high_band = 0; /* avoid warning */
                for (j = 0; j < n1; j++) {
                    n = s->exponent_high_bands[s->frame_len_bits -
                                               s->block_len_bits][j];
                    if (s->high_band_coded[ch][j]) {
                        float e2, v;
                        e2 = 0;
                        for (i = 0; i < n; i++) {
                            v   = exponents[i << bsize >> esize];
                            e2 += v * v;
                        }
                        exp_power[j]   = e2 / n;
                        last_high_band = j;
                        ff_tlog(s->avctx, "%d: power=%f (%d)\n", j, exp_power[j], n);
                    }
                    exponents += n << bsize >> esize;
                }

                /* main freqs and high freqs */
                exponents = s->exponents[ch] + (s->coefs_start << bsize >> esize);
                for (j = -1; j < n1; j++) {
                    if (j < 0)
                        n = s->high_band_start[bsize] - s->coefs_start;
                    else
                        n = s->exponent_high_bands[s->frame_len_bits -
                                                   s->block_len_bits][j];
                    if (j >= 0 && s->high_band_coded[ch][j]) {
                        /* use noise with specified power */
                        mult1 = sqrt(exp_power[j] / exp_power[last_high_band]);
                        /* XXX: use a table */
                        mult1  = mult1 * ff_exp10(s->high_band_values[ch][j] * 0.05);
                        mult1  = mult1 / (s->max_exponent[ch] * s->noise_mult);
                        mult1 *= mdct_norm;
                        for (i = 0; i < n; i++) {
                            noise          = s->noise_table[s->noise_index];
                            s->noise_index = (s->noise_index + 1) & (NOISE_TAB_SIZE - 1);
                            *coefs++       = noise * exponents[i << bsize >> esize] * mult1;
                        }
                        exponents += n << bsize >> esize;
                    } else {
                        /* coded values + small noise */
                        for (i = 0; i < n; i++) {
                            noise          = s->noise_table[s->noise_index];
                            s->noise_index = (s->noise_index + 1) & (NOISE_TAB_SIZE - 1);
                            *coefs++       = ((*coefs1++) + noise) *
                                             exponents[i << bsize >> esize] * mult;
                        }
                        exponents += n << bsize >> esize;
                    }
                }

                /* very high freqs : noise */
                n     = s->block_len - s->coefs_end[bsize];
                mult1 = mult * exponents[(-(1 << bsize)) >> esize];
                for (i = 0; i < n; i++) {
                    *coefs++       = s->noise_table[s->noise_index] * mult1;
                    s->noise_index = (s->noise_index + 1) & (NOISE_TAB_SIZE - 1);
                }
            } else {
                /* XXX: optimize more */
                for (i = 0; i < s->coefs_start; i++)
                    *coefs++ = 0.0;
                n = nb_coefs[ch];
                for (i = 0; i < n; i++)
                    *coefs++ = coefs1[i] * exponents[i << bsize >> esize] * mult;
                n = s->block_len - s->coefs_end[bsize];
                for (i = 0; i < n; i++)
                    *coefs++ = 0.0;
            }
        }
    }

#ifdef TRACE
    for (ch = 0; ch < s->avctx->channels; ch++) {
        if (s->channel_coded[ch]) {
            dump_floats(s, "exponents", 3, s->exponents[ch], s->block_len);
            dump_floats(s, "coefs", 1, s->coefs[ch], s->block_len);
        }
    }
#endif /* TRACE */

    if (s->ms_stereo && s->channel_coded[1]) {
        /* nominal case for ms stereo: we do it before mdct */
        /* no need to optimize this case because it should almost
         * never happen */
        if (!s->channel_coded[0]) {
            ff_tlog(s->avctx, "rare ms-stereo case happened\n");
            memset(s->coefs[0], 0, sizeof(float) * s->block_len);
            s->channel_coded[0] = 1;
        }

        s->fdsp->butterflies_float(s->coefs[0], s->coefs[1], s->block_len);
    }

next:
    mdct = &s->mdct_ctx[bsize];

    for (ch = 0; ch < s->avctx->channels; ch++) {
        int n4, index;

        n4 = s->block_len / 2;
        if (s->channel_coded[ch])
            mdct->imdct_calc(mdct, s->output, s->coefs[ch]);
        else if (!(s->ms_stereo && ch == 1))
            memset(s->output, 0, sizeof(s->output));

        /* multiply by the window and add in the frame */
        index = (s->frame_len / 2) + s->block_pos - n4;
        wma_window(s, &s->frame_out[ch][index]);
    }

    /* update block number */
    s->block_num++;
    s->block_pos += s->block_len;
    if (s->block_pos >= s->frame_len)
        return 1;
    else
        return 0;
}

/* decode a frame of frame_len samples */
static int wma_decode_frame(WMACodecContext *s, float **samples,
                            int samples_offset)
{
    int ret, ch;

#ifdef TRACE
    ff_tlog(s->avctx, "***decode_frame: %d size=%d\n",
            s->frame_count++, s->frame_len);
#endif /* TRACE */

    /* read each block */
    s->block_num = 0;
    s->block_pos = 0;
    for (;;) {
        ret = wma_decode_block(s);
        if (ret < 0)
            return -1;
        if (ret)
            break;
    }

    for (ch = 0; ch < s->avctx->channels; ch++) {
        /* copy current block to output */
        memcpy(samples[ch] + samples_offset, s->frame_out[ch],
               s->frame_len * sizeof(*s->frame_out[ch]));
        /* prepare for next block */
        memmove(&s->frame_out[ch][0], &s->frame_out[ch][s->frame_len],
                s->frame_len * sizeof(*s->frame_out[ch]));

#ifdef TRACE
        dump_floats(s, "samples", 6, samples[ch] + samples_offset,
                    s->frame_len);
#endif /* TRACE */
    }

    return 0;
}

static int wma_decode_superframe(AVCodecContext *avctx, void *data,
                                 int *got_frame_ptr, AVPacket *avpkt)
{
    AVFrame *frame = static_cast<AVFrame *>(data);
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    WMACodecContext *s = static_cast<WMACodecContext *>(avctx->priv_data);
    int nb_frames, bit_offset, i, pos, len, ret;
    uint8_t *q;
    float **samples;
    int samples_offset;

    ff_tlog(avctx, "***decode_superframe:\n");

    /* Backported from FFmpeg master: n4.4.4 simply drops the trailing MDCT
     * overlap, losing the last frame_len samples of every file. On EOF the
     * caller passes an empty packet and the tail in frame_out[] is emitted. */
    if (buf_size == 0) {
        if (s->eof_done)
            return 0;

        frame->nb_samples = s->frame_len;
        if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
            return ret;

        for (int i = 0; i < avctx->channels; i++)
            memcpy(frame->extended_data[i], &s->frame_out[i][0],
                   frame->nb_samples * sizeof(s->frame_out[i][0]));

        s->last_superframe_len = 0;
        s->eof_done = 1;
        *got_frame_ptr = 1;
        return 0;
    }
    if (buf_size < avctx->block_align) {
        av_log(avctx, AV_LOG_ERROR,
               "Input packet size too small (%d < %d)\n",
               buf_size, avctx->block_align);
        return AVERROR_INVALIDDATA;
    }
    if (avctx->block_align)
        buf_size = avctx->block_align;

    init_get_bits(&s->gb, buf, buf_size * 8);

    if (s->use_bit_reservoir) {
        /* read super frame header */
        skip_bits(&s->gb, 4); /* super frame index */
        nb_frames = get_bits(&s->gb, 4) - (s->last_superframe_len <= 0);
        if (nb_frames <= 0) {
            int is_error = nb_frames < 0 || get_bits_left(&s->gb) <= 8;
            av_log(avctx, is_error ? AV_LOG_ERROR : AV_LOG_WARNING,
                   "nb_frames is %d bits left %d\n",
                   nb_frames, get_bits_left(&s->gb));
            if (is_error)
                return AVERROR_INVALIDDATA;

            if ((s->last_superframe_len + buf_size - 1) >
                MAX_CODED_SUPERFRAME_SIZE)
                goto fail;

            q   = s->last_superframe + s->last_superframe_len;
            len = buf_size - 1;
            while (len > 0) {
                *q++ = get_bits (&s->gb, 8);
                len --;
            }
            memset(q, 0, AV_INPUT_BUFFER_PADDING_SIZE);

            s->last_superframe_len += 8*buf_size - 8;
//             s->reset_block_lengths = 1; //XXX is this needed ?
            *got_frame_ptr = 0;
            return buf_size;
        }
    } else
        nb_frames = 1;

    /* get output buffer */
    frame->nb_samples = nb_frames * s->frame_len;
    if ((ret = ff_get_buffer(avctx, frame, 0)) < 0)
        return ret;
    samples        = frame->extended_data;
    samples_offset = 0;

    if (s->use_bit_reservoir) {
        bit_offset = get_bits(&s->gb, s->byte_offset_bits + 3);
        if (bit_offset > get_bits_left(&s->gb)) {
            av_log(avctx, AV_LOG_ERROR,
                   "Invalid last frame bit offset %d > buf size %d (%d)\n",
                   bit_offset, get_bits_left(&s->gb), buf_size);
            goto fail;
        }

        if (s->last_superframe_len > 0) {
            /* add bit_offset bits to last frame */
            if ((s->last_superframe_len + ((bit_offset + 7) >> 3)) >
                MAX_CODED_SUPERFRAME_SIZE)
                goto fail;
            q   = s->last_superframe + s->last_superframe_len;
            len = bit_offset;
            while (len > 7) {
                *q++ = get_bits(&s->gb, 8);
                len -= 8;
            }
            if (len > 0)
                *q++ = get_bits(&s->gb, len) << (8 - len);
            memset(q, 0, AV_INPUT_BUFFER_PADDING_SIZE);

            /* XXX: bit_offset bits into last frame */
            init_get_bits(&s->gb, s->last_superframe,
                          s->last_superframe_len * 8 + bit_offset);
            /* skip unused bits */
            if (s->last_bitoffset > 0)
                skip_bits(&s->gb, s->last_bitoffset);
            /* this frame is stored in the last superframe and in the
             * current one */
            if (wma_decode_frame(s, samples, samples_offset) < 0)
                goto fail;
            samples_offset += s->frame_len;
            nb_frames--;
        }

        /* read each frame starting from bit_offset */
        pos = bit_offset + 4 + 4 + s->byte_offset_bits + 3;
        if (pos >= MAX_CODED_SUPERFRAME_SIZE * 8 || pos > buf_size * 8)
            return AVERROR_INVALIDDATA;
        init_get_bits(&s->gb, buf + (pos >> 3), (buf_size - (pos >> 3)) * 8);
        len = pos & 7;
        if (len > 0)
            skip_bits(&s->gb, len);

        s->reset_block_lengths = 1;
        for (i = 0; i < nb_frames; i++) {
            if (wma_decode_frame(s, samples, samples_offset) < 0)
                goto fail;
            samples_offset += s->frame_len;
        }

        /* we copy the end of the frame in the last frame buffer */
        pos               = get_bits_count(&s->gb) +
                            ((bit_offset + 4 + 4 + s->byte_offset_bits + 3) & ~7);
        s->last_bitoffset = pos & 7;
        pos             >>= 3;
        len               = buf_size - pos;
        if (len > MAX_CODED_SUPERFRAME_SIZE || len < 0) {
            av_log(s->avctx, AV_LOG_ERROR, "len %d invalid\n", len);
            goto fail;
        }
        s->last_superframe_len = len;
        memcpy(s->last_superframe, buf + pos, len);
    } else {
        /* single frame decode */
        if (wma_decode_frame(s, samples, samples_offset) < 0)
            goto fail;
        samples_offset += s->frame_len;
    }

    ff_dlog(s->avctx, "%d %d %d %d outbytes:%"PTRDIFF_SPECIFIER" eaten:%d\n",
            s->frame_len_bits, s->block_len_bits, s->frame_len, s->block_len,
            reinterpret_cast<int8_t *>(samples) - static_cast<int8_t *>(data),
            avctx->block_align);

    *got_frame_ptr = 1;

    return buf_size;

fail:
    /* when error, we reset the bit reservoir */
    s->last_superframe_len = 0;
    return -1;
}

static av_cold void flush(AVCodecContext *avctx)
{
    WMACodecContext *s = static_cast<WMACodecContext *>(avctx->priv_data);

    s->last_bitoffset      =
    s->last_superframe_len = 0;
    s->eof_done            = 0;
}



/* ---- public entry points ---- */

int wma_dec_init(AVCodecContext *avctx)
{
    return wma_decode_init(avctx);
}

int wma_dec_decode(AVCodecContext *avctx, AVFrame *frame, int *got_frame,
                   const uint8_t *buf, int size)
{
    AVPacket pkt;
    pkt.data = buf;
    pkt.size = size;
    *got_frame = 0;
    return wma_decode_superframe(avctx, frame, got_frame, &pkt);
}

void wma_dec_flush(AVCodecContext *avctx)
{
    flush(avctx);
}

int wma_dec_close(AVCodecContext *avctx)
{
    return ff_wma_end(avctx);
}

} // namespace wma_ffmpeg

/* ---------------------------------------------------------------------
 * Public interface
 * ------------------------------------------------------------------ */

/* The corresponding names inside wma_ffmpeg are macros carried over from
 * FFmpeg, so they cannot be namespace-qualified at the call site. */
#define WMA_MAX_CHANNELS MAX_CHANNELS
#define WMA_CODEC_ID_V1  AV_CODEC_ID_WMAV1
#define WMA_CODEC_ID_V2  AV_CODEC_ID_WMAV2

namespace wma_ffmpeg {

void wma_log(int level, const char *fmt, ...)
{
  if (level > AV_LOG_WARNING)
    return;

  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  /* FFmpeg's messages carry their own newline; mocf's logger adds one. */
  size_t n = strlen(msg);
  while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r'))
    msg[--n] = '\0';

  logit("wma: %s", msg);
}

} // namespace wma_ffmpeg

struct WmaDecoderCore
{
  wma_ffmpeg::WMACodecContext wma{};
  wma_ffmpeg::AVCodecContext ctx{};
  wma_ffmpeg::AVCodecShim codec{};
  std::vector<uint8_t> extradata;
  float *planes[2] = { nullptr, nullptr };
};

void WmaCoreDeleter::operator()(WmaDecoderCore *core) const
{
  if (core)
  {
    wma_ffmpeg::wma_dec_close(&core->ctx);
    delete core;
  }
}

unique_wma_core wma_core_create(const WmaStreamParams &params)
{
  if (params.version != 1 && params.version != 2)
    return nullptr;
  /* WMA v1/v2 is mono or stereo only; the decoder's arrays are sized for two. */
  if (params.channels < 1 || params.channels > WMA_MAX_CHANNELS)
    return nullptr;
  if (params.sample_rate <= 0 || params.block_align <= 0)
    return nullptr;

  auto core = std::unique_ptr<WmaDecoderCore, WmaCoreDeleter>(new WmaDecoderCore);

  if (params.extradata_size > 0 && params.extradata)
  {
    core->extradata.assign(params.extradata,
                           params.extradata + params.extradata_size);
  }

  core->codec.id = (params.version == 1) ? WMA_CODEC_ID_V1
                                         : WMA_CODEC_ID_V2;
  core->ctx.codec          = &core->codec;
  core->ctx.channels       = params.channels;
  core->ctx.sample_rate    = params.sample_rate;
  core->ctx.block_align    = params.block_align;
  core->ctx.bit_rate       = params.bit_rate;
  core->ctx.extradata      = core->extradata.data();
  core->ctx.extradata_size = static_cast<int>(core->extradata.size());
  core->ctx.priv_data      = &core->wma;

  if (wma_ffmpeg::wma_dec_init(&core->ctx) < 0)
  {
    /* wma_dec_init() may have allocated before failing; the deleter would run
     * on scope exit anyway, but close explicitly so the intent is plain. */
    return nullptr;
  }

  return core;
}

int wma_core_decode(WmaDecoderCore *core, const uint8_t *pkt, int size,
                    float *const **planes, int *nb_samples)
{
  *nb_samples = 0;

  wma_ffmpeg::AVFrame frame;
  memset(&frame, 0, sizeof(frame));

  int got = 0;
  const int ret = wma_ffmpeg::wma_dec_decode(&core->ctx, &frame, &got, pkt, size);
  if (ret < 0)
    return ret;
  if (!got)
    return 0;

  for (int c = 0; c < core->ctx.channels; c++)
    core->planes[c] = frame.extended_data[c];

  *planes = core->planes;
  *nb_samples = frame.nb_samples;
  return 0;
}

void wma_core_flush(WmaDecoderCore *core)
{
  wma_ffmpeg::wma_dec_flush(&core->ctx);
}

int wma_core_frame_len(const WmaDecoderCore *core)
{
  return core->wma.frame_len;
}
