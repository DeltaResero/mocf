// src/audio/decoders/qoa/qoa.h
// SPDX-License-Identifier: MIT
//
// mocf - Music on Console Framebuffer
// Decode-only derivative of https://github.com/phoboslab/qoa (spec
// v1.0, 2023-04-24). The encoder half of the upstream single-file
// library has been removed: mocf is a player and never encodes.
//
// This file is MIT-licensed (mocf as a whole is GPL-3.0-or-later;
// see qoa.cpp and qoa_impl.c, which use this file). Everything from
// the next "/*" onward is upstream, with the encoder removed.

/*

Copyright (c) 2023, Dominic Szablewski - https://phoboslab.org
SPDX-License-Identifier: MIT

QOA - The "Quite OK Audio" format for fast, lossy audio compression


-- Data Format

QOA encodes pulse-code modulated (PCM) audio data with up to 255 channels, 
sample rates from 1 up to 16777215 hertz and a bit depth of 16 bits.

A QOA file consists of an 8 byte file header, followed by a number of frames.
Each frame contains an 8 byte frame header, the current 16 byte en-/decoder
state per channel and 256 slices per channel. Each slice is 8 bytes wide and
encodes 20 samples of audio data.

All values, including the slices, are big endian. The file layout is as follows:

struct {
	struct {
		char     magic[4];         // magic bytes "qoaf"
		uint32_t samples;          // samples per channel in this file
	} file_header;

	struct {
		struct {
			uint8_t  num_channels; // no. of channels
			uint24_t samplerate;   // samplerate in hz
			uint16_t fsamples;     // samples per channel in this frame
			uint16_t fsize;        // frame size (includes this header)
		} frame_header;

		struct {
			int16_t history[4];    // most recent last
			int16_t weights[4];    // most recent last
		} lms_state[num_channels];

		qoa_slice_t slices[256][num_channels];

	} frames[ceil(samples / (256 * 20))];
} qoa_file_t;

Each `qoa_slice_t` contains a quantized scalefactor `sf_quant` and 20 quantized
residuals `qrNN`:

.- QOA_SLICE -- 64 bits, 20 samples --------------------------/  /------------.
|        Byte[0]         |        Byte[1]         |  Byte[2]  \  \  Byte[7]   |
| 7  6  5  4  3  2  1  0 | 7  6  5  4  3  2  1  0 | 7  6  5   /  /    2  1  0 |
|------------+--------+--------+--------+---------+---------+-\  \--+---------|
|  sf_quant  |  qr00  |  qr01  |  qr02  |  qr03   |  qr04   | /  /  |  qr19   |
`-------------------------------------------------------------\  \------------`

Each frame except the last must contain exactly 256 slices per channel. The last
frame may contain between 1 .. 256 (inclusive) slices per channel. The last
slice (for each channel) in the last frame may contain less than 20 samples; the
slice still must be 8 bytes wide, with the unused samples zeroed out.

Channels are interleaved per slice. E.g. for 2 channel stereo:
slice[0] = L, slice[1] = R, slice[2] = L, slice[3] = R ...

A valid QOA file or stream must have at least one frame. Each frame must contain
at least one channel and one sample with a samplerate between 1 .. 16777215
(inclusive).

If the total number of samples is not known by the encoder, the samples in the
file header may be set to 0x00000000 to indicate that the encoder is
"streaming". In a streaming context, the samplerate and number of channels may
differ from frame to frame. For static files (those with samples set to a
non-zero value), each frame must have the same number of channels and same
samplerate.

Note that this implementation of QOA only handles files with a known total
number of samples.

A decoder should support at least 8 channels. The channel layout for channel
counts 1 .. 8 is:

	1. Mono
	2. L, R
	3. L, R, C
	4. FL, FR, B/SL, B/SR
	5. FL, FR, C, B/SL, B/SR
	6. FL, FR, C, LFE, B/SL, B/SR
	7. FL, FR, C, LFE, B, SL, SR
	8. FL, FR, C, LFE, BL, BR, SL, SR

QOA predicts each audio sample based on the previously decoded ones using a
"Sign-Sign Least Mean Squares Filter" (LMS). This prediction plus the
dequantized residual forms the final output sample.

*/



/* -----------------------------------------------------------------------------
	Header - Public functions */

#ifndef QOA_H
#define QOA_H

#ifdef __cplusplus
extern "C" {
#endif

#define QOA_MIN_FILESIZE 16
#define QOA_MAX_CHANNELS 8

#define QOA_SLICE_LEN 20
#define QOA_SLICES_PER_FRAME 256
#define QOA_FRAME_LEN (QOA_SLICES_PER_FRAME * QOA_SLICE_LEN)
#define QOA_LMS_LEN 4
#define QOA_MAGIC 0x716f6166 /* 'qoaf' */

#define QOA_FRAME_SIZE(channels, slices) \
	(8 + QOA_LMS_LEN * 4 * (channels) + 8 * (slices) * (channels))

typedef struct {
	int history[QOA_LMS_LEN];
	int weights[QOA_LMS_LEN];
} qoa_lms_t;

typedef struct {
	unsigned int channels;
	unsigned int samplerate;
	unsigned int samples;
	qoa_lms_t lms[QOA_MAX_CHANNELS];
	#ifdef QOA_RECORD_TOTAL_ERROR
		double error;
	#endif
} qoa_desc;

unsigned int qoa_decode_header(const unsigned char *bytes, int size, qoa_desc *qoa);
unsigned int qoa_decode_frame(const unsigned char *bytes, unsigned int size, qoa_desc *qoa, short *sample_data, unsigned int *frame_len);
short *qoa_decode(const unsigned char *bytes, int size, qoa_desc *file);


#ifdef __cplusplus
}
#endif
#endif /* QOA_H */


/* -----------------------------------------------------------------------------
	Implementation */

#ifdef QOA_IMPLEMENTATION
#include <stdlib.h>

#ifndef QOA_MALLOC
	#define QOA_MALLOC(sz) malloc(sz)
	#define QOA_FREE(p) free(p)
#endif

typedef unsigned long long qoa_uint64_t;


/* The dequant_tab maps each of the scalefactors and quantized residuals to
their unscaled & dequantized version.

Since qoa_div rounds away from the zero, the smallest entries are mapped to 3/4
instead of 1. The dequant_tab assumes the following dequantized values for each
of the quant_tab indices and is computed as:
float dqt[8] = {0.75, -0.75, 2.5, -2.5, 4.5, -4.5, 7, -7};
dequant_tab[s][q] <- round_ties_away_from_zero(scalefactor_tab[s] * dqt[q])

The rounding employed here is "to nearest, ties away from zero",  i.e. positive
and negative values are treated symmetrically.
*/

static const int qoa_dequant_tab[16][8] = {
	{   1,    -1,    3,    -3,    5,    -5,     7,     -7},
	{   5,    -5,   18,   -18,   32,   -32,    49,    -49},
	{  16,   -16,   53,   -53,   95,   -95,   147,   -147},
	{  34,   -34,  113,  -113,  203,  -203,   315,   -315},
	{  63,   -63,  210,  -210,  378,  -378,   588,   -588},
	{ 104,  -104,  345,  -345,  621,  -621,   966,   -966},
	{ 158,  -158,  528,  -528,  950,  -950,  1477,  -1477},
	{ 228,  -228,  760,  -760, 1368, -1368,  2128,  -2128},
	{ 316,  -316, 1053, -1053, 1895, -1895,  2947,  -2947},
	{ 422,  -422, 1405, -1405, 2529, -2529,  3934,  -3934},
	{ 548,  -548, 1828, -1828, 3290, -3290,  5117,  -5117},
	{ 696,  -696, 2320, -2320, 4176, -4176,  6496,  -6496},
	{ 868,  -868, 2893, -2893, 5207, -5207,  8099,  -8099},
	{1064, -1064, 3548, -3548, 6386, -6386,  9933,  -9933},
	{1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
	{1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};


/* The Least Mean Squares Filter is the heart of QOA. It predicts the next
sample based on the previous 4 reconstructed samples. It does so by continuously
adjusting 4 weights based on the residual of the previous prediction.

The next sample is predicted as the sum of (weight[i] * history[i]).

The adjustment of the weights is done with a "Sign-Sign-LMS" that adds or
subtracts the residual to each weight, based on the corresponding sample from
the history. This, surprisingly, is sufficient to get worthwhile predictions.

This is all done with fixed point integers. Hence the right-shifts when updating
the weights and calculating the prediction. */

static int qoa_lms_predict(qoa_lms_t *lms) {
	int prediction = 0;
	for (int i = 0; i < QOA_LMS_LEN; i++) {
		/* Accumulate in unsigned: a crafted file can drive weight *
		 * history past INT_MAX, which is undefined for signed ints.
		 * Unsigned wraparound is defined and the >>13 result matches.
		 * (Backported from FFmpeg's qoadec.c, oss-fuzz 62285.) */
		prediction += ((unsigned)lms->weights[i] * lms->history[i]);
	}
	return prediction >> 13;
}

static void qoa_lms_update(qoa_lms_t *lms, int sample, int residual) {
	int delta = residual >> 4;
	for (int i = 0; i < QOA_LMS_LEN; i++) {
		lms->weights[i] += lms->history[i] < 0 ? -delta : delta;
	}

	for (int i = 0; i < QOA_LMS_LEN-1; i++) {
		lms->history[i] = lms->history[i+1];
	}
	lms->history[QOA_LMS_LEN-1] = sample;
}


static inline int qoa_clamp(int v, int min, int max) {
	if (v < min) { return min; }
	if (v > max) { return max; }
	return v;
}

/* This specialized clamp function for the signed 16 bit range improves decode
performance quite a bit. The extra if() statement works nicely with the CPUs
branch prediction as this branch is rarely taken. */

static inline int qoa_clamp_s16(int v) {
	if ((unsigned int)(v + 32768) > 65535) {
		if (v < -32768) { return -32768; }
		if (v >  32767) { return  32767; }
	}
	return v;
}

static inline qoa_uint64_t qoa_read_u64(const unsigned char *bytes, unsigned int *p) {
	bytes += *p;
	*p += 8;
	return 
		((qoa_uint64_t)(bytes[0]) << 56) | ((qoa_uint64_t)(bytes[1]) << 48) |
		((qoa_uint64_t)(bytes[2]) << 40) | ((qoa_uint64_t)(bytes[3]) << 32) |
		((qoa_uint64_t)(bytes[4]) << 24) | ((qoa_uint64_t)(bytes[5]) << 16) |
		((qoa_uint64_t)(bytes[6]) <<  8) | ((qoa_uint64_t)(bytes[7]) <<  0);
}

/* -----------------------------------------------------------------------------
	Decoder */

unsigned int qoa_decode_header(const unsigned char *bytes, int size, qoa_desc *qoa) {
	unsigned int p = 0;
	if (size < QOA_MIN_FILESIZE) {
		return 0;
	}


	/* Read the file header, verify the magic number ('qoaf') and read the 
	total number of samples. */
	qoa_uint64_t file_header = qoa_read_u64(bytes, &p);

	if ((file_header >> 32) != QOA_MAGIC) {
		return 0;
	}

	qoa->samples = file_header & 0xffffffff;
	if (!qoa->samples) {
		return 0;
	}

	/* Peek into the first frame header to get the number of channels and
	the samplerate. */
	qoa_uint64_t frame_header = qoa_read_u64(bytes, &p);
	qoa->channels   = (frame_header >> 56) & 0x0000ff;
	qoa->samplerate = (frame_header >> 32) & 0xffffff;

	if (
		qoa->channels == 0 || qoa->samples == 0 || qoa->samplerate == 0 ||
		qoa->channels > QOA_MAX_CHANNELS
	) {
		return 0;
	}

	return 8;
}

unsigned int qoa_decode_frame(const unsigned char *bytes, unsigned int size, qoa_desc *qoa, short *sample_data, unsigned int *frame_len) {
	unsigned int p = 0;
	if (frame_len) {
		*frame_len = 0;
	}

	if (size < 8 + QOA_LMS_LEN * 4 * qoa->channels) {
		return 0;
	}

	/* Read and verify the frame header */
	qoa_uint64_t frame_header = qoa_read_u64(bytes, &p);
	unsigned int channels   = (frame_header >> 56) & 0x0000ff;
	unsigned int samplerate = (frame_header >> 32) & 0xffffff;
	unsigned int samples    = (frame_header >> 16) & 0x00ffff;
	unsigned int frame_size = (frame_header      ) & 0x00ffff;

	unsigned int header_size = 8 + (QOA_LMS_LEN * 4 * channels);
	unsigned int data_size = frame_size - header_size;
	unsigned int max_total_slices = data_size / 8;
	unsigned int num_slices = (samples + QOA_SLICE_LEN - 1) / QOA_SLICE_LEN;
	
	if (
		channels != qoa->channels || 
		samplerate != qoa->samplerate ||
		frame_size < header_size ||
		frame_size > size ||
		num_slices > QOA_SLICES_PER_FRAME ||
		(num_slices * channels) > max_total_slices
	) {
		return 0;
	}


	/* Read the LMS state: 4 x 2 bytes history, 4 x 2 bytes weights per channel */
	for (unsigned int c = 0; c < channels; c++) {
		qoa_uint64_t history = qoa_read_u64(bytes, &p);
		qoa_uint64_t weights = qoa_read_u64(bytes, &p);
		for (int i = 0; i < QOA_LMS_LEN; i++) {
			qoa->lms[c].history[i] = ((signed short)(history >> 48));
			history <<= 16;
			qoa->lms[c].weights[i] = ((signed short)(weights >> 48));
			weights <<= 16;
		}
	}


	/* Decode all slices for all channels in this frame */
	for (unsigned int sample_index = 0; sample_index < samples; sample_index += QOA_SLICE_LEN) {
		for (unsigned int c = 0; c < channels; c++) {
			qoa_uint64_t slice = qoa_read_u64(bytes, &p);

			int scalefactor = (slice >> 60) & 0xf;
			slice <<= 4;

			int slice_start = (sample_index * channels) + c;
			int slice_end = (qoa_clamp(sample_index + QOA_SLICE_LEN, 0, samples) * channels) + c;

			for (int si = slice_start; si < slice_end; si += channels) {
				int predicted = qoa_lms_predict(&qoa->lms[c]);
				int quantized = (slice >> 61) & 0x7;
				int dequantized = qoa_dequant_tab[scalefactor][quantized];
				int reconstructed = qoa_clamp_s16(predicted + dequantized);
				
				sample_data[si] = reconstructed;
				slice <<= 3;

				qoa_lms_update(&qoa->lms[c], reconstructed, dequantized);
			}
		}
	}

	if (frame_len) {
		*frame_len = samples;
	}
	return p;
}

short *qoa_decode(const unsigned char *bytes, int size, qoa_desc *qoa) {
	unsigned int p = qoa_decode_header(bytes, size, qoa);
	if (!p) {
		return NULL;
	}

	/* Calculate the required size of the sample buffer and allocate, round up to full frames */
	unsigned long long num_frames = ((unsigned long long)qoa->samples + QOA_FRAME_LEN - 1) / QOA_FRAME_LEN;
	unsigned long long total_samples_ull = num_frames * QOA_FRAME_LEN * (unsigned long long)qoa->channels;

	if (total_samples_ull > 0x7fffffff) { return NULL; }

	unsigned int total_samples = (unsigned int)total_samples_ull;
	short *sample_data = QOA_MALLOC(total_samples * sizeof(short));
	if (!sample_data) { return NULL; }

	unsigned int sample_index = 0;
	unsigned int frame_len;
	unsigned int frame_size;

	/* Decode all frames */
	do {
		short *sample_ptr = sample_data + sample_index * qoa->channels;
		frame_size = qoa_decode_frame(bytes + p, size - p, qoa, sample_ptr, &frame_len);

		p += frame_size;
		sample_index += frame_len;
	} while (frame_size && sample_index < qoa->samples);

	qoa->samples = sample_index;
	return sample_data;
}



#endif /* QOA_IMPLEMENTATION */
