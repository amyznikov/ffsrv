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
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ffcfg.h"
#include "ffobject.h"
#include "http-client-context.h"
#include "http-get-online-stream.h"
#include "http-get-segments.h"
#include "http-post-online-stream.h"
#include "http-get-file.h"
#include "http-get-directory.h"
#include "http-byte-range.h"
#include "sockopt.h"
#include "strfuncs.h"
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
static bool create_http_request_handler(struct http_client_ctx * client_ctx);


struct http_client_ctx * create_http_client_context(int so, SSL_CTX * ssl_ctx)
{
  struct http_client_ctx * client_ctx = NULL;

  bool fok = false;

  if ( !(client_ctx = calloc(1, sizeof(*client_ctx))) ) {
    PDBG("calloc(client_ctx) fails: %s", strerror(errno));
    goto end;
  }

  http_request_init(&client_ctx->req, NULL, NULL);

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


  static const struct http_request_callback http_request_cb = {
      .on_headers_complete = on_http_headers_complete,
      .on_body = on_http_body,
      .on_message_complete = on_http_message_complete,
  };

  PDBG("[so=%d] STARTED", so);

  if ( client_ctx->ssl && (status = SSL_accept(client_ctx->ssl)) <= 0 ) {
    PDBG("SSL_accept() fails: status=%d", status);
    PSSL();
    goto end;
  }

  while ( 42 ) {

    http_request_init(&client_ctx->req, &http_request_cb, client_ctx);

    while ( (size = http_read(client_ctx)) > 0 ) {
      if ( client_ctx->qh || client_ctx->req.msgcomplete ) {
        break;
      }
    }

    if ( size > 0 && client_ctx->qh && client_ctx->qh->iface->run ) {
      client_ctx->qh->iface->run(client_ctx->qh);
    }

    if ( size <= 0 ) {
      break;
    }

    http_request_cleanup(&client_ctx->req);
    http_request_handler_destroy(&client_ctx->qh);
  }


end:

  destroy_http_client_context(client_ctx);

  PDBG("[so=%d] FINISHED", so);
}





static bool on_http_headers_complete(void * cookie)
{
  struct http_client_ctx * client_ctx = cookie;
  const struct http_request * q = &client_ctx->req;

  // dump headers to debug log
  PDBG("[so=%d] '%s %s %s'", client_ctx->so, q->method, q->url, q->proto);
  for ( size_t i = 0, n = csmap_size(&q->parms); i < n; ++i ) {
    const csmap_entry * e = csmap_item(&q->parms, i);
    PDBG("[so=%d] %s : %s", client_ctx->so, e->key, e->value);
  }
  PDBG("[so=%d]", client_ctx->so);

  return create_http_request_handler(client_ctx);
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


////////////////////////////////////////////////////////////////

bool http_send_error_v(struct http_client_ctx * client_ctx, int status_code, const char * format, va_list arglist)
{
  char * body = NULL;
  bool fok;


  if ( format ) {
    vasprintf(&body, format, arglist);
  }

  fok = http_ssend(client_ctx,
      "%s %d %s\r\n"
          "Content-Type: text/html; charset=utf-8\r\n"
          "Accept-Ranges: bytes\r\n"
          "Connection: keep-alive\r\n"
          "Server: ffsrv\r\n"
          "\r\n"
          "<html><body>\n%s\n</body></html>",
      client_ctx->req.proto,
      status_code,
      http_status_message(status_code),
      body ? body : "");

  if ( body ) {
    free(body);
  }

  return fok;
}

bool http_send_error(struct http_client_ctx * client_ctx, int status_code, const char * format, ...)
{
  va_list arglist;
  bool fok;

  va_start(arglist, format);
  fok = http_send_error_v(client_ctx, status_code, format, arglist);
  va_end(arglist);

  return fok;
}



bool http_send_200_OK(struct http_client_ctx * client_ctx, const char * content_type, size_t content_length, const char * hdrs)
{
  return http_ssend(client_ctx,
      "%s 200 OK\r\n"
          "Content-Type: %s\r\n"
          "Content-Length: %zu\r\n"
          "Accept-Ranges: bytes\r\n"
          "Connection : keep-alive\r\n"
          "Server: ffsrv\r\n"
          "%s"
          "\r\n",
      client_ctx->req.proto,
      content_type,
      content_length,
      hdrs ? hdrs : "");
}

bool http_send_200_OK_ncl(struct http_client_ctx * client_ctx, const char * content_type, const char * hdrs)
{
  return http_ssend(client_ctx,
      "%s 200 OK\r\n"
          "Content-Type: %s\r\n"
          "Accept-Ranges: bytes\r\n"
          "Connection : keep-alive\r\n"
          "Server: ffsrv\r\n"
          "%s"
          "\r\n",
      client_ctx->req.proto,
      content_type,
      hdrs ? hdrs : "");
}


bool http_send_206_partial_content(struct http_client_ctx * client_ctx, const char * content_type,
    size_t content_range_start, size_t content_range_end, size_t full_content_size)
{
  const size_t content_length = content_range_end - content_range_start + 1;

  return http_ssend(client_ctx,
      "%s 206 Partial Content\r\n"
          "Content-Type: %s\r\n"
          "Content-Range: bytes %zu-%zu/%zu\r\n"
          "Content-Length: %zu\r\n"
          "Accept-Ranges: bytes\r\n"
          "Connection : keep-alive\r\n"
          "Server: ffsrv\r\n"
          "\r\n",
      client_ctx->req.proto,
      content_type,
      content_range_start, content_range_end, full_content_size,
      content_length);
}

bool http_send_206_multipart_byteranges(struct http_client_ctx * client_ctx, const char * boundary)
{
  return http_ssend(client_ctx,
      "%s 206 Partial Content\r\n"
          "Content-Type: multipart/byteranges; boundary=%s\r\n"
          "Accept-Ranges: bytes\r\n"
          "Connection : keep-alive\r\n"
          "Server: ffsrv\r\n"
          "\r\n",
      client_ctx->req.proto,
      boundary);
}


bool http_send_403_forbidden(struct http_client_ctx * client_ctx)
{
  return http_send_error(client_ctx, HTTP_403_Forbidden,
      "<h1>Permission denied</h1>");
}

bool http_send_404_not_found(struct http_client_ctx * client_ctx)
{
  return http_send_error(client_ctx, HTTP_404_NotFound,
      "<h1>Requested object not found</h1>\r\n");
}

bool http_send_405_not_allowed(struct http_client_ctx * client_ctx)
{
  return http_send_error(client_ctx, HTTP_405_MethodNotAllowed,
      "<h1>Request method is not supported for the requested resource</h1>\r\n");
}

bool http_send_500_internal_server_error(struct http_client_ctx * client_ctx, const char * format, ...)
{
  va_list arglist;
  bool fok;

  va_start(arglist, format);
  fok = http_send_error_v(client_ctx, HTTP_500_InternalServerError, format, arglist);
  va_end(arglist);

  return fok;
}


bool http_send_501_not_implemented(struct http_client_ctx * client_ctx)
{
  return http_send_error(client_ctx, HTTP_501_NotImplemented,
      "<h1>This function is not supported</h1>\r\n");
}






static char * getuname(const char * url)
{
  char * p, * s;
  size_t len;

  if ( !(p = strpbrk(url, "/?")) ) {
    s = strdup(url);
  }
  else if ( (s = malloc((len = (p - url)) + 1)) ) {
    strncpy(s, url, len)[len] = 0;
  }

  return s;
}

static bool create_http_request_handler(struct http_client_ctx * client_ctx)
{
  const struct http_request * q = &client_ctx->req;
  const char * url = q->url;

  char urlpath[PATH_MAX] = "";
  char urlargs[1024] = "";

  char * uname = NULL;

  char * abspath = NULL;
  char * mime = NULL;

  enum ffmagic objtype = ffmagic_unknown;

  bool fok = true;



  if ( strcmp(q->method, "POST") != 0 && strcmp(q->method, "GET") != 0 ) {
    http_send_501_not_implemented(client_ctx);
    goto end;
  }


  /* Parse requested URL */

  while ( *url == '/' ) {
    ++url;
  }

  split_url(url, urlpath, sizeof(urlpath),
      urlargs, sizeof(urlargs));

  PDBG("uname=%s urlpath=%s urlargs=%s", uname, urlpath, urlargs);

  if ( !(uname = getuname(urlpath)) || !*uname ) {
    http_send_404_not_found(client_ctx);
    goto end;
  }

//  PDBG("uname=%s urlpath=%s", uname, urlpath);

  if ( !ffurl_magic(urlpath, &abspath, &objtype, &mime) ) {
    http_send_404_not_found(client_ctx);
    goto end;
  }

  // PDBG("abspath=%s mime=%s", abspath, mime);

  switch ( objtype ) {

    case ffmagic_input :
      if ( strcmp(q->method, "POST") == 0 ) {
        fok = http_post_online_stream(&client_ctx->qh, client_ctx, urlpath, urlargs);
      }
      else if ( strcmp(q->method, "GET") == 0 ) {
        fok = http_get_online_stream(&client_ctx->qh, client_ctx, urlpath, urlargs);
      }
      else {
        http_send_405_not_allowed(client_ctx);
      }
    break;


    case ffmagic_enc:
      if ( strcmp(q->method, "GET") == 0 ) {
        fok = http_get_online_stream(&client_ctx->qh, client_ctx, urlpath, urlargs);
      }
      else {
        http_send_405_not_allowed(client_ctx);
      }
      break;

    case ffmagic_segments:
      if ( strcmp(q->method, "GET") == 0 ) {
        fok = http_get_segments_stream(&client_ctx->qh, client_ctx, urlpath, urlargs);
      }
      else {
        http_send_405_not_allowed(client_ctx);
      }
      break;

    case ffmagic_directory : {
      if ( strcmp(q->method, "GET") == 0 ) {
        fok = http_get_directory_listing(&client_ctx->qh, client_ctx, ffsrv.db.root, urlpath);
      }
      else {
        http_send_405_not_allowed(client_ctx);
      }
    }
    break;

    case ffmagic_file:
      if ( strcmp(q->method, "GET") == 0 ) {
        fok = http_get_file(&client_ctx->qh, client_ctx, abspath, mime);
      }
      else {
        http_send_405_not_allowed(client_ctx);
      }
      break;

    default :
      http_send_404_not_found(client_ctx);
      break;
  }

end:

  free(abspath);
  free(mime);
  free(uname);

  return fok;
}




static bool http_send_file_bytes(struct http_client_ctx * cctx, int fd, ssize_t send_size)
{
  const size_t BUFF_SIZE = 4 * 1024;
  uint8_t buf[BUFF_SIZE];
  ssize_t size, sent;

  for ( sent = 0; sent < send_size && (size = read(fd, buf, BUFF_SIZE)) > 0; sent += size ) {
    if ( http_write(cctx, buf, size) != size ) {
      break;
    }
  }

  return sent == send_size;
}

bool http_send_file(struct http_client_ctx * cctx, const char * fname, const char * mime_type, const char * range)
{
  const size_t maxranges = 256;
  struct http_byte_range ranges[maxranges];
  int numranges = 0;
  static const char boundary[] = "--THIS_STRING_SEPARATES";


  struct stat stat;
  int fd = -1;

  bool fok = false;

  processaccesshooks(fname);

  if ( (fd = open(fname, O_RDONLY)) == -1 ) {
    PDBG("open(%s) fails: %s", fname, strerror(errno));
    switch ( errno ) {
      case ENOENT : fok = http_send_404_not_found(cctx); break;
      case EPERM : fok = http_send_403_forbidden(cctx); break;
      default : fok = http_send_500_internal_server_error(cctx, "<html><body>open(%s) fails: %s</html></body>", fname, strerror(errno)); break;
    }
    goto end;
  }

  if ( fstat(fd, &stat) == -1 ) {
    PDBG("fstat(%s) fails: %s", fname, strerror(errno));
    fok = http_send_500_internal_server_error(cctx, "<html><body>fstat(%s) fails: %s</html></body>", fname, strerror(errno));
    goto end;
  }

  if ( range && *range && (numranges = http_parse_byte_range(range, stat.st_size, ranges, maxranges)) < 0 ) {
    fok = http_send_error(cctx, HTTP_416_RangeNotSatisfiable, NULL);
    goto end;
  }


  if ( numranges == 0 ) {
    if ( (fok = http_send_200_OK(cctx, mime_type, stat.st_size, NULL)) ) {
      fok = http_send_file_bytes(cctx, fd, stat.st_size);
    }
  }
  else if ( numranges == 1 ) {
    if ( lseek64(fd, ranges[0].firstpos, SEEK_SET) == -1 ) {
      fok = http_send_error(cctx, HTTP_416_RangeNotSatisfiable, NULL);
    }
    else if ( (fok = http_send_206_partial_content(cctx, mime_type, ranges[0].firstpos, ranges[0].lastpos, stat.st_size)) ) {
      fok = http_send_file_bytes(cctx, fd, ranges[0].lastpos - ranges[0].firstpos + 1);
    }
  }
  else if ( (fok = http_send_206_multipart_byteranges(cctx, boundary)) ) {

    for ( int i = 0; i < numranges; ++i ) {

      if ( lseek64(fd, ranges[i].firstpos, SEEK_SET) == -1 ) {
        fok = http_send_error(cctx, HTTP_416_RangeNotSatisfiable, NULL);
      }
      else {

        fok = http_ssend(cctx, "%s\r\n"
            "Content-Type: %s\r\n"
            "Content-Range: bytes %zd-%zd/%zd\r\n\r\n",
            boundary,
            mime_type,
            ranges[i].firstpos,
            ranges[i].lastpos,
            stat.st_size);

        if ( fok ) {
          fok = http_send_file_bytes(cctx, fd, ranges[0].lastpos - ranges[0].firstpos + 1);
        }
      }
    }
  }

end:

  if ( fd != -1 ) {
    close(fd);
  }

  return fok;
}
