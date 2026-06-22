// src/io/io.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// TODO:
// - handle SIGBUS (mmap() read error)
// Copyright (C) 2005 Damian Pietras <daper@daper.net>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cassert>
#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

#ifdef HAVE_MMAP
#include <sys/mman.h>
#endif

/*#define DEBUG*/

#include "core/common.h"
#include "core/log.h"
#include "io/io.h"
#include "core/options.h"
#include "library/files.h"

#ifdef HAVE_MMAP
static void *io_mmap_file(const struct io_stream *s)
{
  void *result = nullptr;

  do
  {
    if (s->size < 1 || static_cast<uint64_t>(s->size) > SIZE_MAX)
    {
      logit("File size unsuitable for mmap()");
      break;
    }

    const size_t sz = static_cast<size_t>(s->size);

    result = mmap(nullptr, sz, PROT_READ, MAP_SHARED, s->fd, 0);
    if (result == MAP_FAILED)
    {
      log_errno("mmap() failed", errno);
      result = nullptr;
      break;
    }

    logit("mmap()ed %zu bytes", sz);
  } while (false);

  return result;
}
#endif

#ifdef HAVE_MMAP
static ssize_t io_read_mmap(struct io_stream *s, const int dont_move, void *buf,
                            size_t count)
{
  struct stat file_stat;
  size_t to_read;

  assert(s->mem != nullptr);

  if (fstat(s->fd, &file_stat) == -1)
  {
    log_errno("fstat() failed", errno);
    return -1;
  }

  if (s->size != file_stat.st_size)
  {
    logit("File size has changed");

    if (munmap(s->mem, static_cast<size_t>(s->size)))
    {
      log_errno("munmap() failed", errno);
      return -1;
    }

    s->size = file_stat.st_size;
    s->mem = io_mmap_file(s);
    if (!s->mem)
    {
      return -1;
    }

    if (s->mem_pos > s->size)
    {
      logit("File shrunk");
    }
  }

  if (s->mem_pos >= s->size)
  {
    return 0;
  }

  to_read = std::min(count, static_cast<size_t>(s->size - s->mem_pos));
  memcpy(buf, static_cast<char *>(s->mem) + s->mem_pos, to_read);

  if (!dont_move)
  {
    s->mem_pos += to_read;
  }

  return to_read;
}
#endif

static ssize_t io_read_fd(struct io_stream *s, const int dont_move, void *buf,
                          size_t count)
{
  ssize_t res;

  res = read(s->fd, buf, count);

  if (res < 0)
  {
    return -1;
  }

  if (dont_move && lseek(s->fd, -res, SEEK_CUR) < 0)
  {
    return -1;
  }

  return res;
}

/* Read the data from the stream resource.  If dont_move was set, the stream
 * position is unchanged. */
static ssize_t io_internal_read(struct io_stream *s, const int dont_move,
                                char *buf, size_t count)
{
  ssize_t res = 0;

  assert(s != nullptr);
  assert(buf != nullptr);

  switch (s->source)
  {
    case IO_SOURCE_FD:
      res = io_read_fd(s, dont_move, buf, count);
      break;
#ifdef HAVE_MMAP
    case IO_SOURCE_MMAP:
      res = io_read_mmap(s, dont_move, buf, count);
      break;
#endif
    default:
      fatal("Unknown io_stream->source: %d", s->source);
  }

  return res;
}

#ifdef HAVE_MMAP
static off_t io_seek_mmap(struct io_stream *s, const off_t where)
{
  return (s->mem_pos = where);
}
#endif

static off_t io_seek_fd(struct io_stream *s, const off_t where)
{
  return lseek(s->fd, where, SEEK_SET);
}

static off_t io_seek_buffered(struct io_stream *s, const off_t where)
{
  off_t res = -1;

  logit("Seeking...");

  switch (s->source)
  {
    case IO_SOURCE_FD:
      res = io_seek_fd(s, where);
      break;
#ifdef HAVE_MMAP
    case IO_SOURCE_MMAP:
      res = io_seek_mmap(s, where);
      break;
#endif
    default:
      fatal("Unknown io_stream->source: %d", s->source);
  }

  std::lock_guard<std::mutex> lock(s->buf_mtx);
  s->buf->clear();
  s->buf_free_cond.notify_one();
  s->after_seek = 1;
  s->eof = 0;

  return res;
}

static off_t io_seek_unbuffered(struct io_stream *s, const off_t where)
{
  off_t res = -1;

  switch (s->source)
  {
#ifdef HAVE_MMAP
    case IO_SOURCE_MMAP:
      res = io_seek_mmap(s, where);
      break;
#endif
    case IO_SOURCE_FD:
      res = io_seek_fd(s, where);
      break;
    default:
      fatal("Unknown io_stream->source: %d", s->source);
  }

  return res;
}

off_t io_seek(struct io_stream *s, off_t offset, int whence)
{
  off_t res, new_pos = 0;

  assert(s != nullptr);
  assert(s->opened);

  if (!io_ok(s))
  {
    return -1;
  }

  std::unique_lock<std::mutex> lock(s->io_mtx);
  switch (whence)
  {
    case SEEK_SET:
      new_pos = offset;
      break;
    case SEEK_CUR:
      new_pos = s->pos + offset;
      break;
    case SEEK_END:
      new_pos = s->size + offset;
      break;
    default:
      fatal("Bad whence value: %d", whence);
  }

  new_pos = std::clamp<off_t>(new_pos, 0, s->size);

  if (s->buffered)
  {
    res = io_seek_buffered(s, new_pos);
  }
  else
  {
    res = io_seek_unbuffered(s, new_pos);
  }

  if (res != -1)
  {
    s->pos = res;
  }
  lock.unlock();

  if (res != -1)
  {
    debug("Seek to: %" PRId64, res);
  }
  else
  {
    logit("Seek error");
  }

  return res;
}

/* Wake up the IO reading thread. */
static void io_wake_up(struct io_stream *s ATTR_UNUSED)
{
}

/* Abort an IO operation from another thread. */
void io_abort(struct io_stream *s)
{
  assert(s != nullptr);

  if (s->buffered && !s->stop_read_thread)
  {
    logit("Aborting...");
    std::lock_guard<std::mutex> lock(s->buf_mtx);
    s->stop_read_thread = 1;
    io_wake_up(s);
    s->buf_fill_cond.notify_all();
    s->buf_free_cond.notify_all();
    logit("done");
  }
}

/* Close the stream and free all resources associated with it. */
void io_close(struct io_stream *s)
{
  assert(s != nullptr);

  logit("Closing stream...");

  if (s->opened)
  {
    if (s->buffered)
    {
      io_abort(s);

      logit("Waiting for io_read_thread()...");
      if (s->read_thread.joinable()) s->read_thread.join();
      logit("IO read thread exited");
    }

    switch (s->source)
    {
      case IO_SOURCE_FD:
        close(s->fd);
        break;
#ifdef HAVE_MMAP
      case IO_SOURCE_MMAP:
        if (s->mem && munmap(s->mem, static_cast<size_t>(s->size)))
        {
          log_errno("munmap() failed", errno);
        }
        close(s->fd);
        break;
#endif
      default:
        fatal("Unknown io_stream->source: %d", s->source);
    }

    s->opened = 0;

    if (s->buffered)
    {
      delete s->buf;
      s->buf = nullptr;
    }
  }

  delete s;

  logit("done");
}

static void io_read_thread(struct io_stream *s)
{
  logit("IO read thread created");

  while (!s->stop_read_thread)
  {
    char read_buf[8096];
    int read_buf_fill = 0;
    int read_buf_pos = 0;

    s->io_mtx.lock();
    debug("Reading...");

    s->buf_mtx.lock();
    s->after_seek = 0;
    s->buf_mtx.unlock();

    read_buf_fill = io_internal_read(s, 0, read_buf, sizeof(read_buf));
    s->io_mtx.unlock();
    if (read_buf_fill > 0)
    {
      debug("Read %d bytes", read_buf_fill);
    }

    std::unique_lock<std::mutex> buf_lock(s->buf_mtx);

    if (s->stop_read_thread)
    {
      break;
    }

    if (read_buf_fill < 0)
    {
      s->errno_val = errno;
      s->read_error = 1;
      logit("Exiting due to read error.");
      s->buf_fill_cond.notify_all();
      break;
    }

    if (read_buf_fill == 0)
    {
      s->eof = 1;
      debug("EOF, waiting");
      s->buf_fill_cond.notify_all();
      s->buf_free_cond.wait(buf_lock);
      debug("Got signal");
      continue;
    }

    s->eof = 0;

    while (read_buf_pos < read_buf_fill && !s->after_seek)
    {
      size_t put;

      debug("Buffer fill: %zu", s->buf->fill());

      put = s->buf->put(read_buf + read_buf_pos,
                         read_buf_fill - read_buf_pos);

      if (s->stop_read_thread)
      {
        break;
      }

      if (put > 0)
      {
        debug("Put %zu bytes into the buffer", put);
        if (s->buf_fill_callback)
        {
          buf_lock.unlock();
          s->buf_fill_callback(s, s->buf->fill(),
                               s->buf->capacity(),
                               s->buf_fill_callback_data);
          buf_lock.lock();
        }
        s->buf_fill_cond.notify_all();
        read_buf_pos += put;
        continue;
      }

      debug("The buffer is full, waiting.");
      s->buf_free_cond.wait(buf_lock);
      debug("Some space in the buffer was freed");
    }
  }

  if (s->stop_read_thread)
  {
    logit("Stop request");
  }

  logit("Exiting IO read thread");
}

static void io_open_file(struct io_stream *s, const char *file)
{
  struct stat file_stat;

  s->source = IO_SOURCE_FD;

  do
  {
    s->fd = open(file, O_RDONLY);
    if (s->fd == -1)
    {
      s->errno_val = errno;
      break;
    }

    if (fstat(s->fd, &file_stat) == -1)
    {
      s->errno_val = errno;
      close(s->fd);
      break;
    }

    s->size = file_stat.st_size;
    s->opened = 1;

#ifdef HAVE_MMAP
    if (!options_get_bool("UseMMap"))
    {
      logit("Not using mmap()");
      s->mem = nullptr;
      break;
    }

    s->mem = io_mmap_file(s);
    if (!s->mem)
    {
      break;
    }

    s->source = IO_SOURCE_MMAP;
    s->mem_pos = 0;
#endif
  } while (false);
}

/* Open the file. */
struct io_stream *io_open(const char *file, const int buffered)
{
  struct io_stream *s;

  assert(file != nullptr);

  s = new io_stream;
  s->errno_val = 0;
  s->read_error = 0;
  s->strerror = "";
  s->opened = 0;
  s->size = -1;
  s->buf_fill_callback = nullptr;

  io_open_file(s, file);

  if (!s->opened)
  {
    return s;
  }

  s->stop_read_thread = 0;
  s->eof = 0;
  s->after_seek = 0;
  s->buffered = buffered;
  s->pos = 0;

  if (buffered)
  {
    s->buf = new fifo_buf(options_get_int("InputBuffer") * 1024);
    s->read_thread = std::thread(io_read_thread, s);
  }

  return s;
}

/* Return non-zero if the stream was free of errors. */
static int io_ok_nolock(struct io_stream *s)
{
  return !s->read_error && s->errno_val == 0;
}

/* Return non-zero if the stream was free of errors. */
int io_ok(struct io_stream *s)
{
  int res;

  std::lock_guard<std::mutex> lock(s->buf_mtx);
  res = io_ok_nolock(s);

  return res;
}

/* Read data from the buffer without removing them, so stream position is
 * unchanged. You can't peek more data than the buffer size. */
static ssize_t io_peek_internal(struct io_stream *s, void *buf, size_t count)
{
  ssize_t received = 0;

  debug("Peeking data...");

  std::unique_lock<std::mutex> lock(s->buf_mtx);

  /* Wait until enough data will be available */
  while (io_ok_nolock(s) && !s->stop_read_thread &&
         count > s->buf->fill() && s->buf->space() &&
         !s->eof)
  {
    debug("waiting...");
    s->buf_fill_cond.wait(lock);
  }

  received = s->buf->peek(static_cast<char *>(buf), count);
  debug("Read %zd bytes", received);

  lock.unlock();

  return io_ok(s) ? received : -1;
}

static ssize_t io_read_buffered(struct io_stream *s, void *buf, size_t count)
{
  ssize_t received = 0;

  std::unique_lock<std::mutex> lock(s->buf_mtx);

  while (received < static_cast<ssize_t>(count) && !s->stop_read_thread &&
         ((!s->eof && !s->read_error) || s->buf->fill()))
  {
    if (s->buf->fill())
    {
      received +=
          s->buf->get(static_cast<char *>(buf) + received, count - received);
      debug("Read %zd bytes so far", received);
      s->buf_free_cond.notify_one();
      continue;
    }

    debug("Buffer empty, waiting...");
    s->buf_fill_cond.wait(lock);
  }

  debug("done");
  s->pos += received;

  return received ? received : (s->read_error ? -1 : 0);
}

/* Read data from the stream without buffering. If dont_move was set, the
 * stream position is unchanged. */
static ssize_t io_read_unbuffered(struct io_stream *s, const int dont_move,
                                  void *buf, size_t count)
{
  ssize_t res;

  assert(!s->eof);

  res = io_internal_read(s, dont_move, static_cast<char *>(buf), count);

  if (!dont_move)
  {
    s->pos += res;
    if (res == 0)
    {
      s->eof = 1;
    }
  }

  return res;
}

/* Read data from the stream to the buffer of size count.  Return the number
 * of bytes read, 0 on EOF, < 0 on error. */
ssize_t io_read(struct io_stream *s, void *buf, size_t count)
{
  ssize_t received;

  assert(s != nullptr);
  assert(buf != nullptr);
  assert(s->opened);

  debug("Reading...");

  if (s->buffered)
  {
    received = io_read_buffered(s, buf, count);
  }
  else if (s->eof)
  {
    received = 0;
  }
  else
  {
    received = io_read_unbuffered(s, 0, buf, count);
  }

  return received;
}

/* Read data from the stream to the buffer of size count. The data are not
 * removed from the stream. Return the number of bytes read, 0 on EOF, < 0
 * on error. */
ssize_t io_peek(struct io_stream *s, void *buf, size_t count)
{
  ssize_t received;

  assert(s != nullptr);
  assert(buf != nullptr);

  debug("Reading...");

  if (s->buffered)
  {
    received = io_peek_internal(s, buf, count);
  }
  else
  {
    received = io_read_unbuffered(s, 1, buf, count);
  }

  return io_ok(s) ? received : -1;
}

/* Get the string describing the error associated with the stream. */
const char *io_strerror(struct io_stream *s)
{
  if (s->errno_val)
    s->strerror = xstrerror(s->errno_val);
  else
    s->strerror = "OK";
  return s->strerror.c_str();
}

/* Get the file size if available or -1. */
off_t io_file_size(const struct io_stream *s)
{
  assert(s != nullptr);

  return s->size;
}

/* Return the stream position. */
off_t io_tell(struct io_stream *s)
{
  off_t res = -1;

  assert(s != nullptr);

  if (s->buffered)
  {
    std::lock_guard<std::mutex> lock(s->buf_mtx);
    res = s->pos;
  }
  else
  {
    res = s->pos;
  }

  debug("We are at byte %" PRId64, res);

  return res;
}

/* Return != 0 if we are at the end of the stream. */
int io_eof(struct io_stream *s)
{
  int eof;

  assert(s != nullptr);

  std::lock_guard<std::mutex> lock(s->buf_mtx);
  eof = (s->eof && (!s->buffered || !s->buf->fill())) ||
        s->stop_read_thread;

  return eof;
}

void io_init()
{
}

void io_cleanup()
{
}

/* Set the callback function to be invoked when the fill of the buffer
 * changes.  data_ptr is a pointer passed to this function along with
 * the pointer to the stream. */
void io_set_buf_fill_callback(struct io_stream *s, buf_fill_callback_t callback,
                              void *data_ptr)
{
  assert(s != nullptr);
  assert(callback != nullptr);

  std::lock_guard<std::mutex> lock(s->buf_mtx);
  s->buf_fill_callback = callback;
  s->buf_fill_callback_data = data_ptr;
}

/* Return a non-zero value if the stream is seekable. */
int io_seekable(const struct io_stream *s)
{
  return s->source == IO_SOURCE_FD || s->source == IO_SOURCE_MMAP;
}

// EOF
