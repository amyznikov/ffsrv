/*
 * url-parser.h
 *
 *  Created on: Apr 20, 2016
 *      Author: amyznikov
 */


#pragma once

#ifndef __ffms_url_parser_h__
#define __ffms_url_parser_h__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


void parse_url(const char * url, char proto[], size_t proto_size, char auth[], size_t auth_size, char host[],
    size_t host_size, int * port_ptr, char path[], size_t path_size);

#ifdef __cplusplus
}
#endif

#endif /* __ffms_url_parser_h__ */
