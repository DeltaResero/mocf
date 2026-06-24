// src/core/common.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// The purpose of this header is to provide common functions and macros
// used throughout MOC code.  It also provides (x-prefixed) functions
// which augment or adapt their respective system functions with error
// checking and the like.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef COMMON_H
#define COMMON_H

#include <cstdlib>
#include <cstdarg>
#include <climits>
#include <iterator>
#include <optional>
#include <string>
#include <algorithm>

#include "core/compat.h"

/* Suppress overly-enthusiastic GNU variadic macro extensions warning. */
#if defined(__clang__) && HAVE_VARIADIC_MACRO_WARNING
#pragma clang diagnostic ignored "-Wgnu-zero-variadic-macro-arguments"
#endif

struct timespec;

#ifdef HAVE_FUNC_ATTRIBUTE_FORMAT
#define ATTR_PRINTF(x, y) __attribute__((format(printf, x, y)))
#else
#define ATTR_PRINTF(...)
#endif

#ifdef HAVE_FUNC_ATTRIBUTE_NORETURN
#define ATTR_NORETURN __attribute__((noreturn))
#else
#define ATTR_NORETURN
#endif

#ifdef HAVE_VAR_ATTRIBUTE_UNUSED
#define ATTR_UNUSED __attribute__((unused))
#else
#define ATTR_UNUSED
#endif

#ifndef GCC_VERSION
#define GCC_VERSION                                                            \
  (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#endif

/* These macros allow us to use the appropriate method for manipulating
 * GCC's diagnostic pragmas depending on the compiler's version. */
#if GCC_VERSION >= 40200
#define GCC_DIAG_STR(s) #s
#define GCC_DIAG_JOINSTR(x, y) GCC_DIAG_STR(x##y)
#define GCC_DIAG_DO_PRAGMA(x) _Pragma(#x)
#define GCC_DIAG_PRAGMA(x) GCC_DIAG_DO_PRAGMA(GCC diagnostic x)
#if GCC_VERSION >= 40600
#define GCC_DIAG_OFF(x)                                                        \
  GCC_DIAG_PRAGMA(push)                                                        \
  GCC_DIAG_PRAGMA(ignored GCC_DIAG_JOINSTR(-W, x))
#define GCC_DIAG_ON(x) GCC_DIAG_PRAGMA(pop)
#else
#define GCC_DIAG_OFF(x) GCC_DIAG_PRAGMA(ignored GCC_DIAG_JOINSTR(-W, x))
#define GCC_DIAG_ON(x) GCC_DIAG_PRAGMA(warning GCC_DIAG_JOINSTR(-W, x))
#endif
#else
#define GCC_DIAG_OFF(x)
#define GCC_DIAG_ON(x)
#endif

#ifdef HAVE_FORMAT_TRUNCATION_WARNING
#define SUPPRESS_FORMAT_TRUNCATION_WARNING GCC_DIAG_OFF(format-truncation)
#define UNSUPPRESS_FORMAT_TRUNCATION_WARNING GCC_DIAG_ON(format-truncation)
#else
#define SUPPRESS_FORMAT_TRUNCATION_WARNING
#define UNSUPPRESS_FORMAT_TRUNCATION_WARNING
#endif

#define CONFIG_DIR ".mocf"
#define ssizeof(x) ((ssize_t)sizeof(x))

/* Exit status on fatal error. */
#define EXIT_FATAL 2

template <typename T, typename U>
constexpr bool in_range(T val, U lim) noexcept {
  return val >= T{0} && val < static_cast<T>(lim);
}

template <typename Lo, typename T, typename Hi>
constexpr bool in_closed_range(Lo lo, T val, Hi hi) noexcept {
  return val >= static_cast<T>(lo) && val <= static_cast<T>(hi);
}

#ifdef NDEBUG
#define error(...) internal_error(nullptr, 0, nullptr, ##__VA_ARGS__)
#define fatal(...) internal_fatal(nullptr, 0, nullptr, ##__VA_ARGS__)
#define ASSERT_ONLY ATTR_UNUSED
#else
#define error(...) internal_error(__FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define fatal(...) internal_fatal(__FILE__, __LINE__, __func__, ##__VA_ARGS__)
#define ASSERT_ONLY
#endif

#ifndef STRERROR_FN
#define STRERROR_FN xstrerror
#endif

#define error_errno(format, errnum)                                            \
  do                                                                           \
  {                                                                            \
    std::string err##__LINE__ = STRERROR_FN(errnum);                          \
    error(format ": %s", err##__LINE__.c_str());                               \
  } while (0)

  void *xmalloc(size_t size);
  void *xcalloc(size_t nmemb, size_t size);
  void *xrealloc(void *ptr, const size_t size);
  char *xstrdup(const char *s);
  void xsleep(size_t ticks, size_t ticks_per_sec);
  void xsignal(int signum, void (*func)(int));

  void internal_error(const char *file, int line, const char *function,
                      const char *format, ...) ATTR_PRINTF(4, 5);
  void internal_fatal(const char *file, int line, const char *function,
                      const char *format, ...) ATTR_NORETURN ATTR_PRINTF(4, 5);
  bool is_valid_symbol(const char *candidate);
  int get_realtime(struct timespec *ts);
std::string sec_to_min(int seconds);
  const char *get_home();
  void common_cleanup();
  char *pathstrcpy(char *restrict dst, const char *restrict src);

std::string xstrerror(int errnum);
std::string create_file_name(const char *file);
std::optional<std::string> trim(const char *src, size_t len);
std::string format_msg(const char *format, ...) ATTR_PRINTF(1, 2);
std::string format_msg_va(const char *format, va_list va);

#endif

// EOF
