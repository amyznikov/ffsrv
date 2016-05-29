/*
 * http-request.h
 *
 *  Created on: Mar 19, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __http_request_h__
#define __http_request_h__

#include "csmap.h"

#ifdef __cplusplus
extern "C" {
#endif

struct http_request;

typedef
struct http_request_callback {
  int (*on_headers_complete)(void * cookie);
  int (*on_body)(void * cookie, const char *at, size_t length);
  int (*on_message_complete)(void * cookie);
} http_request_cb;



typedef
struct http_request {
  char * method;
  char * url;
  char * proto;
  csmap parms;
  struct {
    struct http_parser * p;
    const struct http_request_callback * cb;
    void * cookie;
    char * cf, * cv;
  } private;
} http_request;


void http_request_init(struct http_request * q, const struct http_request_callback * cb, void * cookie);
void http_request_cleanup(struct http_request * q);
bool http_request_parse(struct http_request * q, const void * data, size_t size);



#ifdef __cplusplus
}
#endif

#endif /* __http_request_h__ */
