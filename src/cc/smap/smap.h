/*
 * smap.h
 *
 *  Created on: Jun 18, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffsrv_smap_h__
#define __ffsrv_smap_h__

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_SMAP_SIZE 32
struct ffsmap {
  int map[MAX_SMAP_SIZE];
};

bool ffsmap_init(struct ffsmap * smap, const char * string);
int ffsmap(struct ffsmap * smap, int stidx);

#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_smap_h__ */
