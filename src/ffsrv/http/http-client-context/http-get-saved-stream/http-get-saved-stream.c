/*
 * http-get-saved-stream.c
 *
 *  Created on: Jun 7, 2016
 *      Author: amyznikov
 */

#include "http-get-saved-stream.h"


bool create_http_get_saved_stream_context(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx)
{
  (void)(client_ctx);
  * pqh = NULL;
  return true;
}
