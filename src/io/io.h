// src/io/io.h
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef IO_H
#define IO_H

#include <sys/types.h>
#include <pthread.h>

#include "utils/fifo_buf.h"

#include <string>


  enum io_source
  {
    IO_SOURCE_FD,
    IO_SOURCE_MMAP
  };

  struct io_stream;

  typedef void (*buf_fill_callback_t)(struct io_stream *s, size_t fill,
                                      size_t buf_size, void *data_ptr);

  struct io_stream
  {
    enum io_source source; /* source of the file */
    int fd;
    off_t size;     /* size of the file */
    int errno_val;  /* errno value of the last operation  - 0 if ok */
    int read_error; /* set to != 0 if the last read operation dailed */
    std::string strerror; /* error string */
    int opened;     /* was the stream opened (open(), mmap(), etc.)? */
    int eof;        /* was the end of file reached? */
    int after_seek; /* are we after seek and need to do fresh read()? */
    int buffered;   /* are we using the buffer? */
    off_t pos; /* current position in the file from the user point of view */
    pthread_mutex_t io_mtx; /* mutex for IO operations */

#ifdef HAVE_MMAP
    void *mem;
    off_t mem_pos;
#endif

    struct fifo_buf *buf;
    pthread_mutex_t buf_mtx;
    pthread_cond_t buf_free_cond; /* some space became available in the
             buffer */
    pthread_cond_t buf_fill_cond; /* the buffer was filled with some data */
    pthread_t read_thread;
    int stop_read_thread; /* request for stopping the read
             thread */

    /* callbacks */
    buf_fill_callback_t buf_fill_callback;
    void *buf_fill_callback_data;
  };

  struct io_stream *io_open(const char *file, const int buffered);
  ssize_t io_read(struct io_stream *s, void *buf, size_t count);
  ssize_t io_peek(struct io_stream *s, void *buf, size_t count);
  off_t io_seek(struct io_stream *s, off_t offset, int whence);
  void io_close(struct io_stream *s);
  int io_ok(struct io_stream *s);
  const char *io_strerror(struct io_stream *s);
  off_t io_file_size(const struct io_stream *s);
  off_t io_tell(struct io_stream *s);
  int io_eof(struct io_stream *s);
  void io_init();
  void io_cleanup();
  void io_abort(struct io_stream *s);
  void io_set_buf_fill_callback(struct io_stream *s,
                                buf_fill_callback_t callback, void *data_ptr);
  int io_seekable(const struct io_stream *s);


#endif

// EOF
