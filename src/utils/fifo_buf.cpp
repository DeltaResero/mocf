// src/utils/fifo_buf.cpp
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

#include <cstddef>
#include <sys/types.h>
#include <cassert>
#include <cstring>
#include <vector>

#include "utils/fifo_buf.h"

fifo_buf::fifo_buf(size_t size)
  : size_(size), pos_(0), fill_(0), buf_(size)
{
  assert(size > 0);
}

/* Put data into the buffer. Returns number of bytes actually put. */
size_t fifo_buf::put(const char *data, size_t size)
{
  size_t written = 0;

  assert(!buf_.empty());

  while (fill_ < size_ && written < size)
  {
    size_t write_from;
    size_t to_write;

    if (pos_ + fill_ < size_)
    {
      write_from = pos_ + fill_;
      to_write = size_ - (pos_ + fill_);
    }
    else
    {
      write_from = fill_ - size_ + pos_;
      to_write = size_ - fill_;
    }

    if (to_write > size - written)
    {
      to_write = size - written;
    }

    memcpy(buf_.data() + write_from, data + written, to_write);
    fill_ += to_write;
    written += to_write;
  }

  return written;
}

/* Copy data from the beginning of the buffer to the user buffer without
 * consuming it. Returns the number of bytes copied. */
size_t fifo_buf::peek(char *user_buf, size_t user_buf_size) const
{
  size_t user_buf_pos = 0, written = 0;
  ssize_t left, pos;

  assert(!buf_.empty());

  left = fill_;
  pos = pos_;

  while (left && written < user_buf_size)
  {
    size_t to_copy = pos + left <= size_ ? left : size_ - pos;

    if (to_copy > user_buf_size - written)
    {
      to_copy = user_buf_size - written;
    }

    memcpy(user_buf + user_buf_pos, buf_.data() + pos, to_copy);
    user_buf_pos += to_copy;
    written += to_copy;

    left -= to_copy;
    pos += to_copy;
    if (pos == size_)
    {
      pos = 0;
    }
  }

  return written;
}

/* Copy and consume data from the beginning of the buffer. Returns the number
 * of bytes read. */
size_t fifo_buf::get(char *user_buf, size_t user_buf_size)
{
  size_t user_buf_pos = 0, written = 0;

  assert(!buf_.empty());

  while (fill_ && written < user_buf_size)
  {
    size_t to_copy = pos_ + fill_ <= size_ ? fill_ : size_ - pos_;

    if (to_copy > user_buf_size - written)
    {
      to_copy = user_buf_size - written;
    }

    memcpy(user_buf + user_buf_pos, buf_.data() + pos_, to_copy);
    user_buf_pos += to_copy;
    written += to_copy;

    fill_ -= to_copy;
    pos_ += to_copy;
    if (pos_ == size_)
    {
      pos_ = 0;
    }
  }

  return written;
}

/* Return the amount of free space in the buffer. */
size_t fifo_buf::space() const
{
  assert(!buf_.empty());
  return size_ - fill_;
}

size_t fifo_buf::fill() const
{
  return fill_;
}

size_t fifo_buf::capacity() const
{
  return size_;
}

void fifo_buf::clear()
{
  fill_ = 0;
}

// EOF
