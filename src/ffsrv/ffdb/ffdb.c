/*
 * ffdb.c
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#include "ffdb.h"
#include "ffcfg.h"
#include "ffdb-pg.h"
#include "ffdb-sqlite3.h"
#include "ffdb-txtfile.h"

#include <malloc.h>
#include <string.h>
#include <errno.h>

#include "debug.h"

static bool (*ffdb_find_object_proc)(const char * name, enum ffobject_type * type, ffobj_params * params);


enum ffobject_type str2objtype(const char * stype)
{
  if ( strcmp(stype, "input") == 0 ) {
    return object_type_input;
  }

  if ( strcmp(stype, "output") == 0 ) {
    return object_type_encoder;
  }

  if ( strcmp(stype, "mix") == 0 ) {
    return object_type_mixer;
  }

  return object_type_unknown;
}

const char * objtype2str(enum ffobject_type type)
{
  switch ( type ) {
    case object_type_input :
      return "input";
    case object_type_mixer :
      return "mix";
    case object_type_decoder :
      return "dec";
    case object_type_encoder :
      return "output";
    break;
    default :
      break;
  }

  return "unknown";
}

bool ffdb_init(void)
{
  bool fok = false;

  switch ( ffsrv.db.type ) {

    case ffdb_txtfile :
      if ( (fok = ffdb_txtfile_init()) ) {
        ffdb_find_object_proc = ffdb_txtfile_find_object;
      }
    break;

    case ffdb_sqlite3 :
      if ( (fok = ffdb_sqlite3_init()) ) {
        ffdb_find_object_proc = ffdb_sqlite3_find_object;
      }
    break;

    case ffdb_pg : {
      if ( (fok = ffdb_pg_init()) ) {
        ffdb_find_object_proc = ffdb_pg_find_object;
      }
      break;

      default :
      errno = EINVAL;
      break;
    }
  }

  return fok;
}

bool ffdb_find_object(const char * name, enum ffobject_type * type, ffobj_params * params)
{
  bool fok;

  if ( ffdb_find_object_proc ) {
    fok = ffdb_find_object_proc(name, type, params);
  }
  else {
    errno = ENOENT;
    fok = false;
  }

  return fok;
}

void ffdb_cleanup_object_params(enum ffobject_type objtype, ffobj_params * params)
{
  switch (objtype) {
    case object_type_input:
      free(params->input.source);
      free(params->input.opts);
    break;
    case object_type_encoder:
      free(params->encoder.source);
      free(params->encoder.opts);
    break;
    case object_type_mixer:
      for ( size_t i = 0; i < params->mixer.nb_sources; ++i ) {
        free(params->mixer.sources[i]);
      }
      free(params->mixer.sources);
      free(params->mixer.smap);
    break;
    default:
    break;
  }
}
