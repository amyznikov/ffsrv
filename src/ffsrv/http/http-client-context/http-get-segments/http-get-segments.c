/*
 * http-get-segments.c
 *
 *  Created on: Jul 23, 2016
 *      Author: amyznikov
 */

#include "http-get-segments.h"
#include "ffsegments.h"
#include "debug.h"

bool http_get_segments_stream(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx,
    const char * urlpath,
    const char * urlargs)
{
  (void)(urlargs);

  struct ffsegments * seg = NULL;

  const char * manifestname = NULL;
  const char * mimetype = NULL;

  bool fok = true;
  int status = 0;

  *pqh = NULL;

  status = create_segments_stream(&seg, urlpath);

  if ( status ==  0 ) {
    ff_get_segments_playlist_filename(seg, &manifestname, &mimetype);
    if ( (fok = http_send_file(client_ctx, manifestname, mimetype, NULL)) ) {
      PDBG("http_send_file(%s %s) OK", mimetype, manifestname );
    }
    else {
      PDBG("http_send_file(%s %s) FAILS", mimetype, manifestname );
    }
  }

  else {

    PDBG("ff_create_output_stream() fails: %s", av_err2str(status));

    switch ( status ) {

      case AVERROR(ENOENT) :
        fok = http_send_404_not_found(client_ctx);
      break;

      case AVERROR(EPERM) :
        fok = http_send_405_not_allowed(client_ctx);
      break;

      default :
        fok = http_send_500_internal_server_error(client_ctx, false,
            "<h1>create_segments_stream('%s') fails</h1>\r\n"
            "<p>status=%d (%s)</p>",
            urlpath, status, av_err2str(status));
      break;
    }
  }

  release_segments_stream(&seg);

  return fok;
}
