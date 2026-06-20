// src/utils/fifo_buf.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef FIFO_BUF_H
#define FIFO_BUF_H


  struct fifo_buf;

  struct fifo_buf *fifo_buf_new(const size_t size);
  void fifo_buf_free(struct fifo_buf *b);
  size_t fifo_buf_put(struct fifo_buf *b, const char *data, size_t size);
  size_t fifo_buf_get(struct fifo_buf *b, char *user_buf, size_t user_buf_size);
  size_t fifo_buf_peek(struct fifo_buf *b, char *user_buf,
                       size_t user_buf_size);
  size_t fifo_buf_get_space(const struct fifo_buf *b);
  void fifo_buf_clear(struct fifo_buf *b);
  size_t fifo_buf_get_fill(const struct fifo_buf *b);
  size_t fifo_buf_get_size(const struct fifo_buf *b);


#endif

// EOF
