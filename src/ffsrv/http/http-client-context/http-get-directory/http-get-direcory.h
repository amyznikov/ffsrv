/*
 * http-get-direcory.h
 *
 *  Created on: Jun 7, 2016
 *      Author: amyznikov
 */


#ifndef __ffsrv_http_get_direcory_h__
#define __ffsrv_http_get_direcory_h__

#include "http-client-context.h"


#ifdef __cplusplus
extern "C" {
#endif

bool http_get_directory_listing(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx, const char * root, const char * path);



#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_http_get_direcory_h__ */
