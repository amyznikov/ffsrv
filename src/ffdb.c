/*
 * ffdb.c
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#include "ffdb.h"
#include "ffcfg.h"
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include "debug.h"
#include "ffms-pg.h"
#include "ffms-sqlite3.h"
#include "ffms-txtfile.h"


static bool (*find_object_proc)(const char * name, enum ffobject_type * type, ffobj_params * params);


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

bool ffms_setup_db(void)
{
  bool fok = false;

  switch ( ffms.db.type ) {

    case ffmsdb_txtfile :
      if ( (fok = ffms_txtfile_setup()) ) {
        find_object_proc = ffms_txtfile_find_object;
      }
    break;

    case ffmsdb_sqlite3 :
      if ( (fok = ffms_sqlite3_setup()) ) {
        find_object_proc = ffms_sqlite3_find_object;
      }
    break;

    case ffmsdb_pg : {
      if ( (fok = ffms_pg_setup()) ) {
        find_object_proc = ffms_pg_find_object;
      }
      break;

      default :
      errno = EINVAL;
      break;
    }
  }

  return fok;
}

bool ffms_find_object(const char * name, enum ffobject_type * type, ffobj_params * params)
{
  bool fok;

  if ( find_object_proc ) {
    fok = find_object_proc(name, type, params);
  }
  else {
    errno = EPERM;
    fok = false;
  }

  return fok;
}


void ffms_cleanup_object_params(enum ffobject_type objtype, ffobj_params * params)
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
