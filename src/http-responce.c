/*
 * http-responce.c
 *
 *  Created on: Apr 1, 2016
 *      Author: amyznikov
 */

#define _GNU_SOURCE

#include "http-responce.h"
#include <malloc.h>
#include <stdio.h>
#include <sys/socket.h>


bool http_responce_init(struct http_responce* resp, const http_responce_cb * cb, void * cookie)
{
  *resp = (struct http_responce) {
    .cb = cb,
    .cookie = cookie,
    .buf = NULL,
    .size = 0,
    .cpos = 0,
  };
  return true;
}

void http_responce_cleanup(struct http_responce* resp)
{
  if ( resp ) {
    free(resp->buf);
    *resp = (struct http_responce) {
      .cb = NULL,
      .cookie = NULL,
      .buf = NULL,
      .size = 0,
      .cpos = 0,
    };
  }
}

bool http_responce_set(struct http_responce* resp, const char * format, ...)
{
  bool fok = false;
  if ( !resp->buf ) {
    va_list arglist;
    va_start(arglist, format);
    fok = http_responce_setv(resp, format, arglist);
    va_end(arglist);
  }
  return fok;
}

bool http_responce_setv(struct http_responce* resp, const char * format, va_list arglist)
{
  bool fok = false;
  if ( !resp->buf ) {
    int n = vasprintf(&resp->buf, format, arglist);
    if ( n > 0 ) {
      resp->size = n;
      resp->cpos = 0;
      fok = true;
    }
  }
  return fok;
}

ssize_t http_responce_write(struct http_responce* resp, int so)
{
  const int flags = MSG_NOSIGNAL | MSG_DONTWAIT;
  ssize_t sent = send(so, resp->buf + resp->cpos, resp->size - resp->cpos, flags);
  if ( sent > 0 && (resp->cpos += sent) >= resp->size ) {
    free(resp->buf), resp->buf = NULL, resp->size = 0, resp->cpos = 0;
    resp->cb->send_complete(resp->cookie);
  }
  return sent;
}
