/*
 * http-get-video-stream.c
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */


#include "http-get-online-stream.h"
#include "ffoutput.h"
#include "debug.h"

struct http_get_video_stream_context {
  struct http_request_handler base;
  struct http_client_ctx * client_ctx;
  struct ffoutput * output;
};


static void on_http_get_online_stream_run(void * cookie)
{
  struct http_get_video_stream_context * cc = cookie;
  if ( cc->output ) {
    ff_run_output_stream(cc->output);
  }
}

static void on_http_get_online_stream_destroy(void * cookie)
{
  struct http_get_video_stream_context * cc = cookie;
  delete_output_stream(&cc->output);
}

static int on_http_sendpkt(void * cookie, int stream_index, uint8_t * buf, int buf_size)
{
  (void) (stream_index);
  struct http_client_ctx * client_ctx = ((struct http_get_video_stream_context *) cookie)->client_ctx;
  ssize_t size = http_write(client_ctx, buf, buf_size);
  return (size == (ssize_t) buf_size ? 0 : AVERROR(errno));
}

bool on_http_getoutspc(void * cookie, int * outspc, int * maxspc)
{
  struct http_get_video_stream_context * cc = cookie;
  struct http_client_ctx * client_ctx = cc->client_ctx;
  return http_getoutspc(client_ctx,outspc,maxspc);
}




bool create_http_get_online_stream_context(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx)
{

  struct http_get_video_stream_context * cc = NULL;

  static const struct http_request_handler_iface iface = {
    .run = on_http_get_online_stream_run,
    .ondestroy = on_http_get_online_stream_destroy,
    .onbody = NULL,
    .onbodycomplete = NULL,
  };


  char name[256] = "";
  char opts[256] = "";

  const http_request * q = &client_ctx->req;
  const char * url = q->url;
  const char * ofmt = NULL;

  const char * mime_type= NULL;

  int status;

  while ( *url == '/' ) {
    ++url;
  }

  sscanf(url,"%255[^?]?%255s", name, opts);

  if ( (ofmt = strstr(opts, "fmt=")) ) {
    ofmt += 4;
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

  status = create_output_stream(&cc->output, name,
      &(struct create_output_args ) {
            .format = ofmt && *ofmt ? ofmt : "matroska",
            .send_pkt = on_http_sendpkt,
            .getoutspc = on_http_getoutspc,
            .cookie = cc,
          });

  if ( status ) {

    char * errmsg;

    PDBG("ff_create_output_stream() fails: %s", av_err2str(status));

    switch ( status ) {
      case AVERROR(ENOENT):
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
        "<p>ff_start_output_stream('%s') FAILS</p>\r\n"
        "<p>status=%d (%s)</p>\r\n"
        "</body>\r\n"
        "</html>\r\n",
        q->proto,
        errmsg,
        name,
        status,
        av_err2str(status));

    goto end;
  }

//  PDBG("client_ctx=%p input=%p output=%p", client_ctx, client_ctx->input, client_ctx->output);

  mime_type = ff_get_output_mime_type(cc->output);

  http_ssend(client_ctx,
      "%s 200 OK\r\n"
      "Content-Type: %s\r\n"
      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
      "Pragma: no-cache\r\n"
      "Expires: 0\r\n"
      "Connection: close\r\n"
      "Server: ffsrv\r\n"
      "\r\n",
      q->proto,
      mime_type);

end: ;

  return cc ? (*pqh = &cc->base) != NULL : false;
}
