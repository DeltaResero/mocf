// src/core/compat.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2005 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Various functions which some systems lack. */

#ifndef HAVE_STRCASESTR
#include <algorithm>
#include <cctype>
#include <string>

/* Case insensitive version of strstr(). */
char *strcasestr(const char *haystack, const char *needle)
{
  auto lower = [](unsigned char c) { return std::tolower(c); };

  std::string haystack_l(haystack);
  std::string needle_l(needle);

  std::transform(haystack_l.begin(), haystack_l.end(), haystack_l.begin(), lower);
  std::transform(needle_l.begin(), needle_l.end(), needle_l.begin(), lower);

  size_t pos = haystack_l.find(needle_l);
  return pos == std::string::npos ? nullptr : const_cast<char *>(haystack) + pos;
}
#endif

/* This is required to prevent an "empty translation unit" warning
   if neither strcasestr() nor clock_gettime() get defined. */
#if defined(HAVE_STRCASESTR) && defined(HAVE_CLOCK_GETTIME)
int compat_is_empty;
#endif

// EOF
