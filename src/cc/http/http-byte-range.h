/*
 * http-byte-range.h
 *
 *  Created on: Jul 29, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __http_http_byte_range_h__
#define __http_http_byte_range_h__

#include <stddef.h>
//#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif


struct http_byte_range {
  ssize_t firstpos, lastpos;
};


int http_parse_byte_range(const char * s, size_t content_length, struct http_byte_range ranges[], int maxranges);




#ifdef __cplusplus
}
#endif

#endif /* __http_http_byte_range_h__ */
