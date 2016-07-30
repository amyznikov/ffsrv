/*
 * http-get-online-stream.c
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




bool http_get_online_stream(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx, const char * urlpath, const char * urlargs)
{

  struct http_get_video_stream_context * cc = NULL;

  static const struct http_request_handler_iface iface = {
    .run = on_http_get_online_stream_run,
    .ondestroy = on_http_get_online_stream_destroy,
    .onbody = NULL,
    .onbodycomplete = NULL,
  };

  const char * ofmt = NULL;

  int status = 0;

  if ( (ofmt = strstr(urlargs, "fmt=")) ) {
    ofmt += 4;
  }

  if ( !(cc = http_request_handler_alloc(sizeof(*cc), &iface)) ) {
    http_send_500_internal_server_error(client_ctx,
        "<p>http_request_handler_alloc() fails.</p>\n"
            "<p>errno: %d %s</p>\n",
        errno,
        strerror(errno));
    goto end;
  }

  cc->client_ctx = client_ctx;

  PDBG("urlpath=%s", urlpath);
  status = create_output_stream(&cc->output, urlpath,
      &(struct create_output_args ) {
            .format = ofmt && *ofmt ? ofmt : "matroska",
            .send_pkt = on_http_sendpkt,
            .getoutspc = on_http_getoutspc,
            .cookie = cc,
          });

  if ( status ) {

    PDBG("ff_create_output_stream() fails: %s", av_err2str(status));

    switch ( status ) {
      case AVERROR(ENOENT) :
        http_send_404_not_found(client_ctx);
      break;
      case AVERROR(EPERM) :
        http_send_405_not_allowed(client_ctx);
      break;
      default :
        http_send_500_internal_server_error(client_ctx,
            "<h1>ERROR</h1>\n"
                "<p>create_output_stream('%s') FAILS</p>\n"
                "<p>status=%d (%s)</p>\n",
            status, av_err2str(status));
      break;
    }

    goto end;
  }

  http_send_200_OK_ncl(client_ctx, ff_get_output_mime_type(cc->output),
      "Cache-Control: no-cache, no-store, must-revalidate\r\n"
          "Pragma: no-cache\r\n"
          "Expires: 0\r\n");

end: ;

  return cc ? (*pqh = &cc->base) != NULL : false;
}
