/*
 * http-client-context.h
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */


#ifndef __ffsrv_http_client_context_h__
#define __ffsrv_http_client_context_h__

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include "http-request.h"
#include "co-ssl.h"

#ifdef __cplusplus
extern "C" {
#endif


////////////////////////////////////////////////////////////////////////////////////////////

typedef
struct http_request_handler_iface {
  bool (*onbody)(void * qh, const uint8_t at[], size_t length);
  bool (*onbodycomplete)(void * qh);
  void (*run)(void * qh);
  void (*ondestroy)(void * qh);
} http_request_handler_iface;

struct http_request_handler {
  const struct http_request_handler_iface * iface;
};


void * http_request_handler_alloc(size_t size, const http_request_handler_iface * iface);
void http_request_handler_destroy(struct http_request_handler ** qh);



////////////////////////////////////////////////////////////////////////////////////////////



struct http_client_ctx {
  struct http_request_handler * qh;
  struct cosocket * cosock;
  SSL * ssl;
  http_request req;
  int status;
  int so;
  int txbufsize;
};


struct http_client_ctx * create_http_client_context(int so, SSL_CTX * ssl_ctx);
void destroy_http_client_context(struct http_client_ctx * client_ctx);


ssize_t http_read(struct http_client_ctx * client_ctx);
ssize_t http_write(struct http_client_ctx * client_ctx, const void * buf, size_t size);
bool http_getoutspc(struct http_client_ctx * client_ctx, int * outspc, int * maxspc);

bool http_vssend(struct http_client_ctx * client_ctx, const char * format, va_list arglist);
bool http_ssend(struct http_client_ctx * client_ctx, const char * format, ...)
  __attribute__ ((__format__ (__printf__, 2, 3)));




#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_http_client_context_h__ */
