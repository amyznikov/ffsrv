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

bool http_get_file(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx,
    const char * abspath,
    const char * mimetype)
{
  bool fok = false;

  * pqh = NULL;

  if ( !(fok = http_send_file(client_ctx, abspath, mimetype, csmap_get(&client_ctx->req.parms, "Range") )) ) {
    PDBG("http_send_file(%s %s) fails: %s", abspath, mimetype, strerror(errno));
  }
  else {
    PDBG("http_send_file(%s %s): OK", abspath, mimetype);
  }

  return fok;
}
