/*
 * http-request.c
 *
 *  Created on: Mar 19, 2016
 *      Author: amyznikov
 */

#define _GNU_SOURCE

#include "http-request.h"
#include "http_parser.h"
#include "debug.h"
#include <stdio.h>


static inline char * strndcat(char * s1, const char * s2, size_t length)
{
  char * s;
  if ( !s1 ) {
    s = strndup(s2, length);
  }
  else {
    size_t l1 = strlen(s1);
    strncpy((s = realloc(s1, l1 + length + 1)) + l1, s2, length);
  }
  return s;
}


static int on_message_begin(http_parser * p)
{
  (void)(p);
  return 0;
}

static int on_url(http_parser * p, const char * at, size_t length)
{
  struct http_request * q = p->data;
  q->method = strdup(http_method_str(p->method));
  q->url = strndup(at, length);
  return 0;
}

static int on_header_field(http_parser * p, const char *at, size_t length)
{
  struct http_request * q = p->data;
  if ( q->private.cf && q->private.cv ) {
    csmap_push(&q->parms, q->private.cf, q->private.cv);
    q->private.cf = q->private.cv = NULL;
  }
  q->private.cf = strndcat(q->private.cf, at, length);
  return 0;
}

static int on_header_value(http_parser * p, const char *at, size_t length)
{
  struct http_request * q = p->data;
  q->private.cv = strndcat(q->private.cv, at, length);
  return 0;
}

static int on_headers_complete(http_parser * p)
{
  struct http_request * q = p->data;

  if ( q->private.cf && q->private.cv ) {
    csmap_push(&q->parms, q->private.cf, q->private.cv);
    q->private.cf = q->private.cv = NULL;
  }

  asprintf(&q->proto, "HTTP/%d.%d", p->http_major, p->http_minor);

  if ( q->private.cb->on_headers_complete ) {
    return q->private.cb->on_headers_complete(q->private.cookie) ? 0 : -1;
  }

  return 0;
}

static int on_body(http_parser * p, const char *at, size_t length)
{
  struct http_request * q = p->data;
  if ( q->private.cb->on_body ) {
    return q->private.cb->on_body(q->private.cookie, at, length) ? 0 : -1;
  }
  return 0;
}

static int on_message_complete(http_parser * p)
{
  struct http_request * q = p->data;
  q->msgcomplete = true;
  if ( q->private.cb->on_message_complete ) {
    return q->private.cb->on_message_complete(q->private.cookie) ? 0 : -1;
  }
  return 0;
}

void http_request_init(struct http_request * q, const struct http_request_callback * cb, void * cookie)
{
  memset(q, 0, sizeof(*q));
  q->private.cb = cb;
  q->private.cookie = cookie;
}

void http_request_cleanup(struct http_request * q)
{
  free(q->method);
  free(q->url);
  free(q->proto);
  free(q->private.cf);
  free(q->private.cv);
  free(q->private.p);
  csmap_cleanup(&q->parms);
  memset(q, 0, sizeof(*q));
}


bool http_request_parse(struct http_request * q, const void * data, size_t size)
{
  size_t parsed;

  static const struct http_parser_settings settings = {
      .on_message_begin = on_message_begin,
      .on_url = on_url,
      .on_status = NULL,
      .on_header_field = on_header_field,
      .on_header_value = on_header_value,
      .on_headers_complete = on_headers_complete,
      .on_body = on_body,
      .on_message_complete = on_message_complete,
      .on_chunk_header = NULL,
      .on_chunk_complete= NULL
  };

  if ( !q->private.p ) {
    if ( !(q->private.p = calloc(1, sizeof(struct http_parser))) ) {
      return false;
    }
    http_parser_init(q->private.p, HTTP_REQUEST);
    q->private.p->data = q;
  }

  if ( (parsed = http_parser_execute(q->private.p, &settings, data, size)) != size ) {
      PDBG("http_parser_execute() fails: http_errno=%d (%s) size=%zu parsed=%zu",
          q->private.p->http_errno, http_errno_name(q->private.p->http_errno),
          size, parsed);
    return false;
  }

  return true;
}
