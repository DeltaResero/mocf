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
#include <functional>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <memory>

using out_buf_free_callback = std::function<void()>;

class OutBuf
{
public:
  OutBuf(int size);
  ~OutBuf();

  int put(const char *data, int size);
  void pause();
  void unpause();
  void stop();
  void reset();
  void time_set(const float time);
  int time_get();
  void set_free_callback(out_buf_free_callback callback);
  int get_free();
  int get_fill();
  void wait();

private:
  void read_thread();

  std::unique_ptr<fifo_buf> fifo;
  std::mutex mutex;
  std::thread tid;

  std::condition_variable play_cond;
  std::condition_variable ready_cond;

  out_buf_free_callback free_callback;

  bool is_paused;
  bool is_exit;
  bool is_stopped;
  bool reset_dev;

  float time;
  int hardware_buf_fill;
  bool read_thread_waiting;
};

#endif

// EOF
