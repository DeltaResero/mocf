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

#include <cstddef>
#include <vector>

class fifo_buf
{
public:
  explicit fifo_buf(size_t size);

  /* Put data into the buffer. Returns number of bytes actually written. */
  size_t put(const char *data, size_t size);

  /* Copy and consume data from the front of the buffer. Returns bytes read. */
  size_t get(char *user_buf, size_t user_buf_size);

  /* Copy without consuming data from the front of the buffer. Returns bytes
   * copied. */
  size_t peek(char *user_buf, size_t user_buf_size) const;

  /* Return the number of bytes that can still be written. */
  size_t space() const;

  /* Return the number of bytes currently in the buffer. */
  size_t fill() const;

  /* Return the total capacity of the buffer. */
  size_t capacity() const;

  /* Discard all buffered data. */
  void clear();

private:
  int size_;
  int pos_;
  int fill_;
  std::vector<char> buf_;
};

#endif

// EOF
