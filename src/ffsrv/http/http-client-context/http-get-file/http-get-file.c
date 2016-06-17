/*
 * http-get-file.c
 *
 *  Created on: Jun 7, 2016
 *      Author: amyznikov
 */

#include "ffcfg.h"
#include "http-get-file.h"
#include "strfuncs.h"
#include "ffmpeg.h"
#include "debug.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

struct http_get_file_context {
  struct http_request_handler base;
  struct http_client_ctx * client_ctx;
  char * mime_type;
  struct stat stat;
  int fd;

};

static void on_http_get_saved_stream_run(void * cookie)
{
  struct http_get_file_context * cc = cookie;
  const size_t BUFF_SIZE = 4 * 1024;
  uint8_t buf[BUFF_SIZE];
  ssize_t size, sent;

  bool fok;

  if ( cc->fd == -1 ) {
    goto end;
  }


  fok = http_ssend(cc->client_ctx,
      "%s 200 OK\r\n"
          "Content-Type: %s\r\n"
          "Content-Length: %ld\r\n"
          "Connection: close\r\n"
          "Server: ffsrv\r\n"
          "\r\n",
      cc->client_ctx->req.proto,
      cc->mime_type,
      (long) cc->stat.st_size);

  if ( !fok ) {
    goto end;
  }

  sent = 0;

  while ( sent < cc->stat.st_size && (size = read(cc->fd, buf, BUFF_SIZE)) > 0 ) {
    sent += size;
    if ( http_write(cc->client_ctx, buf, size) != size ) {
      PDBG("http_write() fails: %s", strerror(errno));
      break;
    }
  }

end:

  if ( cc->fd != -1 ) {
    close(cc->fd);
  }

  return;
}


static void on_http_get_saved_stream_destroy(void * cookie)
{
  struct http_get_file_context * cc = cookie;
  if ( cc->fd != -1 ) {
    close(cc->fd);
  }
  free(cc->mime_type);
}


bool http_get_file(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx,
    const char * abspath,
    const char * mimetype)
{
  struct http_get_file_context * cc = NULL;

  static const struct http_request_handler_iface iface = {
    .run = on_http_get_saved_stream_run,
    .ondestroy = on_http_get_saved_stream_destroy,
    .onbody = NULL,
    .onbodycomplete = NULL,
  };

  bool fok = false;

  * pqh = NULL;

  if ( !(cc = http_request_handler_alloc(sizeof(*cc), &iface)) ) {
    PDBG("http_request_handler_alloc() fails: %s", strerror(errno));
    http_ssend(client_ctx,
        "%s 500 Internal Server Error\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html>\r\n"
            "<body>\r\n"
            "<p>http_request_handler_alloc() fails.</p>\r\n"
            "<p>errno: %d %s</p>\r\n"
            "</body>\r\n"
            "</html>\r\n",
        client_ctx->req.proto,
        errno,
        strerror(errno));
    goto end;
  }

  cc->client_ctx = client_ctx;

  if ( (cc->fd = open(abspath, O_RDONLY)) == -1 ) {
    http_ssend(client_ctx,
        "%s 500 Internal Server Error\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html>\r\n"
            "<body>\r\n"
            "<p>open('%s') fails.</p>\r\n"
            "<p>errno: %d %s</p>\r\n"
            "</body>\r\n"
            "</html>\r\n",
            client_ctx->req.proto,
        abspath,
        errno,
        strerror(errno));
    goto end;
  }

  if ( fstat(cc->fd, &cc->stat) == -1 ) {
    http_ssend(client_ctx,
        "%s 500 Internal Server Error\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<html>\r\n"
            "<body>\r\n"
            "<p>fstat('%s') fails.</p>\r\n"
            "<p>errno: %d %s</p>\r\n"
            "</body>\r\n"
            "</html>\r\n",
            client_ctx->req.proto,
        abspath,
        errno,
        strerror(errno));
    goto end;
  }

  cc->mime_type = strdup(mimetype ? mimetype : ff_guess_file_mime_type(abspath));

  fok = true;


end:

  if ( !fok && cc ) {
    http_request_handler_destroy((struct http_request_handler ** )&cc);
  }

  return cc ? (*pqh = &cc->base) != NULL : false;
}
