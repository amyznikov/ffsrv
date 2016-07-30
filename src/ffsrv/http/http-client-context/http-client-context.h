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
#include "http-status-codes.h"
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


bool http_send_file(struct http_client_ctx * client_ctx, const char * fname, const char * mime_type, const char * range);

bool http_send_error(struct http_client_ctx * client_ctx, int status_code, const char * format, ...);
bool http_send_error_v(struct http_client_ctx * client_ctx, int status_code, const char * format, va_list arglist);

bool http_send_200_OK(struct http_client_ctx * client_ctx, const char * content_type, size_t content_length, const char * hdrs);
bool http_send_200_OK_ncl(struct http_client_ctx * client_ctx, const char * content_type, const char * hdrs);
bool http_send_403_forbidden(struct http_client_ctx * client_ctx);
bool http_send_404_not_found(struct http_client_ctx * client_ctx);
bool http_send_405_not_allowed(struct http_client_ctx * client_ctx);
bool http_send_500_internal_server_error(struct http_client_ctx * client_ctx, const char * msg, ...);
bool http_send_501_not_implemented(struct http_client_ctx * client_ctx);

#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_http_client_context_h__ */
