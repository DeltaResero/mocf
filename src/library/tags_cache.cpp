// src/library/tags_cache.cpp
// SPDX-License-Identifier: GPL-3.0-or-later
//
// mocf - Music on Console Framebuffer
// Copyright (C) 2005, 2006 Damian Pietras <daper@daper.net>
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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>
#include <deque>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HAVE_DB_H
#ifndef HAVE_U_INT
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long int u_long;
#endif
#include <db.h>
#define STRERROR_FN bdb_strerror
#endif

#define DEBUG

#include "core/common.h"
#include "core/server.h"
#include "library/playlist.h"

#include "library/files.h"
#include "library/tags_cache.h"
#include "core/log.h"
#include "audio/audio.h"

#ifdef HAVE_DB_H
#define DB_ONLY
#else
#define DB_ONLY ATTR_UNUSED
#endif

/* The name of the tags database in the cache directory. */
#define TAGS_DB "tags.db"

/* The name of the version tag file in the cache directory. */
#define MOC_VERSION_TAG "mocf_version_tag"

/* The maximum length of the version tag (including trailing nullptr). */
#define VERSION_TAG_MAX 64

/* Number used to create cache version tag to detect incompatibilities
 * between cache version stored on the disk and MOC/BerkeleyDB environment.
 *
 * If you modify the DB structure, increase this number.  You can also
 * temporarily set it to zero to disable cache activity during structural
 * changes which require multiple commits.
 */
#define CACHE_DB_FORMAT_VERSION 3

/* How frequently to flush the tags database to disk.  A value of zero
 * disables flushing. */
#define DB_SYNC_COUNT 5

/* Element of a requests queue. */
struct TagRequest {
    std::string file;
    int tags_sel;
};

struct tags_cache
{
  /* BerkeleyDB's stuff for storing cache. */
#ifdef HAVE_DB_H
  DB_ENV *db_env;
  DB *db;
  u_int32_t locker;
#endif

  int max_items; /* maximum number of items in the cache. */
  std::deque<TagRequest> queue; /* pending tag requests */
  int stop_reader_thread;      /* request for stopping read thread (if
                non-zero) */
  std::condition_variable request_cond; /* condition for signalizing new
          requests */
  std::mutex mutex;       /* mutex for all above data (except db because
                it's thread-safe) */
  std::thread reader_thread;     /* tid of the reading thread */
};

struct cache_record
{
  time_t mod_time; /* last modification time of the file */
  time_t atime;    /* Time of last access. */
  struct file_tags *tags;
};

/* BerkleyDB-provided error code to description function wrapper. */
#ifdef HAVE_DB_H
static inline char *bdb_strerror(int errnum)
{
  char *result;

  if (errnum > 0)
  {
    result = xstrdup(xstrerror(errnum).c_str());
  }
  else
  {
    result = xstrdup(db_strerror(errnum));
  }

  return result;
}
#endif


#ifdef HAVE_DB_H
static char *cache_record_serialize(const struct cache_record *rec, int *len)
{
  char *buf;
  char *p;
  size_t artist_len;
  size_t album_len;
  size_t title_len;

  artist_len = rec->tags->artist.size();
  album_len  = rec->tags->album.size();
  title_len  = rec->tags->title.size();

  *len = sizeof(rec->mod_time) + sizeof(rec->atime) +
         sizeof(size_t) * 3 /* lengths of title, artist, time. */
         + artist_len + album_len + title_len + sizeof(rec->tags->track)
         + sizeof(rec->tags->time);

  buf = p = static_cast<char *>(xmalloc(*len));

  memcpy(p, &rec->mod_time, sizeof(rec->mod_time));
  p += sizeof(rec->mod_time);

  memcpy(p, &rec->atime, sizeof(rec->atime));
  p += sizeof(rec->atime);

  memcpy(p, &artist_len, sizeof(artist_len));
  p += sizeof(artist_len);
  if (artist_len)
  {
    std::memcpy(p, rec->tags->artist.c_str(), artist_len);
    p += artist_len;
  }

  memcpy(p, &album_len, sizeof(album_len));
  p += sizeof(album_len);
  if (album_len)
  {
    std::memcpy(p, rec->tags->album.c_str(), album_len);
    p += album_len;
  }

  memcpy(p, &title_len, sizeof(title_len));
  p += sizeof(title_len);
  if (title_len)
  {
    std::memcpy(p, rec->tags->title.c_str(), title_len);
    p += title_len;
  }

  memcpy(p, &rec->tags->track, sizeof(rec->tags->track));
  p += sizeof(rec->tags->track);

  memcpy(p, &rec->tags->time, sizeof(rec->tags->time));
  p += sizeof(rec->tags->time);

  return buf;
}
#endif

#ifdef HAVE_DB_H
static int cache_record_deserialize(struct cache_record *rec,
                                    const char *serialized, size_t size,
                                    int skip_tags)
{
  const char *p = serialized;
  size_t bytes_left = size;
  size_t str_len;

  assert(rec != nullptr);
  assert(serialized != nullptr);

  if (!skip_tags)
  {
    rec->tags = new file_tags{};
  }
  else
  {
    rec->tags = nullptr;
  }

#define extract_num(var)                                                       \
  do                                                                           \
  {                                                                            \
    if (bytes_left < sizeof(var))                                              \
      goto err;                                                                \
    memcpy(&var, p, sizeof(var));                                              \
    bytes_left -= sizeof(var);                                                 \
    p += sizeof(var);                                                          \
  } while (0)

#define extract_str(var)                                                       \
  do                                                                           \
  {                                                                            \
    if (bytes_left < sizeof(str_len))                                          \
      goto err;                                                                \
    memcpy(&str_len, p, sizeof(str_len));                                      \
    p += sizeof(str_len);                                                      \
    if (bytes_left < str_len)                                                  \
      goto err;                                                                \
    (var).assign(p, str_len);                                                  \
    p += str_len;                                                              \
  } while (0)

  extract_num(rec->mod_time);
  extract_num(rec->atime);

  if (!skip_tags)
  {
    extract_str(rec->tags->artist);
    extract_str(rec->tags->album);
    extract_str(rec->tags->title);
    extract_num(rec->tags->track);
    extract_num(rec->tags->time);

    if (!rec->tags->title.empty())
    {
      rec->tags->filled |= TAGS_COMMENTS;
    }
    else
    {
      rec->tags->artist.clear();
      rec->tags->album.clear();
    }

    if (rec->tags->time >= 0)
    {
      rec->tags->filled |= TAGS_TIME;
    }
  }

  return 1;

err:
  logit("Cache record deserialization error at %tdB", p - serialized);
  delete rec->tags;
  rec->tags = nullptr;
  return 0;
}
#endif

/* Locked DB function prototype.
 * The function must not acquire or release DB locks. */
#ifdef HAVE_DB_H
typedef void *t_locked_fn(struct tags_cache *, const char *, int, int, DBT *,
                          DBT *);
#endif

/* This function ensures that a DB function takes place while holding a
 * database record lock.  It also provides an initialised database thang
 * for the key and record. */
#ifdef HAVE_DB_H
static void *with_db_lock(t_locked_fn fn, struct tags_cache *c,
                          const char *file, int tags_sel, int notify)
{
  int rc;
  void *result;
  DB_LOCK lock;
  DBT key, record;

  assert(c->db_env != nullptr);

  memset(&key, 0, sizeof(key));
  key.data = const_cast<char *>(file);
  key.size = strlen(file);

  memset(&record, 0, sizeof(record));
  record.flags = DB_DBT_MALLOC;

  rc = c->db_env->lock_get(c->db_env, c->locker, 0, &key, DB_LOCK_WRITE, &lock);
  if (rc)
  {
    fatal("Can't get DB lock: %s", db_strerror(rc));
  }

  result = fn(c, file, tags_sel, notify, &key, &record);

  rc = c->db_env->lock_put(c->db_env, &lock);
  if (rc)
  {
    fatal("Can't release DB lock: %s", db_strerror(rc));
  }

  if (record.data)
  {
    free(record.data);
  }

  return result;
}
#endif

#ifdef HAVE_DB_H
static void tags_cache_remove_rec(struct tags_cache *c, const char *fname)
{
  DBT key;
  int ret;

  assert(fname != nullptr);

  debug("Removing %s from the cache...", fname);

  memset(&key, 0, sizeof(key));
  key.data = const_cast<char *>(fname);
  key.size = strlen(fname);

  ret = c->db->del(c->db, nullptr, &key, 0);
  if (ret)
  {
    logit("Can't remove item for %s from the cache: %s", fname,
          db_strerror(ret));
  }
}
#endif

/* Remove the one element of the cache based on it's access time. */
#ifdef HAVE_DB_H
static void tags_cache_gc(struct tags_cache *c)
{
  DBC *cur;
  DBT key;
  DBT serialized_cache_rec;
  int ret;
  std::optional<std::string> last_referenced;
  time_t last_referenced_atime = time(nullptr) + 1;
  int nitems = 0;

  c->db->cursor(c->db, nullptr, &cur, 0);

  memset(&key, 0, sizeof(key));
  memset(&serialized_cache_rec, 0, sizeof(serialized_cache_rec));

  key.flags = DB_DBT_MALLOC;
  serialized_cache_rec.flags = DB_DBT_MALLOC;

  while (true)
  {
    struct cache_record rec;

#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR < 6
    ret = cur->c_get(cur, &key, &serialized_cache_rec, DB_NEXT);
#else
    ret = cur->get(cur, &key, &serialized_cache_rec, DB_NEXT);
#endif

    if (ret != 0)
    {
      break;
    }

    if (cache_record_deserialize(&rec, static_cast<const char *>(serialized_cache_rec.data),
                                 serialized_cache_rec.size, 1) &&
        rec.atime < last_referenced_atime)
    {
      last_referenced_atime = rec.atime;
      last_referenced = std::string(static_cast<const char *>(key.data),
                                    key.size);
    }

    // TODO: remove objects with serialization error.

    nitems++;

    free(key.data);
    free(serialized_cache_rec.data);
  }

  if (ret != DB_NOTFOUND)
  {
    log_errno("Searching for element to remove failed (cursor)", ret);
  }

#if DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR < 6
  cur->c_close(cur);
#else
  cur->close(cur);
#endif

  debug("Elements in cache: %d (limit %d)", nitems, c->max_items);

  if (last_referenced)
  {
    if (nitems >= c->max_items)
    {
      tags_cache_remove_rec(c, last_referenced->c_str());
    }
  }
  else
  {
    debug("Cache empty");
  }
}
#endif

/* Synchronize cache every DB_SYNC_COUNT updates. */
#ifdef HAVE_DB_H
static void tags_cache_sync(struct tags_cache *c)
{
  static int sync_count = 0;

  if (DB_SYNC_COUNT == 0)
  {
    return;
  }

  sync_count += 1;
  if (sync_count >= DB_SYNC_COUNT)
  {
    sync_count = 0;
    c->db->sync(c->db, 0);
  }
}
#endif

/* Add this tags object for the file to the cache. */
#ifdef HAVE_DB_H
static void tags_cache_add(struct tags_cache *c, const char *file, DBT *key,
                           struct file_tags *tags)
{
  char *serialized_cache_rec;
  int serial_len;
  struct cache_record rec;
  DBT data;
  int ret;

  assert(tags != nullptr);

  debug("Adding/updating cache object");

  rec.mod_time = get_mtime(file);
  rec.atime = time(nullptr);
  rec.tags = tags;

  serialized_cache_rec = cache_record_serialize(&rec, &serial_len);
  if (!serialized_cache_rec)
  {
    return;
  }

  memset(&data, 0, sizeof(data));
  data.data = serialized_cache_rec;
  data.size = serial_len;

  tags_cache_gc(c);

  ret = c->db->put(c->db, nullptr, key, &data, 0);
  if (ret)
  {
    error_errno("DB put error", ret);
  }

  tags_cache_sync(c);

  free(serialized_cache_rec);
}
#endif

/* Read time tags for a file into tags structure (or create it if nullptr). */
struct file_tags *read_missing_tags(const char *file, struct file_tags *tags,
                                    int tags_sel)
{
  if (tags == nullptr)
  {
    tags = new file_tags{};
  }

  if (tags_sel & TAGS_TIME)
  {
    int time;

    /* Try to get it from the server's playlist first. */
    time = audio_get_ftime(file);

    if (time != -1)
    {
      tags->time = time;
      tags->filled |= TAGS_TIME;
      tags_sel &= ~TAGS_TIME;
    }
  }

  tags = read_file_tags(file, tags, tags_sel);

  return tags;
}

/* Read the selected tags for this file and add it to the cache. */
#ifdef HAVE_DB_H
static void *locked_read_add(struct tags_cache *c, const char *file,
                             const int tags_sel, const int notify, DBT *key,
                             DBT *serialized_cache_rec)
{
  int ret;
  struct file_tags *tags = nullptr;

  assert(c->db != nullptr);

  ret = c->db->get(c->db, nullptr, key, serialized_cache_rec, 0);
  if (ret && ret != DB_NOTFOUND)
  {
    log_errno("Cache DB get error", ret);
  }

  /* If this entry is already present in the cache, we have 3 options:
   * we must read different tags (TAGS_*) or the tags are outdated
   * or this is a synchronous (non-notify) read */
  if (ret == 0)
  {
    struct cache_record rec;

    if (cache_record_deserialize(&rec, static_cast<const char *>(serialized_cache_rec->data),
                                 serialized_cache_rec->size, 0))
    {
      time_t curr_mtime = get_mtime(file);

      if (rec.mod_time != curr_mtime)
      {
        debug("Tags in the cache are outdated");
        delete rec.tags; /* remove them and reread tags */
      }
      else if ((rec.tags->filled & tags_sel) == tags_sel && !notify)
      {
        debug("Tags are in the cache.");
        return rec.tags;
      }
      else
      {
        debug("Tags in the cache are not what we want");
        tags = rec.tags; /* read additional tags */
      }
    }
  }

  tags = read_missing_tags(file, tags, tags_sel);
  tags_cache_add(c, file, key, tags);

  return tags;
}
#endif

/* Read the selected tags for this file and add it to the cache.
 * If notify is true, the server is notified using tags_response().
 * If notify is false, a copy of file_tags is returned. */
static struct file_tags *tags_cache_read_add(struct tags_cache *c DB_ONLY,
                                             const char *file, int tags_sel,
                                             int notify)
{
  struct file_tags *tags = nullptr;

  assert(file != nullptr);

  debug("Getting tags for %s", file);

#ifdef HAVE_DB_H
  if (c->max_items)
  {
    tags = static_cast<struct file_tags *>(with_db_lock(locked_read_add, c, file, tags_sel,
                                            notify));
  }
  else
#endif
    tags = read_missing_tags(file, tags, tags_sel);

  if (notify)
  {
    tags_response(file, tags);
    delete tags;
    tags = nullptr;
  }

  /* TODO: Remove the oldest items from the cache if we exceeded the maximum
   * cache size */

  return tags;
}

static void reader_thread(struct tags_cache *c)
{
  logit("Tags reader thread started");

  std::unique_lock<std::mutex> lock(c->mutex);

  while (!c->stop_reader_thread)
  {
    std::string request_file;
    int tags_sel = 0;

    if (c->queue.empty())
    {
      debug("Queue empty, waiting");
      c->request_cond.wait(lock);
      continue;
    }

    request_file = c->queue.front().file;
    tags_sel = c->queue.front().tags_sel;
    c->queue.pop_front();
    lock.unlock();

    if (!request_file.empty())
      tags_cache_read_add(c, request_file.c_str(), tags_sel, 1);

    lock.lock();
  }

  logit("Exiting tags reader thread");
}

struct tags_cache *tags_cache_new(size_t max_size)
{
  struct tags_cache *result;

  result = new tags_cache;

#ifdef HAVE_DB_H
  result->db_env = nullptr;
  result->db = nullptr;
#endif


#if CACHE_DB_FORMAT_VERSION
  result->max_items = max_size;
#else
  result->max_items = 0;
#endif
  result->stop_reader_thread = 0;

  result->reader_thread = std::thread(reader_thread, result);

  return result;
}

void tags_cache_free(struct tags_cache *c)
{
  assert(c != nullptr);

  {
    std::lock_guard<std::mutex> lock(c->mutex);
    c->stop_reader_thread = 1;
    c->request_cond.notify_one();
  }

#ifdef HAVE_DB_H
  if (c->db)
  {
#ifndef NDEBUG
    c->db->set_errcall(c->db, nullptr);
    c->db->set_msgcall(c->db, nullptr);
    c->db->set_paniccall(c->db, nullptr);
#endif
    c->db->close(c->db, 0);
    c->db = nullptr;
  }
#endif

#ifdef HAVE_DB_H
  if (c->db_env)
  {
    c->db_env->lock_id_free(c->db_env, c->locker);
#ifndef NDEBUG
    c->db_env->set_errcall(c->db_env, nullptr);
    c->db_env->set_msgcall(c->db_env, nullptr);
    c->db_env->set_paniccall(c->db_env, nullptr);
#endif
    c->db_env->close(c->db_env, 0);
    c->db_env = nullptr;
  }
#endif

  if (c->reader_thread.joinable()) c->reader_thread.join();

  c->queue.clear();

  delete c;
}

#ifdef HAVE_DB_H
static void *locked_add_request(struct tags_cache *c, const char *file,
                                int tags_sel, int notify, DBT *key,
                                DBT *serialized_cache_rec)
{
  int db_ret;
  struct cache_record rec;

  assert(c->db);

  db_ret = c->db->get(c->db, nullptr, key, serialized_cache_rec, 0);

  if (db_ret == DB_NOTFOUND)
  {
    return nullptr;
  }

  if (db_ret)
  {
    error_errno("Cache DB search error", db_ret);
    return nullptr;
  }

  if (cache_record_deserialize(&rec, static_cast<const char *>(serialized_cache_rec->data),
                               serialized_cache_rec->size, 0))
  {
    if (rec.mod_time == get_mtime(file) &&
        (rec.tags->filled & tags_sel) == tags_sel)
    {
      tags_response(file, rec.tags);
      delete rec.tags;
      debug("Tags are present in the cache");
      return (void *)1;
    }

    delete rec.tags;
    debug("Found outdated or incomplete tags in the cache");
  }

  return nullptr;
}
#endif

void tags_cache_add_request(struct tags_cache *c, const char *file,
                            int tags_sel)
{
  void *rc = nullptr;

  assert(c != nullptr);
  assert(file != nullptr);

  debug("Request for tags for '%s'", file);

#ifdef HAVE_DB_H
  if (c->max_items)
  {
    rc = with_db_lock(locked_add_request, c, file, tags_sel, 1);
  }
#endif

  if (!rc)
  {
    std::lock_guard<std::mutex> lock(c->mutex);
    c->queue.push_back({file, tags_sel});
    c->request_cond.notify_one();
  }
}

void tags_cache_clear_queue(struct tags_cache *c)
{
  assert(c != nullptr);

  std::lock_guard<std::mutex> lock(c->mutex);
  c->queue.clear();
  debug("Cleared tags request queue");
}

/* Remove all pending requests from the queue up to the request associated
 * with the given file. */
void tags_cache_clear_up_to(struct tags_cache *c, const char *file)
{
  assert(c != nullptr);
  assert(file != nullptr);

  std::lock_guard<std::mutex> lock(c->mutex);
  debug("Removing requests up to file %s", file);
  while (!c->queue.empty())
  {
    std::string f = c->queue.front().file;
    c->queue.pop_front();
    if (f == file) break;
  }
}

#if defined(HAVE_DB_H) && !defined(NDEBUG)
static void db_err_cb(const DB_ENV *unused ATTR_UNUSED, const char *errpfx,
                      const char *msg)
{
  assert(msg);

  if (errpfx && errpfx[0])
  {
    logit("BDB said: %s: %s", errpfx, msg);
  }
  else
  {
    logit("BDB said: %s", msg);
  }
}
#endif

#if defined(HAVE_DB_H) && !defined(NDEBUG)
static void db_msg_cb(const DB_ENV *unused ATTR_UNUSED, const char *msg_pfx,
                      const char *msg)
{
  assert(msg);

  if (msg_pfx && msg_pfx[0])
  {
    logit("BDB said: %s: %s", msg_pfx, msg);
  }
  else
  {
    logit("BDB said: %s", msg);
  }
}
#endif

#if defined(HAVE_DB_H) && !defined(NDEBUG)
static void db_panic_cb(DB_ENV *unused ATTR_UNUSED, int errval)
{
  log_errno("BDB said", errval);
}
#endif

/* Purge content of a directory. */
#ifdef HAVE_DB_H
static int purge_directory(const char *dir_path)
{
  DIR *dir;
  struct dirent *d;

  logit("Purging %s...", dir_path);

  dir = opendir(dir_path);
  if (!dir)
  {
    std::string err = xstrerror(errno);
    logit("Can't open directory %s: %s", dir_path, err.c_str());
    return 0;
  }

  while ((d = readdir(dir)))
  {
    struct stat st;

    if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
    {
      continue;
    }

    std::string fpath = std::string(dir_path) + "/" + d->d_name;

    if (stat(fpath.c_str(), &st) < 0)
    {
      std::string err = xstrerror(errno);
      logit("Can't stat %s: %s", fpath.c_str(), err.c_str());
      closedir(dir);
      return 0;
    }

    if (S_ISDIR(st.st_mode))
    {
      if (!purge_directory(fpath.c_str()))
      {
        closedir(dir);
        return 0;
      }

      logit("Removing directory %s...", fpath.c_str());
      if (rmdir(fpath.c_str()) < 0)
      {
        std::string err = xstrerror(errno);
        logit("Can't remove %s: %s", fpath.c_str(), err.c_str());
        closedir(dir);
        return 0;
      }
    }
    else
    {
      logit("Removing file %s...", fpath.c_str());

      if (unlink(fpath.c_str()) < 0)
      {
        std::string err = xstrerror(errno);
        logit("Can't remove %s: %s", fpath.c_str(), err.c_str());
        closedir(dir);
        return 0;
      }
    }
  }

  closedir(dir);
  return 1;
}
#endif

/* Create a MOC/db version string.
 *
 * @param buf Output buffer (at least VERSION_TAG_MAX chars long)
 */
#ifdef HAVE_DB_H
static const char *create_version_tag(char *buf)
{
  int db_major;
  int db_minor;

  db_version(&db_major, &db_minor, nullptr);

#ifdef PACKAGE_REVISION
  snprintf(buf, VERSION_TAG_MAX, "%d %d %d r%s", CACHE_DB_FORMAT_VERSION,
           db_major, db_minor, PACKAGE_REVISION);
#else
  snprintf(buf, VERSION_TAG_MAX, "%d %d %d", CACHE_DB_FORMAT_VERSION, db_major,
           db_minor);
#endif

  return buf;
}
#endif

/* Check version of the cache directory.  If it was created
 * using format not handled by this version of MOC, return 0. */
#ifdef HAVE_DB_H
static int cache_version_matches(const char *cache_dir)
{
  char disk_version_tag[VERSION_TAG_MAX];
  ssize_t rres;
  FILE *f;
  int compare_result = 0;

  std::string fname =
      std::string(cache_dir) + "/" + MOC_VERSION_TAG;

  f = fopen(fname.c_str(), "r");
  if (!f)
  {
    logit("No %s in cache directory", MOC_VERSION_TAG);
    return 0;
  }

  rres = fread(disk_version_tag, 1, sizeof(disk_version_tag) - 1, f);
  if (rres == sizeof(disk_version_tag) - 1)
  {
    logit("On-disk version tag too long");
  }
  else
  {
    char *ptr, cur_version_tag[VERSION_TAG_MAX];

    disk_version_tag[rres] = '\0';
    ptr = strrchr(disk_version_tag, '\n');
    if (ptr)
    {
      *ptr = '\0';
    }
    ptr = strrchr(disk_version_tag, ' ');
    if (ptr && ptr[1] == 'r')
    {
      *ptr = '\0';
    }

    create_version_tag(cur_version_tag);
    ptr = strrchr(cur_version_tag, '\n');
    if (ptr)
    {
      *ptr = '\0';
    }
    ptr = strrchr(cur_version_tag, ' ');
    if (ptr && ptr[1] == 'r')
    {
      *ptr = '\0';
    }

    compare_result = !strcmp(disk_version_tag, cur_version_tag);
  }

  fclose(f);

  return compare_result;
}
#endif

#ifdef HAVE_DB_H
static void write_cache_version(const char *cache_dir)
{
  char cur_version_tag[VERSION_TAG_MAX];
  FILE *f;
  int rc;

  std::string fname = std::string(cache_dir) + "/" + MOC_VERSION_TAG;

  f = fopen(fname.c_str(), "w");
  if (!f)
  {
    log_errno("Error opening cache", errno);
    return;
  }

  create_version_tag(cur_version_tag);
  rc = fwrite(cur_version_tag, strlen(cur_version_tag), 1, f);
  if (rc != 1)
  {
    logit("Error writing cache version tag: %d", rc);
  }

  fclose(f);
}
#endif

/* Make sure that the cache directory exists and clear it if necessary. */
#ifdef HAVE_DB_H
static int prepare_cache_dir(const char *cache_dir)
{
  if (mkdir(cache_dir, 0700) == 0)
  {
    write_cache_version(cache_dir);
    return 1;
  }

  if (errno != EEXIST)
  {
    error_errno("Failed to create directory for tags cache", errno);
    return 0;
  }

  if (!cache_version_matches(cache_dir))
  {
    logit("Tags cache directory is the wrong version, purging....");

    if (!purge_directory(cache_dir))
    {
      return 0;
    }
    write_cache_version(cache_dir);
  }

  return 1;
}
#endif

void tags_cache_load(struct tags_cache *c DB_ONLY,
                     const char *cache_dir DB_ONLY)
{
  assert(c != nullptr);
  assert(cache_dir != nullptr);

#ifdef HAVE_DB_H
  int ret;

  if (!c->max_items)
  {
    return;
  }

  if (!prepare_cache_dir(cache_dir))
  {
    error("Can't prepare cache directory!");
    goto err;
  }

  ret = db_env_create(&c->db_env, 0);
  if (ret)
  {
    error_errno("Can't create DB environment", ret);
    goto err;
  }

#ifndef NDEBUG
  c->db_env->set_errcall(c->db_env, db_err_cb);
  c->db_env->set_msgcall(c->db_env, db_msg_cb);
  ret = c->db_env->set_paniccall(c->db_env, db_panic_cb);
  if (ret)
  {
    logit("Could not set DB panic callback");
  }
#endif

  ret = c->db_env->open(
      c->db_env, cache_dir,
      DB_CREATE | DB_PRIVATE | DB_INIT_MPOOL | DB_THREAD | DB_INIT_LOCK, 0);
  if (ret)
  {
    error("Can't open DB environment (%s): %s", cache_dir, db_strerror(ret));
    goto err;
  }

  ret = c->db_env->lock_id(c->db_env, &c->locker);
  if (ret)
  {
    error_errno("Failed to get DB locker", ret);
    goto err;
  }

  ret = db_create(&c->db, c->db_env, 0);
  if (ret)
  {
    error_errno("Failed to create cache db", ret);
    goto err;
  }

#ifndef NDEBUG
  c->db->set_errcall(c->db, db_err_cb);
  c->db->set_msgcall(c->db, db_msg_cb);
  ret = c->db->set_paniccall(c->db, db_panic_cb);
  if (ret)
  {
    logit("Could not set DB panic callback");
  }
#endif

  ret = c->db->open(c->db, nullptr, TAGS_DB, nullptr, DB_BTREE, DB_CREATE | DB_THREAD,
                    0);
  if (ret)
  {
    error_errno("Failed to open (or create) tags cache db", ret);
    goto err;
  }

  return;

err:
  if (c->db)
  {
#ifndef NDEBUG
    c->db->set_errcall(c->db, nullptr);
    c->db->set_msgcall(c->db, nullptr);
    c->db->set_paniccall(c->db, nullptr);
#endif
    c->db->close(c->db, 0);
    c->db = nullptr;
  }
  if (c->db_env)
  {
#ifndef NDEBUG
    c->db_env->set_errcall(c->db_env, nullptr);
    c->db_env->set_msgcall(c->db_env, nullptr);
    c->db_env->set_paniccall(c->db_env, nullptr);
#endif
    c->db_env->close(c->db_env, 0);
    c->db_env = nullptr;
  }
  c->max_items = 0;
  error("Failed to initialise tags cache: caching disabled");
#endif
}

/* Immediately read tags for a file bypassing the request queue. */
struct file_tags *tags_cache_get_immediate(struct tags_cache *c,
                                           const char *file, int tags_sel)
{
  struct file_tags *tags;

  assert(c != nullptr);
  assert(file != nullptr);

  debug("Immediate tags read for %s", file);

  tags = tags_cache_read_add(c, file, tags_sel, 0);

  return tags;
}

// EOF
