// src/audio/outputs/out_buf.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef BUF_H
#define BUF_H

#include "utils/fifo_buf.h"


  typedef void out_buf_free_callback();

  struct out_buf;

  struct out_buf *out_buf_new(int size);
  void out_buf_free(struct out_buf *buf);
  int out_buf_put(struct out_buf *buf, const char *data, int size);
  void out_buf_pause(struct out_buf *buf);
  void out_buf_unpause(struct out_buf *buf);
  void out_buf_stop(struct out_buf *buf);
  void out_buf_reset(struct out_buf *buf);
  void out_buf_time_set(struct out_buf *buf, const float time);
  int out_buf_time_get(struct out_buf *buf);
  void out_buf_set_free_callback(struct out_buf *buf,
                                 out_buf_free_callback callback);
  int out_buf_get_free(struct out_buf *buf);
  int out_buf_get_fill(struct out_buf *buf);
  void out_buf_wait(struct out_buf *buf);


#endif

// EOF
