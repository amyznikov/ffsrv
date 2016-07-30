/*
 * hashfuncs.h
 *
 *  Created on: Jul 28, 2016
 *      Author: amyznikov
 *
 *  http://www.cse.yorku.ca/~oz/hash.html
 *
 *  Results are in big-endian byte order (also referred to as network byte order)
 */

#pragma once

#ifndef __ffsrv_hashfuncs_h__
#define __ffsrv_hashfuncs_h__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif


uint32_t sdbm_begin(void);
uint32_t sdbm_update(uint32_t h, const void * p, size_t n);
uint32_t sdbm_update_s(uint32_t h, const char * s);
uint32_t sdbm_finalize(uint32_t h);

static inline uint32_t sdbm(const void * p, size_t n) {
  return sdbm_finalize(sdbm_update(sdbm_begin(), p, n));
}
static inline uint32_t sdbm_s(const char * s) {
  return sdbm_finalize(sdbm_update_s(sdbm_begin(), s));
}



uint32_t djb2_begin();
uint32_t djb2_update(uint32_t h, const void * p, size_t n);
uint32_t djb2_update_s(uint32_t h, const char * s);
uint32_t djb2_finalize(uint32_t h);

static inline uint32_t djb2(const void * p, size_t n) {
  return djb2_finalize(djb2_update(djb2_begin(), p, n));
}
static inline uint32_t djb2_s(const char * s) {
  return djb2_finalize(djb2_update_s(djb2_begin(), s));
}

#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_hashfuncs_h__ */
