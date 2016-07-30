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



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum ffmagic {
  ffmagic_unknown,
  ffmagic_input,
  ffmagic_enc,
  ffmagic_segments,
  ffmagic_directory,
  ffmagic_file,
};

typedef
enum ffobjtype {
  ffobjtype_unknown = 0x00,
  ffobjtype_input = 0x01,
  ffobjtype_mixer = 0x02,
  ffobjtype_decoder = 0x04,
  ffobjtype_encoder = 0x08,
  ffobjtype_sink = 0x10,
  ffobjtype_segments = 0x20,
} ffobjtype;


struct ffinput_params {
  char * source;
  char * opts;
  char * decopts;
  char * sink;
  int re;
  bool genpts;
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

struct ffsegments_params {
  char * source;
  char * manifest;
  char * opts;
  int rtmo;
  int itmo;
};

typedef
union ffobjparams {
  struct ffinput_params input;
  struct ffmixer_params mixer;
  struct ffencoder_params encoder;
  struct ffsegments_params segments;
} ffobjparams;



bool ffurl_magic(const char * urlpath, char ** abspath, enum ffmagic * magic, char ** mime);

enum ffobjtype str2objtype(const char * stype);
const char * objtype2str(enum ffobjtype type);

bool ffdb_load_object_params(const char * urlpath, enum ffobjtype * type, ffobjparams * params);
void ffdb_cleanup_object_params(enum ffobjtype type, ffobjparams * params);





#ifdef __cplusplus
}
#endif

#endif /* __ffdb_h__ */

