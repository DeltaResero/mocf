// src/audio/decoders/sidplayfp/sidplayfp.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// This code is based on the original MOC sidplay2 plugin
// Copyright (C) 2004 Damian Pietras <daper@daper.net>
// Copyright (C) 2007 Hendrik Iben <hiben@tzi.de>
// Copyright (C) 2026 DeltaResero <deltaresero@zoho.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <pthread.h>
#include <cstring>
#include <strings.h>
#include <algorithm>
#include <memory>

#include "core/common.h"
#include "audio/decoders/sidplayfp/sidplayfp.h"
#include "core/log.h"
#include "core/options.h"

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

static std::unique_ptr<SidDatabase> database;
static int              init_db;
static int              defaultLength; // seconds
static int              minLength;     // seconds
static bool             startAtStart;
static bool             playSubTunes;

static pthread_mutex_t  db_mtx;

// ---------------------------------------------------------------------------
// Database helpers
// ---------------------------------------------------------------------------

static void init_database()
{
    int cancel = 0;

    pthread_mutex_lock(&db_mtx);
    if (init_db == 0)
        cancel = 1;
    init_db = 0;
    pthread_mutex_unlock(&db_mtx);

    if (cancel)
        return;

    const char *dbfile = options_get_str(OPT_DATABASE);
    if (dbfile == nullptr || dbfile[0] == '\0')
        return;

    database = std::make_unique<SidDatabase>();
    if (!database->open(dbfile)) {
        logit("sidplayfp: Unable to open SidDatabase \"%s\": %s",
              dbfile, database->error());
        database.reset();
    }
}

// Return the length of the currently-selected song in milliseconds,
// or -1 if unavailable. Tries the newer ms-precision API first, then
// falls back to the older seconds-based one (pre-MD5 databases).
static int song_length_ms(SidTune &tune)
{
    if (database == nullptr)
        return -1;

    int_least32_t ms = database->lengthMs(tune);
    if (ms >= 0)
        return static_cast<int>(ms);

    int_least32_t s = database->length(tune);
    if (s >= 0)
        return (s * 1000);

    return -1;
}

// ---------------------------------------------------------------------------
// Engine / builder construction
// ---------------------------------------------------------------------------

// Cycles to run the emulation per step. ~5ms of C64 time at ~1MHz; small
// enough to keep latency reasonable, large enough to avoid excessive call
// overhead. Matches the value used in libsidplayfp's own reference demo.
static constexpr unsigned int SIDPLAYFP_CYCLES = 5000;

// Create a SIDLiteBuilder. SIDLiteBuilder's constructor cannot fail
// visibly (no getStatus()/create(n) in the current API. SID chip
// emulation objects are allocated lazily by the engine as tunes are
// loaded), so this just centralizes construction for readability.
static std::unique_ptr<SIDLiteBuilder> make_builder()
{
    return std::make_unique<SIDLiteBuilder>("SIDLite");
}

// Create and configure a sidplayfp engine.  Returns nullptr on failure.
static std::unique_ptr<sidplayfp> make_engine(SIDLiteBuilder *builder, int frequency)
{
    auto engine = std::make_unique<sidplayfp>();

    SidConfig cfg    = engine->config();
    cfg.frequency    = static_cast<uint_least32_t>(frequency);
    cfg.sidEmulation = builder;

    // Honour OPT_SID_MODEL: 0 = auto (trust tune header),
    //                        1 = force 6581,
    //                        2 = force 8580.
    int model_pref = options_get_int(OPT_SID_MODEL);
    if (model_pref == 1) {
        cfg.defaultSidModel = SidConfig::MOS6581;
        cfg.forceSidModel   = true;
    } else if (model_pref == 2) {
        cfg.defaultSidModel = SidConfig::MOS8580;
        cfg.forceSidModel   = true;
    }

    cfg.samplingMethod = SidConfig::INTERPOLATE;

    if (!engine->config(cfg)) {
        logit("sidplayfp: engine config failed: %s", engine->error());
        return nullptr;
    }

    return engine;
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

void *sidplayfp_open(const char *file)
{
    if (init_db)
        init_database();

    struct sidplayfp_data *s = new sidplayfp_data();
    decoder_error_init(&s->error);

    s->frequency = options_get_int(OPT_FREQ);

#ifdef WORDS_BIGENDIAN
    s->sample_format = SFMT_S16 | SFMT_BE;
#else
    s->sample_format = SFMT_S16 | SFMT_LE;
#endif

    // ---- Load tune --------------------------------------------------------

    s->tune = std::make_unique<SidTune>(file);
    if (!s->tune->getStatus()) {
        decoder_error(&s->error, ERROR_FATAL, 0,
                      "sidplayfp: cannot load \"%s\": %s",
                      file, s->tune->statusString());
        return s;
    }

    const SidTuneInfo *ti = s->tune->getInfo();
    s->songs     = (int)ti->songs();
    s->startSong = (int)ti->startSong();
    s->timeStart = 1;
    s->timeEnd   = s->songs;

    if (startAtStart)
        s->timeStart = s->startSong;
    if (!playSubTunes)
        s->timeEnd = s->timeStart;

    // ---- Accumulate per-song lengths ------------------------------------

    s->sublengths_ms = std::make_unique<int[]>(s->songs);
    s->length_ms     = 0;

    for (int song = s->timeStart; song <= s->timeEnd; ++song) {
        s->tune->selectSong(song);
        if (!s->tune->getStatus()) {
            decoder_error(&s->error, ERROR_FATAL, 0,
                          "sidplayfp: cannot query song %d in \"%s\"",
                          song, file);
            return s;
        }

        int ms = song_length_ms(*s->tune);
        if (ms < 1)
            ms = defaultLength * 1000;
        if (ms < minLength * 1000)
            ms = minLength * 1000;

        s->sublengths_ms[song - 1] = ms;
        s->length_ms += ms;
    }

    if (s->length_ms == 0)
        s->length_ms = defaultLength * 1000;

    // ---- Build emulation engine -----------------------------------------

    s->builder = make_builder();
    if (!s->builder) {
        decoder_error(&s->error, ERROR_FATAL, 0,
                      "sidplayfp: cannot create SIDLite builder");
        return s;
    }

    s->engine = make_engine(s->builder.get(), s->frequency);
    if (!s->engine) {
        decoder_error(&s->error, ERROR_FATAL, 0,
                      "sidplayfp: cannot create engine");
        return s;
    }

    // ---- Select starting song and load ----------------------------------

    s->currentSong = s->timeStart;
    s->tune->selectSong(s->currentSong);

    if (!s->engine->load(s->tune.get())) {
        decoder_error(&s->error, ERROR_FATAL, 0,
                      "sidplayfp: load failed: %s", s->engine->error());
        return s;
    }

    // Must come after load(): initMixer() reads the set of SID chips the
    // engine attached for this tune, which load() is what populates.
    // Calling it earlier (e.g. right after config()) mixes against zero
    // chips and crashes inside libsidplayfp's own mixer setup.
    s->engine->initMixer(true);

    // Sized for SIDPLAYFP_CYCLES worth of interleaved output; see the
    // comment on mix_scratch in the header for why this can't just be
    // sized to whatever play() returns each call.
    s->mix_scratch.resize((size_t)s->engine->getBufSize(SIDPLAYFP_CYCLES));

    s->song_length_frames  = (int)(((long long)s->sublengths_ms[s->currentSong - 1]
                             * s->frequency) / 1000);
    s->song_elapsed_frames = 0;

    return s;
}

void sidplayfp_close(void *void_data)
{
    struct sidplayfp_data *s = static_cast<struct sidplayfp_data *>(void_data);

    decoder_error_clear(&s->error);
    delete s;
}

// ---------------------------------------------------------------------------
// Error / metadata
// ---------------------------------------------------------------------------

void sidplayfp_get_error(void *prv_data,
                                     struct decoder_error *error)
{
    struct sidplayfp_data *s = static_cast<struct sidplayfp_data *>(prv_data);
    decoder_error_copy(error, &s->error);
}

void sidplayfp_info(const char *file_name, struct file_tags *info,
                                const int /* tags_sel */)
{
    if (init_db)
        init_database();

    SidTune st(file_name);
    if (!st.getStatus())
        return;

    const SidTuneInfo *ti = st.getInfo();

    auto fill_tag = [&](std::string *dst, unsigned int idx, int flag) {
        if (ti->numberOfInfoStrings() > idx
                && ti->infoString(idx)
                && ti->infoString(idx)[0]) {
            if (auto tmp = trim(ti->infoString(idx), strlen(ti->infoString(idx)))) {
                *dst = std::move(*tmp);
                info->filled |= flag;
            }
        }
    };

    fill_tag(&info->title,  0, TAGS_COMMENTS);
    fill_tag(&info->artist, 1, TAGS_COMMENTS);
    fill_tag(&info->album,  2, TAGS_COMMENTS); // copyright as album

    info->time   = 0;
    int countStart = 1;
    int countEnd   = (int)ti->songs();

    if (startAtStart)
        countStart = (int)ti->startSong();
    if (!playSubTunes)
        countEnd = countStart;

    for (int song = countStart; song <= countEnd; ++song) {
        st.selectSong(song);

        int ms = song_length_ms(st);
        if (ms < 1)
            ms = defaultLength * 1000;
        if (ms < minLength * 1000)
            ms = minLength * 1000;

        info->time += ms / 1000;
    }

    info->filled |= TAGS_TIME;
}

// ---------------------------------------------------------------------------
// Decode
// ---------------------------------------------------------------------------

int sidplayfp_seek(void *, int)
{
    // Accurate seeking would require replaying from the start; not supported.
    return -1;
}

int sidplayfp_decode(void *void_data, char *buf, int buf_len,
                                 struct sound_params *sound_params)
{
    struct sidplayfp_data *s = static_cast<struct sidplayfp_data *>(void_data);

    if (!s->engine || !s->tune)
        return 0;

    for (;;) {
        int frames_remaining = s->song_length_frames - s->song_elapsed_frames;

        if (frames_remaining <= 0) {
            if (s->currentSong >= s->timeEnd)
                return 0; // all songs consumed

            ++s->currentSong;
            s->tune->selectSong(s->currentSong);
            if (!s->engine->load(s->tune.get()))
                return 0;

            s->song_length_frames  = (int)(((long long)s->sublengths_ms[s->currentSong - 1]
                                     * s->frequency) / 1000);
            s->song_elapsed_frames = 0;

            // Anything queued was rendered under the previous song's
            // (now-reset) engine state — discard rather than deliver it
            // as if it belonged to the new song.
            s->pcm_queue.clear();
            continue;
        }

        // Stereo 16-bit output: 4 bytes per frame (2 samples).
        int frames_available = buf_len / 4;
        int frames_to_render = std::min(frames_available, frames_remaining);

        if (frames_to_render <= 0)
            return 0;

        int samples_needed = frames_to_render * 2;

        // Top up the queue in fixed cycle-sized steps until we have
        // enough samples to satisfy this call (or the engine stalls).
        while ((int)s->pcm_queue.size() < samples_needed) {
            int res = s->engine->play(SIDPLAYFP_CYCLES);
            if (res < 0) {
                decoder_error(&s->error, ERROR_FATAL, 0,
                              "sidplayfp: play failed: %s", s->engine->error());
                return 0;
            }
            if (res == 0)
                break; // nothing more to render this call; avoid spinning

            unsigned int mixed = s->engine->mix(s->mix_scratch.data(),
                                                 (unsigned)res);
            s->pcm_queue.insert(s->pcm_queue.end(),
                                 s->mix_scratch.begin(),
                                 s->mix_scratch.begin() + mixed);
        }

        int have = (int)s->pcm_queue.size();
        int to_copy = std::min(have, samples_needed);
        if (to_copy <= 0)
            return 0;

        std::memcpy(buf, s->pcm_queue.data(), (size_t)to_copy * sizeof(int16_t));
        s->pcm_queue.erase(s->pcm_queue.begin(), s->pcm_queue.begin() + to_copy);

        s->song_elapsed_frames += to_copy / 2;

        sound_params->channels = 2;
        sound_params->rate     = s->frequency;
        sound_params->fmt      = s->sample_format;

        return to_copy * (int)sizeof(int16_t);
    }
}

// ---------------------------------------------------------------------------
// Bitrate / duration / format
// ---------------------------------------------------------------------------

int sidplayfp_get_bitrate(void *)
{
    return -1; // synthesized — no meaningful bitrate
}

int sidplayfp_get_duration(void *void_data)
{
    struct sidplayfp_data *s = static_cast<struct sidplayfp_data *>(void_data);
    return s->length_ms / 1000;
}

int sidplayfp_our_format_ext(const char *ext)
{
    return !strcasecmp(ext, "SID") || !strcasecmp(ext, "MUS");
}

// ---------------------------------------------------------------------------
// Plugin lifecycle
// ---------------------------------------------------------------------------

static void init()
{
    defaultLength = options_get_int(OPT_DEFLEN);
    minLength     = options_get_int(OPT_MINLEN);
    startAtStart  = options_get_bool(OPT_START);
    playSubTunes  = options_get_bool(OPT_SUBTUNES);
    database      = nullptr;
    init_db       = 1;
}

static void destroy()
{
    pthread_mutex_destroy(&db_mtx);
    database.reset();
}

class SidplayfpDecoder : public AudioDecoder {
public:
    std::unique_ptr<void, void(*)(void*)> data;
    SidplayfpDecoder(void *d) : data(d, sidplayfp_close) {}
    ~SidplayfpDecoder() override = default;

    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return sidplayfp_decode(data.get(), buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return sidplayfp_seek(data.get(), sec);
    }

    int get_bitrate() override {
        return sidplayfp_get_bitrate(data.get());
    }

    int get_duration() override {
        return sidplayfp_get_duration(data.get());
    }

    void get_error(struct decoder_error *error) override {
        sidplayfp_get_error(data.get(), error);
    }
};

class SidplayfpPlugin : public AudioPlugin {
public:
    void init() override {
        ::init();
    }

    void destroy() override {
        ::destroy();
    }

    std::unique_ptr<AudioDecoder> open(const char *file) override {
        void *d = sidplayfp_open(file);
        if (!d) return nullptr;
        return std::make_unique<SidplayfpDecoder>(d);
    }

    void info(const char *file_name, struct file_tags *info, const int tags_sel) override {
        sidplayfp_info(file_name, info, tags_sel);
    }

    int our_format_ext(const char *ext) override {
        return sidplayfp_our_format_ext(ext);
    }
};

extern "C" class AudioPlugin *sidplayfp_plugin_init() {
    pthread_mutex_init(&db_mtx, nullptr);
    static SidplayfpPlugin plugin;
    return &plugin;
}

// EOF
