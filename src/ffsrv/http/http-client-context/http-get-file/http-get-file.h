/*
 * http-get-saved-stream.h
 *
 *  Created on: Jun 7, 2016
 *      Author: amyznikov
 */


#ifndef __ffsrv_http_get_file_h__
#define __ffsrv_http_get_file_h__

#include "http-client-context.h"

#ifdef __cplusplus
extern "C" {
#endif

bool http_get_file(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx,
    const char * abspath,
    const char * mimetype);




#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_http_get_file_h__ */
