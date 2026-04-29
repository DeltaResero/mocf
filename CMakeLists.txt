cmake_minimum_required(VERSION 3.15)
project(
  mocf
  VERSION 2.6
  LANGUAGES C CXX)

# --- Package Metadata ---
set(PACKAGE_NAME "Music on Console Framebuffer")
set(PACKAGE_VERSION "2.6")
set(PACKAGE_BUGREPORT "https://github.com/DeltaResero/mocf/issues")
set(PACKAGE_URL "https://github.com/DeltaResero/mocf")
set(PACKAGE_TARNAME "mocf")
set(PACKAGE_STRING "${PACKAGE_NAME} ${PACKAGE_VERSION}")

# Standard settings
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")

# Large File Support & Config Header Guard & GNU Extensions
add_compile_definitions(_FILE_OFFSET_BITS=64)
add_compile_definitions(HAVE_CONFIG_H)
add_compile_definitions(_GNU_SOURCE)

# Ensure checks use GNU_SOURCE too
set(CMAKE_REQUIRED_DEFINITIONS "-D_GNU_SOURCE")

# Options
option(ENABLE_DEBUG "Enable debugging code" OFF)
option(ENABLE_CACHE "Enable tags caching code" ON)
option(ENABLE_INTERNAL_FLOAT "Enable internal float processing" ON)
option(ENABLE_RCC "Enable LIBRCC support" ON)
option(ENABLE_CURL "Enable network streams support" ON)
option(ENABLE_SAMPLERATE "Enable libsamplerate" ON)
option(ENABLE_MAGIC "Enable MIME magic support" ON)

# OS-specific defaults for native audio backends
set(DEFAULT_ALSA OFF)
set(DEFAULT_OSS OFF)
set(DEFAULT_SNDIO OFF)

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  set(DEFAULT_ALSA ON)
elseif(CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
  set(DEFAULT_SNDIO ON)
else() # FreeBSD, NetBSD, etc.
  set(DEFAULT_OSS ON)
endif()

# Sound output options
option(WITH_PULSE "Enable PulseAudio support" OFF)
option(WITH_OSS "Enable OSS support" ${DEFAULT_OSS})
option(WITH_SNDIO "Enable SNDIO support" ${DEFAULT_SNDIO})
option(WITH_ALSA "Enable ALSA support" ${DEFAULT_ALSA})
option(WITH_JACK "Enable JACK support" OFF)

# Decoder options
option(WITH_MP3 "Enable MP3 support (libmad)" ON)
option(WITH_MPG123 "Enable MPG123 support" ON)
option(WITH_AAC "Enable AAC support" ON)
option(WITH_VORBIS "Enable Ogg Vorbis support" ON)
option(WITH_OPUS "Enable Opus support" ON)
option(WITH_FLAC "Enable FLAC support" ON)
option(WITH_MUSEPACK "Enable Musepack support" ON)
option(WITH_WAVPACK "Enable WavPack support" ON)
option(WITH_SNDFILE "Enable libsndfile support" ON)
option(WITH_MODPLUG "Enable libmodplug support" ON)
option(WITH_FFMPEG "Enable ffmpeg/libav support" ON)
option(WITH_SPEEX "Enable Speex support" ON)
option(WITH_TIMIDITY "Enable libtimidity support" ON)
option(WITH_SIDPLAY2 "Enable libsidplay2 support" ON)

# Dependencies
include(CheckIncludeFile)
include(CheckFunctionExists)
include(CheckSymbolExists)
include(CheckLibraryExists)
include(TestBigEndian)
include(GNUInstallDirs)

# Set install directories
set(THEMES_DIR "${CMAKE_INSTALL_DATADIR}/mocf/themes")

# Platform checks
test_big_endian(WORDS_BIGENDIAN)
if(CMAKE_SYSTEM_NAME MATCHES "Linux")
  add_compile_definitions(LINUX=1)
elseif(CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
  add_compile_definitions(OPENBSD=1)
elseif(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
  add_compile_definitions(FREEBSD=1)
endif()

# Required Packages
find_package(Threads REQUIRED)
find_package(Curses REQUIRED)
find_package(Iconv REQUIRED)

# Iconv Check
if(Iconv_FOUND)
  set(HAVE_ICONV 1)
endif()

# Curses Header Checks
set(CMAKE_REQUIRED_INCLUDES ${CURSES_INCLUDE_DIRS})
check_include_file("ncursesw/curses.h" HAVE_NCURSESW_CURSES_H)
check_include_file("ncursesw.h" HAVE_NCURSESW_H)
check_include_file("ncurses/curses.h" HAVE_NCURSES_CURSES_H)
check_include_file("ncurses.h" HAVE_NCURSES_H)
check_include_file("curses.h" HAVE_CURSES_H)
unset(CMAKE_REQUIRED_INCLUDES)


# POPT
find_library(POPT_LIBRARY NAMES popt)
find_path(POPT_INCLUDE_DIR popt.h)
if(NOT POPT_LIBRARY OR NOT POPT_INCLUDE_DIR)
  message(FATAL_ERROR "libpopt not found.")
endif()

# Header checks
check_include_file(strings.h HAVE_STRINGS_H)
check_include_file(sys/un.h HAVE_SYS_UN_H)
check_include_file(sys/ioctl.h HAVE_SYS_IOCTL_H)
check_include_file(sys/param.h HAVE_SYS_PARAM_H)
check_include_file(byteswap.h HAVE_BYTESWAP_H)
check_include_file(langinfo.h HAVE_LANGINFO_H)
check_include_file(nl_types.h HAVE_NL_TYPES_H)
check_include_file(sys/inotify.h HAVE_SYS_INOTIFY_H)
check_include_file(stdint.h HAVE_STDINT_H)
check_include_file(inttypes.h HAVE_INTTYPES_H)

# Function checks
check_function_exists(nl_langinfo HAVE_NL_LANGINFO)
check_function_exists(localtime_r HAVE_LOCALTIME_R)
check_function_exists(nanosleep HAVE_NANOSLEEP)
check_function_exists(strcasecmp HAVE_STRCASECMP)
check_function_exists(strdup HAVE_STRDUP)
check_function_exists(strncasecmp HAVE_STRNCASECMP)
check_function_exists(wcswidth HAVE_WCSWIDTH)
check_function_exists(sched_get_priority_max HAVE_SCHED_GET_PRIORITY_MAX)
check_function_exists(syslog HAVE_SYSLOG)
check_function_exists(strcasestr HAVE_STRCASESTR)
check_function_exists(getrlimit HAVE_GETRLIMIT)
check_function_exists(mmap HAVE_MMAP)

# Clock checks
check_library_exists(rt clock_gettime "" HAVE_CLOCK_GETTIME_IN_RT)
if(HAVE_CLOCK_GETTIME_IN_RT)
  set(HAVE_CLOCK_GETTIME 1)
  list(APPEND EXTRA_LIBS rt)
else()
  check_function_exists(clock_gettime HAVE_CLOCK_GETTIME)
  if(NOT HAVE_CLOCK_GETTIME)
    check_function_exists(gettimeofday HAVE_GETTIMEOFDAY)
  endif()
endif()

# Optional Features
if(ENABLE_CACHE)
  # Berkeley DB
  find_path(BDB_INCLUDE_DIR db.h)
  find_library(BDB_LIBRARY NAMES db)
  if(BDB_INCLUDE_DIR AND BDB_LIBRARY)
    include_directories(${BDB_INCLUDE_DIR})
    list(APPEND EXTRA_LIBS ${BDB_LIBRARY})
    set(HAVE_DB_H 1)
  endif()
endif()

if(ENABLE_INTERNAL_FLOAT)
  set(INTERNAL_FLOAT 1)
endif()

if(ENABLE_CURL)
  find_package(CURL 7.15.1)
  if(CURL_FOUND)
    set(HAVE_CURL 1)
    list(APPEND EXTRA_OBJS src/io/io_curl.c)
    include_directories(${CURL_INCLUDE_DIRS})
    list(APPEND EXTRA_LIBS ${CURL_LIBRARIES})
  endif()
endif()

if(ENABLE_SAMPLERATE)
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(SAMPLERATE samplerate>=0.1.0)
    if(SAMPLERATE_FOUND)
      set(HAVE_SAMPLERATE 1)
      include_directories(${SAMPLERATE_INCLUDE_DIRS})
      list(APPEND EXTRA_LIBS ${SAMPLERATE_LIBRARIES})
    endif()
  endif()
endif()

if(ENABLE_MAGIC)
  check_library_exists(magic magic_open "" HAVE_LIBMAGIC)
  if(HAVE_LIBMAGIC)
    set(HAVE_LIBMAGIC 1)
    list(APPEND EXTRA_LIBS magic)
  endif()
endif()

if(ENABLE_RCC)
  check_include_file(librcc.h HAVE_RCC_H)
  check_library_exists(rcc rccInit "" HAVE_LIBRCC)
  if(HAVE_RCC_H AND HAVE_LIBRCC)
    set(HAVE_RCC 1)
    list(APPEND EXTRA_LIBS rcc)
  endif()
endif()

# Sound Drivers
set(SOUND_DRIVERS "")

if(WITH_PULSE)
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(PULSE libpulse)
    if(PULSE_FOUND)
      set(HAVE_PULSE 1)
      list(APPEND EXTRA_OBJS src/audio/outputs/pulse.c)
      include_directories(${PULSE_INCLUDE_DIRS})
      list(APPEND EXTRA_LIBS ${PULSE_LIBRARIES})
      list(APPEND SOUND_DRIVERS "PULSE")
    endif()
  endif()
endif()

if(WITH_ALSA)
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(ALSA alsa>=1.0.11)
    if(ALSA_FOUND)
      set(HAVE_ALSA 1)
      list(APPEND EXTRA_OBJS src/audio/outputs/alsa.c)
      include_directories(${ALSA_INCLUDE_DIRS})
      list(APPEND EXTRA_LIBS ${ALSA_LIBRARIES})
      list(APPEND SOUND_DRIVERS "ALSA")
    endif()
  endif()
endif()

if(WITH_JACK)
  find_package(PkgConfig)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(JACK jack>=0.4)
    if(JACK_FOUND)
      set(HAVE_JACK 1)
      list(APPEND EXTRA_OBJS src/audio/outputs/jack.c)
      include_directories(${JACK_INCLUDE_DIRS})
      list(APPEND EXTRA_LIBS ${JACK_LIBRARIES})
      list(APPEND SOUND_DRIVERS "JACK")

      set(CMAKE_REQUIRED_LIBRARIES ${JACK_LIBRARIES})
      check_symbol_exists(jack_client_open "jack/jack.h" HAVE_JACK_CLIENT_OPEN)
      if(HAVE_JACK_CLIENT_OPEN)
        set(HAVE_JACK_CLIENT_OPEN 1)
      endif()
    endif()
  endif()
endif()

if(WITH_OSS)
  check_include_file(sys/soundcard.h HAVE_SYS_SOUNDCARD_H)
  check_include_file(soundcard.h HAVE_SOUNDCARD_H)
  if(HAVE_SYS_SOUNDCARD_H OR HAVE_SOUNDCARD_H)
    set(HAVE_OSS 1)
    list(APPEND EXTRA_OBJS src/audio/outputs/oss.c)
    list(APPEND SOUND_DRIVERS "OSS")
    # Some systems need libossaudio
    check_library_exists(ossaudio _oss_ioctl "" HAVE_OSSAUDIO)
    if(HAVE_OSSAUDIO)
      list(APPEND EXTRA_LIBS ossaudio)
    endif()
  endif()
endif()

if(WITH_SNDIO)
  check_include_file(sndio.h HAVE_SNDIO_H)
  if(HAVE_SNDIO_H)
    set(HAVE_SNDIO 1)
    list(APPEND EXTRA_OBJS src/audio/outputs/sndio_out.c)
    list(APPEND SOUND_DRIVERS "SNDIO")
    check_library_exists(sndio sio_open "" HAVE_SNDIO_LIB)
    if(HAVE_SNDIO_LIB)
      list(APPEND EXTRA_LIBS sndio)
    endif()
  endif()
endif()

if(ENABLE_DEBUG)
  list(APPEND EXTRA_OBJS src/audio/outputs/null_out.c src/utils/md5.c)
else()
  set(NDEBUG 1)
endif()

# Git Revision
execute_process(
  COMMAND git describe --always
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_REVISION
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(GIT_REVISION)
  set(PACKAGE_REVISION "${GIT_REVISION}")
endif()

# Configure config.h
configure_file(config.h.in config.h)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# Configure config.example Map standard Autoconf variables for substitution in
# config.example.in
set(prefix "${CMAKE_INSTALL_PREFIX}")
set(exec_prefix "${CMAKE_INSTALL_PREFIX}")
set(sysconfdir "${CMAKE_INSTALL_FULL_SYSCONFDIR}")
set(datarootdir "${CMAKE_INSTALL_FULL_DATADIR}")
set(datadir "${CMAKE_INSTALL_FULL_DATADIR}")
set(pkgdatadir "${CMAKE_INSTALL_FULL_DATADIR}/mocf")
set(libdir "${CMAKE_INSTALL_FULL_LIBDIR}")
set(includedir "${CMAKE_INSTALL_FULL_INCLUDEDIR}")

configure_file(assets/doc/config.example.in assets/doc/config.example @ONLY)

# Subdirectories
add_subdirectory(src/audio/decoders)
add_subdirectory(assets/themes)

# Main Executable
set(MOCF_SOURCES
    src/core/log.c
    src/core/protocol.c
    src/core/server.c
    src/core/main.c
    src/core/common.c
    src/library/playlist.c
    src/utils/fifo_buf.c
    src/audio/outputs/out_buf.c
    src/audio/audio.c
    src/audio/decoder.c
    src/ui/curses/interface.c
    src/ui/curses/interface_elements.c
    src/ui/curses/menu.c
    src/library/files.c
    src/core/options.c
    src/library/player.c
    src/library/playlist_file.c
    src/ui/themes.c
    src/ui/input/keys.c
    src/io/io.c
    src/core/compat.c
    src/audio/conversion/audio_conversion.c
    src/utils/rbtree.c
    src/library/tags_cache.c
    src/utils/utf8.c
    src/library/rcc.c
    src/audio/processing/softmixer.c
    src/utils/lists.c
    src/audio/processing/equalizer.c
    ${EXTRA_OBJS}
    ${DECODER_OBJECTS})

add_executable(mocf ${MOCF_SOURCES})

target_include_directories(
  mocf PRIVATE ${CMAKE_SOURCE_DIR}/src
               ${CMAKE_BINARY_DIR}/src/audio
               ${CURSES_INCLUDE_DIRS}
               ${POPT_INCLUDE_DIR})

target_compile_definitions(
  mocf
  PRIVATE
    SYSTEM_THEMES_DIR="${CMAKE_INSTALL_PREFIX}/${THEMES_DIR}")

target_link_libraries(
  mocf
  PRIVATE ${CMAKE_THREAD_LIBS_INIT}
          ${CURSES_LIBRARIES}
          ${POPT_LIBRARY}
          Iconv::Iconv
          m
          ${DECODER_LIBS}
          ${EXTRA_LIBS})

# Propagate any extra library search paths needed by decoder dependencies
# (currently only sidplay2's resid-builder path, when enabled).
if(DECODER_LINK_DIRS)
  target_link_directories(mocf PRIVATE ${DECODER_LINK_DIRS})
endif()

# Installation
install(TARGETS mocf DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/assets/doc/config.example" README.md
              assets/doc/README_equalizer assets/doc/keymap.example
        DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES assets/man/mocf.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
install(FILES assets/desktop/mocf.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES assets/desktop/mocf.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# Helper for summary output
macro(bool_to_onoff VAR RESULT)
  if(${VAR})
    set(${RESULT} "ON")
  else()
    set(${RESULT} "OFF")
  endif()
endmacro()

bool_to_onoff(HAVE_RCC STATUS_RCC)
bool_to_onoff(HAVE_CURL STATUS_CURL)
bool_to_onoff(HAVE_SAMPLERATE STATUS_SAMPLERATE)
bool_to_onoff(HAVE_LIBMAGIC STATUS_MAGIC)

# Summary
message(STATUS "")
message(STATUS "Configuration Summary (mocf):")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  Sound drivers: ${SOUND_DRIVERS}")
message(STATUS "  Debug: ${ENABLE_DEBUG}")
message(STATUS "  Cache: ${ENABLE_CACHE}")
message(STATUS "  RCC: ${STATUS_RCC}")
message(STATUS "  Network streams: ${STATUS_CURL}")
message(STATUS "  Resampling: ${STATUS_SAMPLERATE}")
message(STATUS "  MIME magic: ${STATUS_MAGIC}")
message(STATUS "")
