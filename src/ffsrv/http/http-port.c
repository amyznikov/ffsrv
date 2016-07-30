/*
 * http-port.c
 *
 *  Created on: Apr 2, 2016
 *      Author: amyznikov
 */
#define _GNU_SOURCE

#include <inttypes.h>
#include <unistd.h>

#include "ffsrv.h"
#include "ffcfg.h"
#include "http-port.h"
#include "http-client-context.h"
#include "sockopt.h"
#include "ipaddrs.h"
#include "debug.h"


#define UNUSED(x)  ((void)(x))


#define HTTP_SERVER_STACK_SIZE  (128 * 1024)


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct http_server_ctx {
  SSL_CTX * ssl_ctx;
  int so;
};


static struct http_server_ctx * create_http_server_context(const struct sockaddr_in * addrs, SSL_CTX * ssl_ctx);
static void destroy_http_server_context(struct http_server_ctx * server_ctx);
static int  http_server_io_callback(void * cookie, uint32_t epoll_events);



/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



static struct http_server_ctx * create_http_server_context(const struct sockaddr_in * addrs, SSL_CTX * ssl_ctx)
{
  struct http_server_ctx * server_ctx = NULL;
  int so = -1;

  bool fok = false;

  if ( (so = so_tcp_listen_addrs((struct sockaddr *) addrs, sizeof(*addrs))) == -1 ) {
    PDBG("so_tcp_listen_addrs() fails: %s", strerror(errno));
    goto end;
  }

  so_set_noblock(so, true);

  if ( !(server_ctx = calloc(1, sizeof(*server_ctx))) ) {
    PDBG("calloc(server_ctx) fails: %s", strerror(errno));
    goto end;
  }

  server_ctx->so = so;
  server_ctx->ssl_ctx = ssl_ctx;

  if ( !(fok = co_schedule_io(so, EPOLLIN, http_server_io_callback, server_ctx, HTTP_SERVER_STACK_SIZE)) ) {
    PDBG("co_schedule_io(http_server_io_callback) fails: %s", strerror(errno));
    goto end;
  }

end: ;

  if ( !fok ) {

    if ( so != -1 ) {
      close(so);
    }

    if ( server_ctx ) {
      free(server_ctx);
      server_ctx = NULL;
    }
  }

  return server_ctx;
}


static void destroy_http_server_context(struct http_server_ctx * server_ctx)
{
  if ( server_ctx ) {
    int so = server_ctx->so;
    if ( so != -1 ) {
      close(so);
    }
    free(server_ctx);
    PDBG("[so=%d] DESTROYED", so);
  }
}


static int http_server_io_callback(void * cookie, uint32_t epoll_events)
{
  struct http_server_ctx * server_ctx = cookie;
  int so;

  int status = 0;

  if ( epoll_events & EPOLLERR ) {
    status = -1;
  }
  else if ( epoll_events & EPOLLIN ) {

    struct sockaddr_in addrs;
    socklen_t addrslen = sizeof(addrs);

    while ( (so = accept(server_ctx->so, (struct sockaddr*) &addrs, &addrslen)) != -1 ) {

      PDBG("ACCEPTED SO=%d from %s. SSL_CTX = %p", so, saddr2str(&addrs, NULL), server_ctx->ssl_ctx);

      if ( !create_http_client_context(so, server_ctx->ssl_ctx) ) {
        so_close_connection(so, 1);
      }

      co_yield();
    }

    if ( errno != EAGAIN ) {
      PDBG("ACCEPT(so=%d) FAILS: %s", server_ctx->so, strerror(errno));
      status = -1;
    }
  }
  else {
    status = -1;
  }

  if ( status ) {
    PDBG("[so=%d] SERVER ERROR", server_ctx->so);
    destroy_http_server_context(server_ctx);
  }

  return status;
}



/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




bool ffsrv_add_http_port(const struct sockaddr_in * addrs)
{
  return create_http_server_context(addrs, NULL) != NULL;
}

bool ffsrv_add_https_port(const struct sockaddr_in * addrs)
{
  static SSL_CTX * g_ssl_ctx = NULL;

  bool fok = false;

  if ( !g_ssl_ctx ) {

    if ( !ffsrv.https.cert ) {
      PDBG("https.cert not specified");
      goto end;
    }

    if ( !ffsrv.https.key ) {
      PDBG("https.key not specified");
      goto end;
    }

    PDBG("Using cert=%s key=%s", ffsrv.https.cert, ffsrv.https.key);

    if ( !(g_ssl_ctx = co_ssl_create_context(ffsrv.https.cert, ffsrv.https.key, ffsrv.https.ciphers)) ) {
      PDBG("co_ssl_create_context() fails");
      PSSL();
      goto end;
    }
  }

  if ( !create_http_server_context(addrs, g_ssl_ctx) ) {
    PDBG("create_http_server_ctx() fails");
    goto end;
  }

  fok = true;

end : ;

  return fok;
}
