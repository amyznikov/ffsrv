/*
 * http-get-segments.h
 *
 *  Created on: Jul 23, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __http_get_segments_h__
#define __http_get_segments_h__

#include "http-client-context.h"

#ifdef __cplusplus
extern "C" {
#endif


bool http_get_segments_stream(struct http_request_handler ** pqh,
    struct http_client_ctx * client_ctx,
    const char * urlpath,
    const char * urlargs);


#ifdef __cplusplus
}
#endif

#endif /* __http_get_segments_h__ */
