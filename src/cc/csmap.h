/*
 * csmap.h
 *
 *  Created on: Mar 18, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __csmap_h__
#define __csmap_h__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <malloc.h>
#include <string.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif


#define smap_npos (SIZE_MAX)

typedef
struct csmap_entry {
  char * key;
  char * value;
} csmap_entry;

typedef
struct csmap {
  struct csmap_entry * items;
  size_t capacity, size;
} csmap;


typedef
int (*csmap_cmpfunc_t)(
    const void * p1,
    const void * p2);

static inline int csmap_key_cmp1(const void * p1, const void * p2)
{
  return strcmp(((const csmap_entry * )p1)->key, ((const csmap_entry * )p2)->key);
}

static inline int csmap_key_cmp2(const void * p1, const void * p2)
{
  return strcmp(((const csmap_entry * )p1)->key, p2);
}

static inline void csmap_init(csmap * map)
{
  memset(map, 0, sizeof(*map));
}

static inline void csmap_cleanup(csmap * map)
{
  for ( size_t i = 0; i < map->size; ++i ) {
    free(map->items[i].key);
    free(map->items[i].value);
  }
  free(map->items);
  memset(map, 0, sizeof(*map));
}


static inline size_t csmap_lowerbound(const csmap * c, size_t beg, size_t end, csmap_cmpfunc_t cmp, const void * item)
{
  const size_t origbeg = beg;
  size_t len = end - beg;
  size_t half, mid;
  int rc;

  while ( len > 0 ) {

    mid = beg + (half = len >> 1);
    rc = cmp(c->items + mid, item);

    if ( rc == 0 ) {
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

  while ( beg > origbeg && cmp(c->items + (beg - 1), item) == 0 ) {
    --beg;
  }

  return beg;
}



static inline bool csmap_push(csmap * map, char * key, char * value)
{
  size_t pos;

  pos = csmap_lowerbound(map, 0, map->size, csmap_key_cmp2, key);

  if ( pos < map->size && csmap_key_cmp2(map->items+pos, key) == 0 ) {
    free(map->items[pos].key), map->items[pos].key = key;
    free(map->items[pos].value), map->items[pos].value = value;
    return true;
  }

  if ( map->size == map->capacity ) {
      map->items = realloc(map->items, (map->capacity + 16) * sizeof(*map->items));
    memset(map->items + map->capacity, 0, 16 * sizeof(*map->items));
    map->capacity += 16;
  }

  if ( pos < map->size ) {
    memmove(map->items+pos+1,map->items+pos, (map->size - pos) * sizeof(*map->items));
  }

  map->items[pos].key = key;
  map->items[pos].value = value;
  ++map->size;

  return true;
}

static inline const char * csmap_get(const csmap * map, const char * key)
{
  size_t pos;
  if ( (pos = csmap_lowerbound(map, 0, map->size, csmap_key_cmp2, key)) < map->size ) {
    if ( strcmp(key, map->items[pos].key ) == 0 ) {
      return map->items[pos].value;
    }
  }
  return NULL;
}

static inline size_t csmap_size(const csmap * map)
{
  return map->size;
}

static inline const csmap_entry * csmap_item(const csmap * map, size_t index)
{
  return &map->items[index];
}


#ifdef __cplusplus
}
#endif

#endif /* __csmap_h__ */
