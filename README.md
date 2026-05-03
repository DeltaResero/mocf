# Music on Console Framebuffer (mocf)

Music on Console Framebuffer (mocf) is a lightweight ncurses audio player
forked from Music on Console Player (mocp). Targeting minimal resource usage
for smoother playback on constrained systems, the project trims out
non-essential features to apply sane defaults for low-end, non-accelerated,
software-rendered environments like framebuffer consoles.

The interface features a dual-pane layout reminiscent of Midnight Commander.
Native directory handling allows you to simply select a file to begin playback
for the entire folder. While playlists are not required, complex queues can be
built, saved, and exported as m3u files.

### Key Features

*   Client-server architecture for persistent playback
*   Gapless playback via song precaching
*   Native PulseAudio backend and modern FFmpeg (> 4.0) support
*   Support for OSS, ALSA, SNDIO, and JACK output drivers
*   Inotify support for automatic directory refreshing
*   Reworked audio formats with support for 32-bit samples and float processing
*   Simple software mixer and a cache for file tags
*   Customizable color themes and user-defined keybindings
*   Stripped non-essential features for a minimal default build
*   Optimized defaults for software-rendered and terminal environments

## Supported Formats

Supported formats include MP3, Ogg Vorbis, FLAC, Musepack, Speex, Opus, WAVE,
and formats handled by FFmpeg/LibAV (e.g., WMA, AAC, MP4). Additional
support includes AIFF, AU, SID, wavpack, MIDI, and modplug.

## Building and Installation

This project uses the **CMake** build system.

### Prerequisites

*   CMake 3.15 or later
*   C99 and C++17 compatible compilers
*   POSIX.1-2001 compatible system
*   **Required Libraries:** libpopt, ncurses, pthreads
*   **Optional Libraries:** PulseAudio, ALSA, JACK, OSS, SNDIO, libcurl,
    libsamplerate, libmagic, librcc, BerkeleyDB.
*   **Decoder Libraries:** libmad, libmpg123, libfaad2, libvorbis, opusfile,
    libflac, libmpcdec, libwavpack, libsndfile, libmodplug, ffmpeg/libav,
    libspeex, libtimidity, libsidplay2.

### Quick Start

```bash
mkdir build
cd build
cmake ..
make
sudo make install
```

### Build Options

You can configure the build by passing `-DOPTION=ON/OFF` to cmake.

#### General Options
| Option | Description | Default |
|--------|-------------|---------|
| `ENABLE_DEBUG` | Enable debugging code | `OFF` |
| `ENABLE_CACHE` | Enable tags caching code (requires BerkeleyDB) | `ON` |
| `ENABLE_INTERNAL_FLOAT` | Use float for internal processing | `ON` |
| `ENABLE_RCC` | Enable LIBRCC support | `ON` |
| `ENABLE_CURL` | Enable network streams support | `ON` |
| `ENABLE_SAMPLERATE` | Enable libsamplerate | `ON` |
| `ENABLE_MAGIC` | Enable MIME magic support | `ON` |

#### Sound Output Options
| Option | Description | Default |
|--------|-------------|---------|
| `WITH_PULSE` | Enable PulseAudio support | `ON` (Linux) |
| `WITH_ALSA` | Enable ALSA support | `ON` (Linux) |
| `WITH_JACK` | Enable JACK support | `OFF` |
| `WITH_OSS` | Enable OSS support | `ON` (FreeBSD/NetBSD) |
| `WITH_SNDIO` | Enable SNDIO support | `ON` (OpenBSD) |

#### Decoder Options
| Option | Description | Default |
|--------|-------------|---------|
| `WITH_MP3` | Enable MP3 support (libmad) | `ON` |
| `WITH_MPG123` | Enable MPG123 support | `ON` |
| `WITH_AAC` | Enable AAC support | `ON` |
| `WITH_VORBIS` | Enable Ogg Vorbis support | `ON` |
| `WITH_OPUS` | Enable Opus support | `ON` |
| `WITH_FLAC` | Enable FLAC support | `ON` |
| `WITH_MUSEPACK` | Enable Musepack support | `ON` |
| `WITH_WAVPACK` | Enable WavPack support | `ON` |
| `WITH_SNDFILE` | Enable libsndfile support | `ON` |
| `WITH_MODPLUG` | Enable libmodplug support | `ON` |
| `WITH_FFMPEG` | Enable ffmpeg/libav support | `ON` |
| `WITH_SPEEX` | Enable Speex support | `ON` |
| `WITH_TIMIDITY` | Enable libtimidity support | `ON` |
| `WITH_SIDPLAY2` | Enable libsidplay2 support | `ON` |

### Installation Directories

You can customize the installation location:

```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
```

## Usage

Run the program with the `mocf` command. The usage is simple and designed to
be as intuitive as basic functions in Midnight Commander.

```bash
mocf
```

If you need help, press `h` inside the program or read the manpage
(`man mocf`).

### Configuration

Using a configuration file is not required. If you choose to create one,
it should be placed at `~/.mocf/config`. See the `config.example` file for a
full list of options and documentation.

### Themes

Themes control the color scheme of the interface. An example theme is included
in `data/themes/`. Custom themes should be placed in `~/.mocf/themes/`.

The active theme can be selected with the `Theme` option in your config file
or by using the `-T` command-line option.

### Keymaps

You can redefine the default keybindings by creating a custom keymap file. See
the instructions in the `keymap.example` file for details on the format and a
full list of available commands.

## Bug Reporting

This is a fork of the original MOC project. **Please do not report bugs for
MOCF to the original MOC developers.** This version deviates from the
original codebase and follows a different development philosophy.

## Acknowledgments

This software is a fork of the original Music on Console (MOC) project. For a
detailed history, a full list of attributions, and information on the community
forks this is based on, please see the [AUTHORS](AUTHORS) file.

## Disclaimer

This project is not affiliated with, endorsed by, nor sponsored by the
original authors of MOC.

This program is distributed under the terms of the GNU General Public
License version 3 (or later). You can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

This project is distributed in the hope that it will be useful, but
**WITHOUT ANY WARRANTY**; without even the implied warranty of
**MERCHANTABILITY** or **FITNESS FOR A PARTICULAR PURPOSE**. See the
[GNU General Public License](https://www.gnu.org/licenses/gpl-3.0.html)
for more details.
