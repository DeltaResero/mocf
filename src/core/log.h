// src/core/log.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef LOG_H
#define LOG_H

#include <cstdio>
#ifdef __cplusplus
#include <string>
#endif

#ifdef __cplusplus
extern "C"
{
#endif

/* Suppress overly-enthusiastic GNU variadic macro extensions warning. */
#if defined(__clang__) && HAVE_VARIADIC_MACRO_WARNING
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

#ifdef DEBUG
#define debug logit
#else
#define debug(...)                                                             \
  do                                                                           \
  {                                                                            \
  } while (0)
#endif

#ifndef NDEBUG
#define logit(...) internal_logit(__FILE__, __LINE__, __func__, ##__VA_ARGS__)
#else
#define logit(...)                                                             \
  do                                                                           \
  {                                                                            \
  } while (0)
#endif

#ifndef STRERROR_FN
#define STRERROR_FN xstrerror
#endif

#ifndef NDEBUG
#define log_errno(format, errnum)                                              \
  do                                                                           \
  {                                                                            \
    std::string err##__LINE__ = STRERROR_FN(errnum);                          \
    logit(format ": %s", err##__LINE__.c_str());                               \
  } while (0)
#else
#define log_errno(...)                                                         \
  do                                                                           \
  {                                                                            \
  } while (0)
#endif

  void internal_logit(const char *file, const int line, const char *function,
                      const char *format, ...) ATTR_PRINTF(4, 5);

#ifndef NDEBUG
#define LOGIT_ONLY
#else
#define LOGIT_ONLY ATTR_UNUSED
#endif

#if !defined(NDEBUG) && defined(DEBUG)
#define DEBUG_ONLY
#else
#define DEBUG_ONLY ATTR_UNUSED
#endif

  void log_init_stream(FILE *f, const char *fn);
  void log_circular_start();
  void log_circular_log();
  void log_circular_reset();
  void log_circular_stop();
  void log_close();

#ifndef NDEBUG
  void log_signal(int sig);
#else
#define log_signal(...)                                                        \
  do                                                                           \
  {                                                                            \
  } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif

// EOF
