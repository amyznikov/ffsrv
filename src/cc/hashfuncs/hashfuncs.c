/*
 * hashfuncs.c
 *
 *  Created on: Jul 28, 2016
 *      Author: amyznikov
 *
 *  http://www.cse.yorku.ca/~oz/hash.html
 *
 *  Results are in big-endian byte order (also referred to as network byte order)
 *
 */

#include "hashfuncs.h"
#include <endian.h>


uint32_t sdbm_begin(void)
{
  return 0;
}

uint32_t sdbm_update(uint32_t h, const void * p, size_t n)
{
  const uint8_t * s = p;
  while ( n-- ) {
    h = *s++ + (h << 6) + (h << 16) - h;
  }
  return h;
}

uint32_t sdbm_update_s(uint32_t h, const char * s)
{
  while ( *s ) {
    h = (uint8_t)*s++ + (h << 6) + (h << 16) - h;
  }
  return h;
}

uint32_t sdbm_finalize(uint32_t h)
{
  return htobe32(h);
}











uint32_t djb2_begin()
{
  return 5381;
}

uint32_t djb2_update(uint32_t h, const void * p, size_t n)
{
  const uint8_t * s = p;
  while ( n-- ) {
    h = ((h << 5) + h) + (uint8_t) *s++;
  }
  return h;
}

uint32_t djb2_update_s(uint32_t h, const char * s)
{
  while ( *s ) {
    h = ((h << 5) + h) + (uint8_t) *s++;
  }
  return h;
}

uint32_t djb2_finalize(uint32_t h)
{
  return htobe32(h);
}






