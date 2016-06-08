/*
 * http-post-video-stream.c
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */

#include "http-post-online-stream.h"
#include "ffinput.h"
#include "url-parser.h"
#include "debug.h"


struct http_post_online_stream_context {
  struct http_request_handler base;
  struct http_client_ctx * client_ctx;
  struct ffinput * input;
  uint8_t * body;
  size_t body_capacity;
  size_t body_size;
  size_t body_pos;
};

static int on_http_recvpkt(void * cookie, uint8_t *buf, int buf_size)
{
  struct http_post_online_stream_context * cc = cookie;
  struct http_client_ctx * client_ctx = cc->client_ctx;
  ssize_t size;
  int copy_size;

  if ( client_ctx->status ) {
    return client_ctx->status;
  }

  while ( !cc->body_size ) {
    if ( (size = http_read(client_ctx)) <= 0 ) {
      if ( size == 0 ) {
        client_ctx->status = AVERROR_EOF;
      }
      else if ( errno == EAGAIN ) { // return EAGAIN will lead to infinite loop in avformat_find_stream_info()
        client_ctx->status = AVERROR(ETIMEDOUT);
      }
      else {
        client_ctx->status = AVERROR(errno);
      }
      return client_ctx->status;
    }
  }

  size = cc->body_size - cc->body_pos;
  copy_size = buf_size < size ? buf_size : size;

  memcpy(buf, cc->body + cc->body_pos, copy_size);

  if ( (cc->body_pos += copy_size) == cc->body_size ) {
    cc->body_pos = cc->body_size = 0;
  }

  return copy_size;
}

static void on_http_post_online_stream_run(void * cookie)
{
  struct http_post_online_stream_context * cc = cookie;
  if ( cc->input ) {
    ff_run_input_stream(cc->input);
  }
}

static bool on_http_post_online_stream_body(void * cookie, const uint8_t at[], size_t length)
{
  struct http_post_online_stream_context * cc = cookie;

  if ( cc->body == NULL ) {
    size_t initial_capacity = 32 * 1024;
    cc->body = malloc(cc->body_capacity = initial_capacity);
  }

  if ( cc->body != NULL ) {
    size_t capacity_required = (cc->body_size + length);
    if ( capacity_required > cc->body_capacity ) {
      capacity_required = (capacity_required + 1023) & ~1023UL;
      cc->body = realloc(cc->body, capacity_required);
      cc->body_capacity = capacity_required;
    }
  }

  memcpy(cc->body + cc->body_size, at, length);
  cc->body_size += length;

  return true;
}


static void on_http_post_online_stream_destroy(void * cookie)
{
  struct http_post_online_stream_context * cc = cookie;
  release_input_stream(&cc->input);
}



bool create_http_post_online_stream_context(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx)
{
  struct http_post_online_stream_context * cc = NULL;

  static const struct http_request_handler_iface iface = {
    .run = on_http_post_online_stream_run,
    .ondestroy = on_http_post_online_stream_destroy,
    .onbody = on_http_post_online_stream_body,
    .onbodycomplete = NULL,
  };

  char input_name[256] = "";
  char opts[256] = "";

  const http_request * q = &client_ctx->req;
  const char * url = q->url;
  const char * ifmt = NULL;

  int status = 0;

  while ( *url == '/' ) {
    ++url;
  }

  split_stream_path(url, input_name, sizeof(input_name), opts, sizeof(opts));

  if ( (ifmt = strstr(opts, "fmt=")) ) {
    ifmt += 4;
  }

  if ( !(cc = http_request_handler_alloc(sizeof(*cc), &iface)) ) {
    PDBG("http_request_handler_alloc() fails: %s", av_err2str(status));
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
        q->proto,
        errno,
        strerror(errno));
    goto end;
  }

  cc->client_ctx = client_ctx;

  status = create_input_stream(&cc->input, input_name,
      &(struct create_input_args ) {
            .cookie = cc,
            .recv_pkt = on_http_recvpkt
          });


  if ( status ) {

    char * errmsg;

    PDBG("create_input() fails: %s", av_err2str(status));

    switch ( errno ) {
      case AVERROR(ENOENT) :
        errmsg = "404 NOT FOUND";
      break;
      case AVERROR(EPERM):
        errmsg = "405 Not Allowed";
      break;
      default :
        errmsg = "500 Internal Server Error";
      break;
    }

    http_ssend(client_ctx,
        "%s %s\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n"
        "\r\n"
        "<html>\r\n"
        "<body>\r\n"
        "<h1>ERROR</h1>\r\n"
        "<p>ff_start_input_stream('%s') FAILS</p>\r\n"
        "<p>status=%d (%s)</p>\r\n"
        "</body>\r\n"
        "</html>\r\n",
        q->proto,
        errmsg,
        input_name,
        status,
        av_err2str(status));
  }

end:

  return cc ? (*pqh = &cc->base) != NULL : false;
}
