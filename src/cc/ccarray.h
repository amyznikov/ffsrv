/*
 * ccarray.h
 *
 *  Created on: Dec 15, 2011
 *      Author: amyznikov
 */

#pragma once

#ifndef __ccarray_h__
#define __ccarray_h__

#include <stddef.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#ifndef __symbian__
# include <malloc.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef
struct ccarray_t {
  void * items;
  size_t capacity;
  size_t size;
  size_t item_size;
} ccarray_t;

#define ccarray_npos            ((size_t)(-1))
#define ccarray_item_type(c)    uint8_t(*)[(c)->item_size]
#define ccarray_item(c,pos)     (((ccarray_item_type(c))(c)->items)+(pos))



typedef int (*cmpfunc_t)(const void * v1, const void * v2);


static inline ccarray_t * ccarray_init(ccarray_t * c, size_t capacity, size_t item_size)
{
  if ( c ) {
    if ( !(c->items = calloc(capacity, item_size)) ) {
      c = NULL;
    }
    else {
      c->capacity = capacity;
      c->size = 0;
      c->item_size = item_size;
    }
  }
  return c;
}

static inline void ccarray_cleanup(ccarray_t * c)
{
  if ( c && c->items ) {
    free(c->items), c->items = NULL;
  }
}

static inline ccarray_t * ccarray_create(size_t capacity, size_t item_size)
{
  ccarray_t * c;
  if ( (c = (ccarray_t *) calloc(1, sizeof(ccarray_t))) && !ccarray_init(c, capacity, item_size) ) {
    free(c), c = NULL;
  }
  return c;
}

static inline void ccarray_destroy(ccarray_t * c)
{
  if ( c ) {
    ccarray_cleanup(c);
    free(c);
  }
}

static inline size_t ccarray_realloc(ccarray_t * c, size_t capacity)
{
  void * p;
  if ( (p = realloc(c->items, capacity * c->item_size) ) ) {
    c->items = p;
    if ( c->size > (c->capacity = capacity) ) {
      c->size = capacity;
    }
  }
  return c->capacity;
}


static inline size_t ccarray_resize(ccarray_t * c, size_t size)
{
  const size_t old_size = c->size;

  if ( ccarray_realloc(c, size) == size && size > old_size ) {
    memset((uint8_t*) c->items + old_size * c->item_size, 0, (size - old_size) * c->item_size);
    c->size = size;
  }

  return c->size;
}

static inline size_t ccarray_size(const ccarray_t * c)
{
  return c ? c->size : 0;
}

static inline size_t ccarray_capacity(const ccarray_t * c)
{
  return c ? c->capacity : 0;
}

static inline size_t ccarray_push_back(ccarray_t * c, const void * restrict data)
{
  if ( c->size < c->capacity ) {
    size_t pos = c->size++;
    memcpy(ccarray_item(c, pos), data, c->item_size);
    return pos;
  }
  return ccarray_npos;
}

static inline size_t ccarray_ppush_back(ccarray_t * c, const void * ptr)
{
  return ccarray_push_back(c, &ptr);
}


static inline size_t ccarray_push_front(ccarray_t * c, const void * data)
{
  if ( c->size < c->capacity ) {
    memmove(ccarray_item(c, 1), c->items, c->size * c->item_size);
    memcpy(c->items, data, c->item_size);
    return 0;
  }
  return ccarray_npos;
}

static inline size_t ccarray_insert(ccarray_t * c, size_t pos, const void * restrict data)
{
  if ( c->size < c->capacity ) {
    memmove(ccarray_item(c, pos + 1), ccarray_item(c, pos), (c->size++ - pos) * c->item_size);
    memcpy(ccarray_item(c, pos), data, c->item_size);
    return pos;
  }
  return ccarray_npos;
}

static inline size_t ccarray_pop_back(ccarray_t * c, void * restrict data)
{
  if ( c->size < 1 ) {
    return ccarray_npos;
  }

  memcpy(data, ccarray_item(c, --c->size), c->item_size);

  return c->size;
}

static inline size_t ccarray_pop_front(ccarray_t * c, void * restrict data)
{
  if ( c->size < 1 ) {
    return ccarray_npos;
  }

  if ( data ) {
    memcpy(data, c->items, c->item_size);
  }

  memmove(c->items, (uint8_t*) c->items + c->item_size, --c->size * c->item_size);
  return c->size;
}

static inline size_t ccarray_erase(ccarray_t * c, size_t pos)
{
  if ( pos < c->size ) {
    memmove(ccarray_item(c, pos), ccarray_item(c, pos + 1), (--c->size - pos) * c->item_size);
  }
  return c->size;
}

static inline void ccarray_clear(ccarray_t * c)
{
  if ( c ) {
    c->size = 0;
  }
}

static inline void ccarray_pclear(ccarray_t * c)
{
  if ( c ) {
    for ( size_t i = 0; i < c->size; ++i ) {
      free( ((void **) c->items)[i] );
    }
    ccarray_clear(c);
  }
}

static inline void * ccarray_peek(const ccarray_t * c, size_t pos) {
  return ccarray_item(c, pos);
}

static inline void * ccarray_ppeek(const ccarray_t * c, size_t pos) {
  return ((void**)c->items)[pos];
}

static inline void * ccarray_peek_end(const ccarray_t * c) {
  return ccarray_item(c, c->size);
}

static inline size_t ccarray_set_size(ccarray_t * c, size_t newsize) {
  return (c->size = newsize);
}

static inline size_t ccarray_find(const ccarray_t * c, cmpfunc_t cmp, const void * value)
{
  size_t pos = 0;
  while ( pos < c->size && cmp(ccarray_item(c, pos), value) != 0 ) {
    ++pos;
  }
  return pos;
}

static inline size_t ccarray_find_item(const ccarray_t * c, const void * restrict value)
{
  size_t pos = 0;
  while ( pos < c->size && memcmp(ccarray_item(c, pos), value, c->item_size) != 0 ) {
    ++pos;
  }
  return pos;
}

static inline size_t ccarray_erase_item(ccarray_t * c, const void * restrict value)
{
  size_t pos = ccarray_find_item(c, value);
  if ( pos < ccarray_size(c) ) {
    ccarray_erase(c, pos);
  }
  return pos;
}

static inline void ccarray_sort(ccarray_t * c, size_t beg, size_t end, cmpfunc_t cmp)
{
  qsort(ccarray_item(c, beg), end - beg, c->item_size, cmp);
}

static inline size_t ccarray_lowerbound(const ccarray_t * c, size_t beg, size_t end, cmpfunc_t cmp, const void * restrict value)
{
  const size_t oldbeg = beg;
  size_t len = end - beg;
  size_t half, mid;
  int rc;

  while ( len > 0 ) {

    mid = beg + (half = len >> 1);

    if ( (rc = cmp(ccarray_item(c, mid), value)) == 0 ) {
      beg = mid;
      break;
    }

    if ( rc > 0 ) {
      len = half;
    }
    else {
      beg = mid + 1;
      len -= (half + 1);
    }
  }

  while ( beg > oldbeg && cmp(ccarray_item(c, beg - 1), value) == 0 ) {
    --beg;
  }

  return beg;
}

#ifdef __cplusplus
}
#endif

#endif /* __ccarray_h__ */
