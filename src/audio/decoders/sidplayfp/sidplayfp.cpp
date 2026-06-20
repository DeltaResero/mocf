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

static SidDatabase     *database;
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

    database = new SidDatabase();
    if (!database->open(dbfile)) {
        logit("sidplayfp: Unable to open SidDatabase \"%s\": %s",
              dbfile, database->error());
        delete database;
        database = nullptr;
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

// Create and sanity-check an ReSIDBuilder.  Returns NULL on failure.
static ReSIDBuilder *make_builder(unsigned int n_chips)
{
    ReSIDBuilder *b = new ReSIDBuilder("ReSID");
    if (!b->getStatus()) {
        logit("sidplayfp: ReSIDBuilder construction failed");
        delete b;
        return nullptr;
    }

    b->create(n_chips);
    if (!b->getStatus()) {
        logit("sidplayfp: ReSIDBuilder::create(%u) failed: %s",
              n_chips, b->error());
        delete b;
        return nullptr;
    }

    return b;
}

// Create and configure a sidplayfp engine.  Returns NULL on failure.
static sidplayfp *make_engine(ReSIDBuilder *builder, int frequency)
{
    sidplayfp *engine = new sidplayfp();

    SidConfig cfg    = engine->config();
    cfg.frequency    = static_cast<uint_least32_t>(frequency);
    cfg.playback     = SidConfig::STEREO;
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
        delete engine;
        return nullptr;
    }

    return engine;
}

// ---------------------------------------------------------------------------
// open / close
// ---------------------------------------------------------------------------

extern "C" void *sidplayfp_open(const char *file)
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

    s->tune = new SidTune(file);
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

    s->sublengths_ms = new int[s->songs]();
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

    // sidChips() is a property of the whole tune file, not per-song.
    unsigned int n_chips = (unsigned int)s->tune->getInfo()->sidChips();
    if (n_chips == 0)
        n_chips = 1;

    s->builder = make_builder(n_chips);
    if (!s->builder) {
        decoder_error(&s->error, ERROR_FATAL, 0,
                      "sidplayfp: cannot create ReSID builder");
        return s;
    }

    s->engine = make_engine(s->builder, s->frequency);
    if (!s->engine) {
        decoder_error(&s->error, ERROR_FATAL, 0,
                      "sidplayfp: cannot create engine");
        return s;
    }

    // ---- Select starting song and load ----------------------------------

    s->currentSong = s->timeStart;
    s->tune->selectSong(s->currentSong);

    if (!s->engine->load(s->tune)) {
        decoder_error(&s->error, ERROR_FATAL, 0,
                      "sidplayfp: load failed: %s", s->engine->error());
        return s;
    }

    s->song_length_frames  = (int)(((long long)s->sublengths_ms[s->currentSong - 1]
                             * s->frequency) / 1000);
    s->song_elapsed_frames = 0;

    return s;
}

extern "C" void sidplayfp_close(void *void_data)
{
    struct sidplayfp_data *s = (struct sidplayfp_data *)void_data;

    // engine holds a raw pointer to the tune — delete engine first.
    delete s->engine;
    delete s->builder;
    delete s->tune;
    delete[] s->sublengths_ms;

    decoder_error_clear(&s->error);
    delete s;
}

// ---------------------------------------------------------------------------
// Error / metadata
// ---------------------------------------------------------------------------

extern "C" void sidplayfp_get_error(void *prv_data,
                                     struct decoder_error *error)
{
    struct sidplayfp_data *s = (struct sidplayfp_data *)prv_data;
    decoder_error_copy(error, &s->error);
}

extern "C" void sidplayfp_info(const char *file_name, struct file_tags *info,
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
            char *tmp = trim(ti->infoString(idx),
                             strlen(ti->infoString(idx)));
            if (tmp) {
                *dst = tmp;
                free(tmp);
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

extern "C" int sidplayfp_seek(void *, int)
{
    // Accurate seeking would require replaying from the start; not supported.
    return -1;
}

extern "C" int sidplayfp_decode(void *void_data, char *buf, int buf_len,
                                 struct sound_params *sound_params)
{
    struct sidplayfp_data *s = (struct sidplayfp_data *)void_data;

    if (!s->engine || !s->tune)
        return 0;

    // Advance to the next song when the current one has been fully rendered.
    // The while handles the (unlikely) case of a zero-length sublength.
    while (s->song_elapsed_frames >= s->song_length_frames) {
        if (s->currentSong >= s->timeEnd)
            return 0; // all songs consumed

        ++s->currentSong;
        s->tune->selectSong(s->currentSong);
        if (!s->engine->load(s->tune))
            return 0;

        s->song_length_frames  = (int)(((long long)s->sublengths_ms[s->currentSong - 1]
                                 * s->frequency) / 1000);
        s->song_elapsed_frames = 0;
    }

    // Stereo 16-bit output: 4 bytes per frame (2 samples).
    int frames_available = buf_len / 4;
    int frames_remaining = s->song_length_frames - s->song_elapsed_frames;
    int frames_to_render = std::min(frames_available, frames_remaining);

    if (frames_to_render <= 0)
        return 0;

    int samples_to_render = frames_to_render * 2;
    int samples_rendered = s->engine->play(reinterpret_cast<int16_t *>(buf), samples_to_render);

    if (samples_rendered <= 0)
        return 0;

    int frames_rendered = samples_rendered / 2;
    s->song_elapsed_frames += frames_rendered;

    sound_params->channels = 2;
    sound_params->rate     = s->frequency;
    sound_params->fmt      = s->sample_format;

    return (int)(samples_rendered * sizeof(int16_t));
}

// ---------------------------------------------------------------------------
// Bitrate / duration / format
// ---------------------------------------------------------------------------

extern "C" int sidplayfp_get_bitrate(void *)
{
    return -1; // synthesized — no meaningful bitrate
}

extern "C" int sidplayfp_get_duration(void *void_data)
{
    struct sidplayfp_data *s = (struct sidplayfp_data *)void_data;
    return s->length_ms / 1000;
}

extern "C" int sidplayfp_our_format_ext(const char *ext)
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
    delete database;
    database = nullptr;
}

class SidplayfpDecoder : public AudioDecoder {
public:
    void *data;
    SidplayfpDecoder(void *d) : data(d) {}
    ~SidplayfpDecoder() override { sidplayfp_close(data); }
    
    int decode(char *buf, int buf_len, struct sound_params *sound_params) override {
        return sidplayfp_decode(data, buf, buf_len, sound_params);
    }

    int seek(int sec) override {
        return sidplayfp_seek(data, sec);
    }

    int get_bitrate() override {
        return sidplayfp_get_bitrate(data);
    }

    int get_duration() override {
        return sidplayfp_get_duration(data);
    }

    void get_error(struct decoder_error *error) override {
        sidplayfp_get_error(data, error);
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
