/*
 * http-context.c
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <malloc.h>
#include <string.h>

#include "ffcfg.h"
#include "http-client-context.h"
#include "http-get-online-stream.h"
#include "http-post-online-stream.h"
#include "http-get-saved-stream.h"
#include "http-get-saved-streams.h"
#include "url-parser.h"
#include "sockopt.h"
#include "debug.h"


#define HTTP_RXBUF_SIZE (4*1024)
#define HTTP_CLIENT_STACK_SIZE (HTTP_RXBUF_SIZE + 1024*1024)


void * http_request_handler_alloc(size_t size, const http_request_handler_iface * iface)
{
  struct http_request_handler * qh = calloc(1, size);
  if ( qh ) {
    qh->iface = iface;
  }
  return qh;
}

void http_request_handler_destroy(struct http_request_handler ** qh)
{
  if ( qh && *qh ) {
    if ( (*qh)->iface && (*qh)->iface->ondestroy ) {
      (*qh)->iface->ondestroy(*qh);
    }
    free(*qh);
    *qh = NULL;
  }
}


////////////////////////////////////////////////////////////////////////////////////////////


static void http_client_thread(void * arg);
static bool on_http_headers_complete(void * cookie);
static bool on_http_body(void * cookie, const char *at, size_t length);
static bool on_http_message_complete(void * cookie);

struct http_client_ctx * create_http_client_context(int so, SSL_CTX * ssl_ctx)
{
  struct http_client_ctx * client_ctx = NULL;

  bool fok = false;

  static const struct http_request_callback http_request_cb = {
      .on_headers_complete = on_http_headers_complete,
      .on_body = on_http_body,
      .on_message_complete = on_http_message_complete,
  };

  if ( !(client_ctx = calloc(1, sizeof(*client_ctx))) ) {
    PDBG("calloc(client_ctx) fails: %s", strerror(errno));
    goto end;
  }

  http_request_init(&client_ctx->req, &http_request_cb, client_ctx);

  so_set_noblock(client_ctx->so = so, true);

  if ( so_set_recvbuf(so, ffsrv.http.rxbuf) != 0 ) {
    PDBG("[so=%d] so_set_recvbuf() fails: %s", so, strerror(errno));
    goto end;
  }

  if ( so_set_sendbuf(so, ffsrv.http.txbuf) != 0 ) {
    PDBG("[so=%d] so_set_sendbuf() fails: %s", so, strerror(errno));
    goto end;
  }

  if ( so_get_sendbuf(so, &client_ctx->txbufsize) != 0 ) {
    PDBG("[so=%d]: so_get_sendbuf() fails: %s", so, strerror(errno));
    goto end;
  }

  so_set_keepalive(so, ffsrv.keepalive.enable,
      ffsrv.keepalive.idle,
      ffsrv.keepalive.intvl,
      ffsrv.keepalive.probes);

  if ( !(client_ctx->cosock = cosocket_create(so)) ) {
    PDBG("[so=%d] create_cosock() fails: %s", so, strerror(errno));
    goto end;
  }

  if ( !ssl_ctx ) {
    cosocket_set_rcvtmout(client_ctx->cosock, ffsrv.http.rcvtmo);
    cosocket_set_sndtmout(client_ctx->cosock, ffsrv.http.sndtmo);
  }
  else if ( !(client_ctx->ssl = co_ssl_new(ssl_ctx, client_ctx->cosock)) ) {
    PDBG("[so=%d] co_ssl_new() fails", so);
    goto end;
  }
  else {
    cosocket_set_rcvtmout(client_ctx->cosock, ffsrv.https.rcvtmo);
    cosocket_set_sndtmout(client_ctx->cosock, ffsrv.https.sndtmo);
  }


  if ( !(fok = co_schedule(http_client_thread, client_ctx, HTTP_CLIENT_STACK_SIZE)) ) {
    PDBG("co_schedule(http_client_io_callback) fails: %s", strerror(errno));
    goto end;
  }

end: ;

  if ( !fok && client_ctx ) {
    cosocket_delete(&client_ctx->cosock);
    free(client_ctx);
    client_ctx = NULL;
  }

  return client_ctx;
}

void destroy_http_client_context(struct http_client_ctx * client_ctx)
{
  if ( client_ctx ) {

    int so = client_ctx->so;

    if ( so != -1 ) {
      so_close_connection(so, 0);
    }

    http_request_handler_destroy(&client_ctx->qh);
    co_ssl_free(&client_ctx->ssl);
    cosocket_delete(&client_ctx->cosock);
    http_request_cleanup(&client_ctx->req);
//    free(client_ctx->body);
    free(client_ctx);

    PDBG("[so=%d] DESTROYED", so);
  }
}



ssize_t http_read(struct http_client_ctx * client_ctx)
{
  ssize_t size;
  uint8_t rx[HTTP_RXBUF_SIZE];

  if ( client_ctx->ssl ) {
    size = SSL_read(client_ctx->ssl,rx, sizeof(rx));
  }
  else {
    size = cosocket_recv(client_ctx->cosock, rx, sizeof(rx), 0);
  }

  if ( size >= 0 && !http_request_parse(&client_ctx->req, rx, size) ) {
    PDBG("[so=%d] http_request_parse() fails", client_ctx->so);
    errno = EPROTO, size = -1;
  }

  return size;
}

ssize_t http_write(struct http_client_ctx * client_ctx, const void * buf, size_t size)
{
  ssize_t sent;

  if ( client_ctx->ssl ) {
    sent = SSL_write(client_ctx->ssl, buf, size);
  }
  else {
    sent = cosocket_send(client_ctx->cosock, buf, size, 0);
  }

  return sent;
}


bool http_vssend(struct http_client_ctx * client_ctx, const char * format, va_list arglist)
{
  char * msg = NULL;
  int n;

  if ( (n = vasprintf(&msg, format, arglist)) > 0 ) {
    n = http_write(client_ctx, msg, n);
  }

  free(msg);
  return n > 0;
}

bool http_ssend(struct http_client_ctx * client_ctx, const char * format, ...)
{
  va_list arglist;
  bool fok;

  va_start(arglist, format);
  fok = http_vssend(client_ctx, format, arglist);
  va_end(arglist);

  return fok;
}


bool http_getoutspc(struct http_client_ctx * client_ctx, int * outspc, int * maxspc)
{
  int qsz = 0;
  bool fok = false;

  if ( (qsz = so_get_outq_size(client_ctx->so)) < 0 ) {
    PDBG("so_get_outq_size() fails: %s", strerror(errno));
  }
  else {
    *outspc = client_ctx->txbufsize - qsz;
    *maxspc = client_ctx->txbufsize;
    fok = true;
  }

  return fok;
}




static void http_client_thread(void * arg)
{
  struct http_client_ctx * client_ctx = arg;
  ssize_t size;
  int status;
  int so = client_ctx->so;

  PDBG("[so=%d] STARTED", so);

  if ( client_ctx->ssl && (status = SSL_accept(client_ctx->ssl)) <= 0 ) {
    PDBG("SSL_accept() fails: status=%d", status);
    PSSL();
    goto end;
  }

  while ( (size = http_read(client_ctx)) > 0 ) {
    if ( client_ctx->qh ) {
      break;
    }
  }

  if ( size < 0 ) {
    goto end;
  }

  if ( client_ctx->qh && client_ctx->qh->iface->run ) {
    client_ctx->qh->iface->run(client_ctx->qh);
  }

end:

  destroy_http_client_context(client_ctx);

  PDBG("[so=%d] FINISHED", so);
}





static bool on_http_headers_complete(void * cookie)
{
  struct http_client_ctx * client_ctx = cookie;
  const struct http_request * q = &client_ctx->req;
  const char * url = q->url;

  bool fok = false;

  // dump headers to debug log
  PDBG("[so=%d] '%s %s %s'", client_ctx->so, q->method, q->url, q->proto);
  for ( size_t i = 0, n = csmap_size(&q->parms); i < n; ++i ) {
    const csmap_entry * e = csmap_item(&q->parms, i);
    PDBG("[so=%d] %s : %s", client_ctx->so, e->key, e->value);
  }
  PDBG("[so=%d]", client_ctx->so);


  while ( *url == '/' ) {
    ++url;
  }

  if ( strcmp(q->method, "POST") == 0 ) {
    // POST input[?opts]
    fok = create_http_post_online_stream_context(&client_ctx->qh, client_ctx);
  }
  else if ( strcmp(q->method, "GET") == 0 ) {

    // GET input[?opts]
    // GET encoder[?opts]
    // GET mix[?opts]
    // GET store/input[/file][?opts]

    if ( strncmp(url, "store/", 6) != 0 ) {
      fok = create_http_get_online_stream_context(&client_ctx->qh, client_ctx);
    }
    else if ( strchr(url + 6, '/') ) {
      fok = create_http_get_saved_stream_context(&client_ctx->qh, client_ctx);
    }
    else {
      fok = create_http_get_saved_streams_context(&client_ctx->qh, client_ctx);
    }
  }

  else {

    http_ssend(client_ctx,
        "%s 501 Not Implemented\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>\r\n"
        "<body>\r\n"
        "<h1>This function is not supported</h1>\r\n"
        "</body>\r\n"
        "</html>\r\n",
        q->proto);
  }

  return fok;
}

static bool on_http_message_complete(void * cookie)
{
  struct http_client_ctx * client_ctx = cookie;
  if ( client_ctx->qh && client_ctx->qh->iface->onbodycomplete ) {
    return client_ctx->qh->iface->onbodycomplete(client_ctx->qh);
  }
  return false;
}


static bool on_http_body(void * cookie, const char *at, size_t length)
{
  struct http_client_ctx * client_ctx = cookie;

  if ( !client_ctx->qh || !client_ctx->qh->iface->onbody ) {
    PDBG("[so=%d] CLIENT ERROR: UNEXPECTED BODY ON NULL QH", client_ctx->so);
    return false;
  }

  return client_ctx->qh->iface->onbody(client_ctx->qh, (uint8_t*)at, length);
}


