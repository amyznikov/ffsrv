/*
 * ffdb.h
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffdb_h__
#define __ffdb_h__

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef
enum ffobject_type {
  object_type_unknown = 0x00,
  object_type_input = 0x01,
  object_type_mixer = 0x02,
  object_type_decoder = 0x04,
  object_type_encoder = 0x08,
  object_type_sink = 0x10,
} ffobject_type;


struct ffinput_params {
  char * source;
  char * opts;
  int re;
  int genpts;
  int rtmo;
  int itmo;
};


struct ffencoder_params {
  char * source;
  char * opts;
};

struct ffmixer_params {
  char ** sources;
  size_t nb_sources;
  char * smap;
};


typedef
union ffobj_params {
  struct ffinput_params input;
  struct ffmixer_params mixer;
  struct ffencoder_params encoder;
} ffobj_params;


enum ffobject_type str2objtype(const char * stype);
const char * objtype2str(enum ffobject_type type);


bool ffdb_init(void);
bool ffdb_find_object(const char * name, enum ffobject_type * type, ffobj_params * params);
void ffdb_cleanup_object_params(enum ffobject_type objtype, ffobj_params * params);




#ifdef __cplusplus
}
#endif

#endif /* __ffdb_h__ */

