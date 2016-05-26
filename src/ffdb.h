/*
 * ffdb.h
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

// #pragma once

#ifndef __ffdb_h__
#define __ffdb_h__

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef
enum ffobject_type {
  object_type_unknown = 0,
  object_type_input = 1,
  object_type_mixer = 2,
  object_type_decoder = 4,
  object_type_encoder = 8,
} ffobject_type;


struct ff_input_params {
  char * source;
  char * opts;
  int re;
  int genpts;
};


struct ff_encoder_params {
  char * source;
  char * opts;
};

struct ff_mixer_params {
  char ** sources;
  size_t nb_sources;
  char * smap;
};


typedef
union ffobj_params {
  struct ff_input_params input;
  struct ff_mixer_params mixer;
  struct ff_encoder_params encoder;
} ffobj_params;


enum ffobject_type str2objtype(const char * stype);
const char * objtype2str(enum ffobject_type type);


bool ffms_setup_db(void);
bool ffms_find_object(const char * name, enum ffobject_type * type, ffobj_params * params);
void ffms_cleanup_object_params(enum ffobject_type objtype, ffobj_params * params);




#ifdef __cplusplus
}
#endif

#endif /* __ffdb_h__ */

