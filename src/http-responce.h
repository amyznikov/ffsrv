/*
 * http-responce.h
 *
 *  Created on: Apr 1, 2016
 *      Author: amyznikov
 */


// #pragma once

#ifndef SRC_HTTP_RESPONCE_H_
#define SRC_HTTP_RESPONCE_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/types.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef
struct http_responce_callback {
  void (*send_complete)(void * cookie);
} http_responce_cb;


typedef
struct http_responce {
  const http_responce_cb * cb;
  void * cookie;
  char * buf;
  size_t size;
  size_t cpos;
} http_responce;


bool http_responce_init(struct http_responce* resp, const http_responce_cb * cb, void * cookie);
void http_responce_cleanup(struct http_responce* resp);

bool http_responce_set(struct http_responce* resp, const char * format, ...);
bool http_responce_setv(struct http_responce* resp, const char * format, va_list arglist);

ssize_t http_responce_write(struct http_responce* resp, int so);

#ifdef __cplusplus
}
#endif

#endif /* SRC_HTTP_RESPONCE_H_ */
