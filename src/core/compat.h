// src/core/compat.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// The purpose of this header is to provide functions and macros which
// MOC code expects but which are missing or broken on the host system.
// This header should be included by all code before any other MOC
// headers (except 'compiler.h').  Therefore, it is included once by
// 'common.h' which is itself included by all code.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef COMPAT_H
#define COMPAT_H

#ifdef HAVE_BYTESWAP_H
#include <byteswap.h>
#else
/* Given an unsigned 16-bit argument X, return the value corresponding to
   X with reversed byte order.  */
#define bswap_16(x) ((((x) & 0x00FF) << 8) | (((x) & 0xFF00) >> 8))

/* Given an unsigned 32-bit argument X, return the value corresponding to
   X with reversed byte order.  */
#define bswap_32(x)                                                            \
  ((((x) & 0x000000FF) << 24) | (((x) & 0x0000FF00) << 8) |                    \
   (((x) & 0x00FF0000) >> 8) | (((x) & 0xFF000000) >> 24))
#endif

/* Maximum path length, we don't consider exceptions like mounted NFS */
#ifndef PATH_MAX
#if defined(_POSIX_PATH_MAX)
#define PATH_MAX _POSIX_PATH_MAX /* Posix */
#elif defined(MAXPATHLEN)
#define PATH_MAX MAXPATHLEN /* Solaris? Also linux...*/
#else
#define PATH_MAX 4096 /* Suppose, we have 4096 */
#endif
#endif


#if !HAVE_DECL_STRCASESTR && !defined(__cplusplus)
  char *strcasestr(const char *haystack, const char *needle);
#endif


#endif

// EOF
