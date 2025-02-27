/*
 * MIT License
 *
 * Copyright (c) 2023 Frédéric Bour <frederic.bour@lakaban.net>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "sprotocol.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

#define PACK(a,b,c,d) ((d << 24) | (c << 16) | (b << 8) | a)
#define BUF_SIZE 4096

struct channel_s
{
  int fd;
  struct {
    char buffer[BUF_SIZE];
    int pos, len;
  } input;
  struct {
    char buffer[BUF_SIZE];
    int pos;
  } output;
  char *buf;
  int buf_size;
};

static int buffered_read_at_least(int fd, char *buf, int atleast, int size)
{
  int n;
  char *org = buf, *ok = buf + atleast;
  if (size < atleast) abort();
  while (1)
  {
    n = read(fd, buf, size);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      pabort();
    }

    buf += n;
    size -= n;
    if (buf >= ok)
      break;
  }
  return (buf - org);
}

static void read_all(int fd, char *buf, int size)
{
  while (size > 0)
  {
    int n = read(fd, buf, size);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      pabort();
    }
    if (n == 0)
      abort();

    buf += n;
    size -= n;
  }
}

static void write_all(int fd, const char *buf, int size)
{
  while (size > 0)
  {
    int n = write(fd, buf, size);
    if (n == -1)
    {
      if (errno == EINTR)
        continue;
      pabort();
    }
    if (n == 0)
      abort();

    buf += n;
    size -= n;
  }
}

static void cflush(channel_t *c)
{
  int pos = c->output.pos;
  if (pos == 0) return;
  write_all(c->fd, c->output.buffer, pos);
  c->output.pos = 0;
}

static void crefill(channel_t *c, int at_least)
{
  int avail = (c->input.len - c->input.pos);
  memmove(c->input.buffer, c->input.buffer + c->input.pos, avail);

  c->input.pos = 0;
  avail += buffered_read_at_least(c->fd, c->input.buffer + avail, at_least, BUF_SIZE - avail);
  c->input.len = avail;
}

/* HANDSHAKE */

#define HND_SERVER "TEXPRESSOS01"
#define HND_CLIENT "TEXPRESSOC01"

bool channel_handshake(channel_t *c)
{
  char answer[LEN(HND_CLIENT)];
  write_all(c->fd, HND_SERVER, LEN(HND_SERVER));
  read_all(c->fd, answer, LEN(HND_CLIENT));
  return (strncmp(HND_CLIENT, answer, LEN(HND_CLIENT)) == 0);
}

/* PROTOCOL DEFINITION */

#define CASE(K,X) case K##_##X: return STR(X)

const char *query_to_string(enum query q)
{
  switch (q)
  {
    CASE(Q,OPEN);
    CASE(Q,READ);
    CASE(Q,WRIT);
    CASE(Q,CLOS);
    CASE(Q,SIZE);
    CASE(Q,SEEN);
    CASE(Q,CHLD);
    CASE(Q,BACK);
    CASE(Q,ACCS);
    CASE(Q,STAT);
    CASE(Q,GPIC);
    CASE(Q,SPIC);
  }
}

const char *answer_to_string(enum answer q)
{
  switch (q)
  {
    CASE(A,DONE);
    CASE(A,PASS);
    CASE(A,SIZE);
    CASE(A,READ);
    CASE(A,FORK);
    CASE(A,ACCS);
    CASE(A,STAT);
    CASE(A,OPEN);
    CASE(A,GPIC);
  }
}

const char *ask_to_string(enum ask q)
{
  switch (q)
  {
    CASE(C,TERM);
    CASE(C,FLSH);
  }
}

channel_t *channel_new(int fd)
{
  channel_t *c = calloc(sizeof(channel_t), 1);
  if (!c) mabort();
  c->fd = fd;
  c->buf = malloc(256);
  if (!c->buf) mabort();
  c->buf_size = 256;
  return c;
}

void channel_free(channel_t *c)
{
  close(c->fd);
  free(c->buf);
  free(c);
}

static void resize_buf(channel_t *t)
{
  int old_size = t->buf_size;
  int new_size = old_size * 2;
  char *buf = malloc(new_size);
  if (!buf) mabort();
  memcpy(buf, t->buf, old_size);
  free(t->buf);
  t->buf = buf;
  t->buf_size = new_size;
}

static int cgetc(channel_t *t)
{
  if (t->input.pos == t->input.len)
    crefill(t, 1);
  return t->input.buffer[t->input.pos++];
}

static int read_zstr(channel_t *t, int *pos)
{
  int c, p0 = *pos;
  do {
    if (*pos == t->buf_size)
      resize_buf(t);
    c = cgetc(t);
    t->buf[*pos] = c;
    *pos += 1;
  } while (c != 0);
  return p0;
}

static void read_bytes(channel_t *t, int pos, int size)
{
  while (t->buf_size < pos + size)
    resize_buf(t);

  int ipos = t->input.pos, ilen = t->input.len;
  if (ipos + size <= ilen)
  {
    memcpy(&t->buf[pos], t->input.buffer + ipos, size);
    t->input.pos += size;
    return;
  }

  int isize = ilen - ipos;
  memcpy(&t->buf[pos], t->input.buffer + ipos, isize);
  pos += isize;
  size -= isize;
  t->input.pos = t->input.len = 0;
  read_all(t->fd, &t->buf[pos], size);
}

static void write_bytes(channel_t *t, void *buf, int size)
{
  if (t->output.pos + size <= BUF_SIZE)
  {
    memcpy(t->output.buffer + t->output.pos, buf, size);
    t->output.pos += size;
    return;
  }

  cflush(t);

  if (size > BUF_SIZE)
    write_all(t->fd, buf, size);
  else
  {
    memcpy(t->output.buffer, buf, size);
    t->output.pos = size;
  }
}

static bool try_read_u32(channel_t *t, uint32_t *tag)
{
  int avail = t->input.len - t->input.pos;

  if (avail == 0)
  {
    crefill(t, 0);
    avail = t->input.len - t->input.pos;
  }

  if (avail == 0)
    return 0;

  if (avail < 4)
    crefill(t, 4 - avail);

  memcpy(tag, t->input.buffer + t->input.pos, 4);
  t->input.pos += 4;
  return 1;
}

static uint32_t read_u32(channel_t *t)
{
  int avail = t->input.len - t->input.pos;

  if (avail < 4)
    crefill(t, 4 - avail);

  uint32_t tag;
  memcpy(&tag, t->input.buffer + t->input.pos, 4);
  t->input.pos += 4;

  return tag;
}

static void write_u32(channel_t *t, uint32_t u)
{
  write_bytes(t, &u, 4);
}

static float read_f32(channel_t *t)
{
  int avail = t->input.len - t->input.pos;

  if (avail < 4)
    crefill(t, 4 - avail);

  float f;
  memcpy(&f, t->input.buffer + t->input.pos, 4);
  t->input.pos += 4;

  return f;
}

static void write_f32(channel_t *t, float f)
{
  write_bytes(t, &f, 4);
}

void log_query(FILE *f, query_t *r)
{
  fprintf(f, "%04dms: ", r->time);
  switch (r->tag)
  {
    case Q_OPEN:
      {
        fprintf(f, "open(%d, \"%s\", \"%s\")\n", r->open.fid, r->open.path, r->open.mode);
        break;
      }
    case Q_READ:
      {
        fprintf(f, "read(%d, %d, %d)\n", r->read.fid, r->read.pos, r->read.size);
        break;
      }
    case Q_WRIT:
      {
        fprintf(f, "write(%d, %d, %d)\n",
            r->writ.fid, r->writ.pos, r->writ.size);
        break;
      }
    case Q_CLOS:
      {
        fprintf(f, "close(%d)\n", r->clos.fid);
        break;
      }
    case Q_SIZE:
      {
        fprintf(f, "size(%d)\n", r->size.fid);
        break;
      }
    case Q_SEEN:
      {
        fprintf(f, "seen(%d, %d)\n", r->seen.fid, r->seen.pos);
        break;
      }
    case Q_CHLD:
      {
        fprintf(f, "child(%d)\n", r->chld.pid);
        break;
      }
    case Q_BACK:
      {
        fprintf(f, "back(%d, %d, %d)\n", r->back.pid, r->back.cid, r->back.exitcode);
        break;
      }
    case Q_ACCS:
      {
        fprintf(f, "access(\"%s\", %d)\n", r->accs.path, r->accs.flags);
        break;
      }
    case Q_STAT:
      {
        fprintf(f, "stat(\"%s\")\n", r->stat.path);
        break;
      }
    case Q_GPIC:
      {
        fprintf(f, "gpic(\"%s\",%d,%d)\n", r->gpic.path, r->gpic.type, r->gpic.page);
        break;
      }
    case Q_SPIC:
      {
        fprintf(f, "spic(\"%s\", %d, %d, %.02f, %.02f, %.02f, %.02f)\n", 
                r->spic.path, 
                r->spic.cache.type, r->spic.cache.page,
                r->spic.cache.bounds[0], r->spic.cache.bounds[1],
                r->spic.cache.bounds[2], r->spic.cache.bounds[3]);
        break;
      }
    default:
      mabort();
  }
}

bool channel_has_pending_query(channel_t *t, int timeout)
{
  if (t->input.pos != t->input.len) return 1;

  struct pollfd pfd;
  int n;
  while(1)
  {
    pfd.fd = t->fd;
    pfd.events = POLLRDNORM;
    pfd.revents = 0;
    n = poll(&pfd, 1, timeout);
    if (!(n == -1 && errno == EINTR))
      break;
  }

  if (n == -1)
    pabort();
  if (n == 0)
    return 0;
  return 1;
}

bool channel_read_query(channel_t *t, query_t *r)
{
  uint32_t tag;

  if (!try_read_u32(t, &tag))
    return 0;
  r->tag = tag;
  r->time = read_u32(t);
  int pos = 0;
  switch (tag)
  {
    case Q_OPEN:
      {
        r->open.fid = read_u32(t);
        int pos_path = read_zstr(t, &pos);
        int pos_mode = read_zstr(t, &pos);
        r->open.path = &t->buf[pos_path];
        r->open.mode = &t->buf[pos_mode];
        break;
      }
    case Q_READ:
      {
        r->read.fid = read_u32(t);
        r->read.pos = read_u32(t);
        r->read.size = read_u32(t);
        break;
      }
    case Q_WRIT:
      {
        r->writ.fid = read_u32(t);
        r->writ.pos = read_u32(t);
        r->writ.size = read_u32(t);
        read_bytes(t, 0, r->writ.size);
        r->writ.buf = t->buf;
        break;
      }
    case Q_CLOS:
      {
        r->clos.fid = read_u32(t);
        break;
      }
    case Q_SIZE:
      {
        r->size.fid = read_u32(t);
        break;
      }
    case Q_SEEN:
      {
        r->seen.fid = read_u32(t);
        r->seen.pos = read_u32(t);
        break;
      }
    case Q_CHLD:
      {
        r->chld.pid = read_u32(t);
        break;
      }
    case Q_BACK:
      {
        r->back.pid = read_u32(t);
        r->back.cid = read_u32(t);
        r->back.exitcode = read_u32(t);
        break;
      }
    case Q_ACCS:
      {
        int pos_path = read_zstr(t, &pos);
        r->accs.path = &t->buf[pos_path];
        r->accs.flags = read_u32(t);
        break;
      }
    case Q_STAT:
      {
        int pos_path = read_zstr(t, &pos);
        r->stat.path = &t->buf[pos_path];
        break;
      }
    case Q_GPIC:
      {
        int pos_path = read_zstr(t, &pos);
        r->gpic.path = &t->buf[pos_path];
        r->gpic.type = read_u32(t);
        r->gpic.page = read_u32(t);
        break;
      }
    case Q_SPIC:
      {
        int pos_path = read_zstr(t, &pos);
        r->spic.path = &t->buf[pos_path];
        r->spic.cache.type = read_u32(t);
        r->spic.cache.page = read_u32(t);
        r->spic.cache.bounds[0] = read_f32(t);
        r->spic.cache.bounds[1] = read_f32(t);
        r->spic.cache.bounds[2] = read_f32(t);
        r->spic.cache.bounds[3] = read_f32(t);
        break;
      }
    default:
      mabort();
  }
  if (LOG)
  {
    fprintf(stderr, "[info] ");
    log_query(stderr, r);
  }
  return 1;
}

void channel_write_ask(channel_t *t, ask_t *a)
{
  write_u32(t, a->tag);
  switch (a->tag)
  {
    case C_TERM: write_u32(t, a->term.pid); break;
    case C_FLSH: break;
    default: mabort();
  }
}

static void write_time(channel_t *t, struct stat_time tm)
{
  write_u32(t, tm.sec);
  write_u32(t, tm.nsec);
}

void channel_write_answer(channel_t *t, answer_t *a)
{
  if (LOG)
  {
    if (a->tag == A_READ)
      fprintf(stderr, "[info] -> READ %d\n", a->read.size);
    else
      fprintf(stderr, "[info] -> %s\n", answer_to_string(a->tag));
  }
  write_u32(t, a->tag);
  switch (a->tag)
  {
    case A_DONE:
      break;
    case A_PASS:
      break;
    case A_FORK:
      break;
    case A_READ:
      write_u32(t, a->read.size);
      write_bytes(t, t->buf, a->read.size);
      break;
    case A_ACCS:
      write_u32(t, a->accs.flag);
      break;
    case A_STAT:
      write_u32(t, a->stat.flag);
      if (a->accs.flag == ACCS_OK)
      {
        write_u32(t, a->stat.stat.dev);
        write_u32(t, a->stat.stat.ino);
        write_u32(t, a->stat.stat.mode);
        write_u32(t, a->stat.stat.nlink);
        write_u32(t, a->stat.stat.uid);
        write_u32(t, a->stat.stat.gid);
        write_u32(t, a->stat.stat.rdev);
        write_u32(t, a->stat.stat.size);
        write_u32(t, a->stat.stat.blksize);
        write_u32(t, a->stat.stat.blocks);
        write_time(t, a->stat.stat.atime);
        write_time(t, a->stat.stat.ctime);
        write_time(t, a->stat.stat.mtime);
      }
      break;
    case A_SIZE:
      write_u32(t, a->size.size);
      break;
    case A_OPEN:
      write_u32(t, a->open.size);
      write_bytes(t, t->buf, a->open.size);
      break;
    case A_GPIC:
      write_f32(t, a->gpic.bounds[0]);
      write_f32(t, a->gpic.bounds[1]);
      write_f32(t, a->gpic.bounds[2]);
      write_f32(t, a->gpic.bounds[3]);
      break;
    default:
      mabort();
  }
}

void channel_flush(channel_t *t)
{
  cflush(t);
}

void *channel_write_buffer(channel_t *t, size_t n)
{
  while (n > t->buf_size)
    resize_buf(t);
  return t->buf;
}
