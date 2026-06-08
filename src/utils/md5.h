// src/utils/md5.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// -*- buffer-read-only: t -*- vi: set ro:
// DO NOT EDIT! GENERATED AUTOMATICALLY!
// Declaration of functions and data types used for MD5 sum computing
// This file is part of the GNU C Library.
// Copyright (C) 1995-1997, 1999-2001, 2004-2006, 2008-2010 Free Software
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef _MD5_H
#define _MD5_H 1

#include <stdio.h>
#include <stdint.h>

#define MD5_DIGEST_SIZE 16
#define MD5_BLOCK_SIZE 64

#ifndef _LIBC
#define __md5_buffer md5_buffer
#define __md5_finish_ctx md5_finish_ctx
#define __md5_init_ctx md5_init_ctx
#define __md5_process_block md5_process_block
#define __md5_process_bytes md5_process_bytes
#define __md5_read_ctx md5_read_ctx
#define __md5_stream md5_stream
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  /* Structure to save state of computation between the single steps.  */
  struct md5_ctx
  {
    uint32_t A;
    uint32_t B;
    uint32_t C;
    uint32_t D;

    uint32_t total[2];
    uint32_t buflen;
    uint32_t buffer[32];
  };

  /*
   * The following three functions are build up the low level used in
   * the functions `md5_stream' and `md5_buffer'.
   */

  /* Initialize structure containing state of computation.
     (RFC 1321, 3.3: Step 3)  */
  extern void __md5_init_ctx(struct md5_ctx *ctx);

  /* Starting with the result of former calls of this function (or the
     initialization function update the context for the next LEN bytes
     starting at BUFFER.
     It is necessary that LEN is a multiple of 64!!! */
  extern void __md5_process_block(const void *buffer, size_t len,
                                  struct md5_ctx *ctx);

  /* Starting with the result of former calls of this function (or the
     initialization function update the context for the next LEN bytes
     starting at BUFFER.
     It is NOT required that LEN is a multiple of 64.  */
  extern void __md5_process_bytes(const void *buffer, size_t len,
                                  struct md5_ctx *ctx);

  /* Process the remaining bytes in the buffer and put result from CTX
     in first 16 bytes following RESBUF.  The result is always in little
     endian byte order, so that a byte-wise output yields to the wanted
     ASCII representation of the message digest.  */
  extern void *__md5_finish_ctx(struct md5_ctx *ctx, void *resbuf);

  /* Put result from CTX in first 16 bytes following RESBUF.  The result is
     always in little endian byte order, so that a byte-wise output yields
     to the wanted ASCII representation of the message digest.  */
  extern void *__md5_read_ctx(const struct md5_ctx *ctx, void *resbuf);

  /* Compute MD5 message digest for bytes read from STREAM.  The
     resulting message digest number will be written into the 16 bytes
     beginning at RESBLOCK.  */
  extern int __md5_stream(FILE *stream, void *resblock);

  /* Compute MD5 message digest for LEN bytes beginning at BUFFER.  The
     result is always in little endian byte order, so that a byte-wise
     output yields to the wanted ASCII representation of the message
     digest.  */
  extern void *__md5_buffer(const char *buffer, size_t len,
                            void *resblock);

#ifdef __cplusplus
}
#endif

#endif /* md5.h */

// EOF
