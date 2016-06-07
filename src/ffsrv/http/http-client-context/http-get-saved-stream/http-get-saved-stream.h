/*
 * http-get-saved-stream.h
 *
 *  Created on: Jun 7, 2016
 *      Author: amyznikov
 */


#ifndef __ffsrv_http_get_saved_stream_h__
#define __ffsrv_http_get_saved_stream_h__

#include "http-client-context.h"

#ifdef __cplusplus
extern "C" {
#endif

bool create_http_get_saved_stream_context(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx);




#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_http_get_saved_stream_h__ */
