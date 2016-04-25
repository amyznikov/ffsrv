/*
 * ffcfg.c
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <ctype.h>
#include <errno.h>
#include "ffinput.h"
#include "ffmixer.h"
#include "ffencoder.h"
#include "ffdecoder.h"
#include "ccarray.h"

#include "debug.h"


static char cfgfilename[] = "ffmsdb.cfg";


/*
 *
 *
 * <input cam1
 *   source = rtsp://cam1.sis.lan
 *   ctxopts = "-rtsp_transport tcp"
 *   idle_timeout = 5s
 *   re = -1,
 *   genpts = true
 *   decopts = -maxthreads 1
 * />
 *
 * <input cam2
 *   source = rtsp://cam2.sis.lan
 *   opts = "-rtsp_transport tcp"
 *   idle_timeout = 5s
 *   re = -1,
 *   genpts = true
 * />
 *
 * <mix webcam
 *   source = cam1 cam2/640x480
 *   smap = 0:1 1:0
 * />
 *
 *
 * <output cam2/640x480
 *   source = cam2
 *   opts = -c:v libx264 -c:a aac -g 16 -crf 32 -s 640x480
 *   format = matroska
 * />
 *
 * <output cam2/800x600
 *  source = cam2
 *  opts = -c:v libx264 -c:a aac -g 16 -crf 32 -s 800x600
 *  format = asf
 * />
 *
 *
 *
 *
 * <alias
 *   webcam/640x480.asf  webcam/640x480?fmt=asf
 *   webcam/800x600.mkv  webcam/800x600?fmt=matroska
 * />
 *
 *
 */



////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



typedef
union ffobj_params {
  struct ff_input_params input;
  struct ff_mixer_params mixer;
  struct ff_encoder_params encoder;
} ffobj_params;


static ccarray_t g_objects;

static enum object_type ffcfg_get_object(const char * name, ffobj_params * params);
static void ffcfg_cleanup_object_params(enum object_type objtype, ffobj_params * params);



int ff_object_init(void)
{
  ccarray_init(&g_objects, 1000, sizeof(struct ffobject*));
  return 0;
}

void * ff_create_object(size_t objsize, enum object_type type, const char * name, const struct ff_object_iface * iface)
{
  struct ffobject * obj = calloc(1, objsize);
  if ( obj ) {
    obj->type = type;
    obj->name = strdup(name);
    obj->iface = iface;
    obj->refs = 1;
    ccarray_ppush_back(&g_objects, obj);
  }
  return obj;
}


static void delete_object(struct ffobject * obj)
{
  ccarray_erase_item(&g_objects, &obj);

  if ( obj->iface->on_release) {
    obj->iface->on_release(obj);
  }

  free(obj->name);
  free(obj);
}


void add_object_ref(struct ffobject * obj)
{
  ++obj->refs;
  if ( obj->iface->on_add_ref ) {
    obj->iface->on_add_ref(obj);
  }
}

void release_object(struct ffobject * obj)
{
  if ( obj && --obj->refs == 0 ) {
    delete_object(obj);
  }
}

struct ffobject * ff_find_object(const char * name, int obj_type_mask)
{
  struct ffobject * found = NULL;

  for ( size_t i = 0, n = ccarray_size(&g_objects); i < n; ++i ) {

    struct ffobject * obj = ccarray_ppeek(&g_objects, i);

    if ( obj_type_mask && !(get_object_type(obj) & obj_type_mask)) {
      continue;
    }

    if ( name && strcmp(get_object_name(obj), name) != 0 ) {
      continue;
    }

    add_object_ref(found = obj);
    break;
  }

  return found;
}


int ff_get_object(struct ffobject ** pp, const char * name, uint type_mask)
{
  struct ffobject * obj = NULL;

  enum object_type objtype = object_type_unknown;
  union ffobj_params objparams;

  int status = 0;

  memset(&objparams, 0, sizeof(objparams));

  PDBG("find_object('%s')", name);

  if ( (obj = ff_find_object(name, type_mask)) ) {
    PDBG("FOUND('%s')", name);
    goto end;
  }

  PDBG("ffcfg_get_object('%s')", name);
  if ( (objtype = ffcfg_get_object(name, &objparams)) == object_type_unknown ) {
    status = AVERROR(errno);
    goto end;
  }

  if ( !(objtype & type_mask) ) {
    status = AVERROR(ENOENT);
    goto end;
  }

  switch ( objtype ) {

    case object_type_input : {
      status = ff_create_input(&obj, &(struct ff_create_input_args ) {
            .name = name,
            .params = &objparams.input
          });

    }
    break;

    case object_type_encoder : {

      // encoder requires decoder as source

      const char * decoder_name = objparams.encoder.source;
      struct ffobject * decoder = NULL;

      status = ff_get_object(&decoder, decoder_name, object_type_decoder);
      if ( status ) {
        break;
      }

      status = ff_create_encoder(&obj, &(struct ff_create_encoder_args ) {
            .name = name,
            .decoder = decoder,
            .params = &objparams.encoder
          });

      if ( status ) {
        release_object(decoder);
      }
    }
    break;


    case object_type_decoder : {

      // decoder requires input as source

      const char * source_name = name;
      struct ffobject * source = NULL;

      status = ff_get_object(&source, source_name, object_type_input | object_type_mixer );
      if ( status ) {
        break;
      }

      status = ff_create_decoder(&obj, &(struct ff_create_decoder_args) {
            .name = name,
            .source = source,
          });

      if ( status ) {
        release_object(source);
      }
    }
    break;



    case object_type_mixer :
      status = AVERROR(ENOSYS);
      break;

    default:
      status = AVERROR_UNKNOWN;
      break;
  }


end:

  ffcfg_cleanup_object_params(objtype, &objparams);

  * pp = obj;

  return status;
}





////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static enum object_type str2objtype(const char * stype)
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


static int sstrncmp(const char * s1, const char * s2, size_t n)
{
  while ( isspace(*s1) ) {
    ++s1;
  }

  while ( isspace(*s2) ) {
    ++s2;
  }

  return strncmp(s1, s2, n);
}


static bool get_param(const char * line, char key[128], char value[512])
{
  memset(key, 0, 128);
  memset(value, 0, 512);
  return sscanf(line, " %127[A-Za-z1-9_:-.] = %511[^#\n]", key, value) >= 1 && *key != '#';
}


static enum object_type ffcfg_get_object(const char * name, ffobj_params * params)
{
  FILE * fp = NULL;

  char line[1024] = "";

  char stype[64] = "";
  char sname[512] = "";

  char key[128] = "";
  char value[512] = "";

  bool fok = false;
  bool found = false;
  enum object_type type = object_type_unknown;

  memset(params, 0, sizeof(*params));

  if ( !(fp = fopen(cfgfilename, "r")) ) {
    PDBG("fopen('%s') fails", cfgfilename);
    goto end;
  }

  while ( fgets(line, sizeof(line), fp) ) {
    if ( sscanf(line, " <%63s %511s", stype, sname) == 2 && strcmp(sname, name) == 0 ) {
      found = true;
      PDBG("FOUND : %s %s", stype, sname);
      break;
    }
  }


  if ( !found || (type = str2objtype(stype)) == object_type_unknown ) {
    PDBG("str2objtype(%s): %d", stype, type);
    goto end;
  }

  switch ( type ) {

    case object_type_input : {

      while ( fgets(line, sizeof(line), fp) ) {

        if ( sstrncmp(line, "/>", 2) == 0 ) {
          fok = true;
          break;
        }

        if ( get_param(line, key, value) ) {
          if ( strcmp(key, "source") == 0 ) {
            params->input.source = strdup(value);
          }
          else if ( strcmp(key, "opts") == 0 ) {
            params->input.opts = strdup(value);
          }
          else if ( strcmp(key, "re") == 0 ) {
            sscanf(value, "%d", &params->input.re);
          }
          else if ( strcmp(key, "genpts") == 0 ) {
            sscanf(value, "%d", &params->input.genpts);
          }
          else {
            // unknown property
          }
        }
      }
    }
    break;

    case object_type_encoder : {

      while ( fgets(line, sizeof(line), fp) ) {

        if ( sstrncmp(line, "/>", 2) == 0 ) {
          fok = true;
          break;
        }

        if ( get_param(line, key, value) ) {
          if ( strcmp(key, "source") == 0 ) {
            params->encoder.source = strdup(value);
          }
          else if ( strcmp(key, "opts") == 0 ) {
            params->encoder.opts = strdup(value);
          }
          else if ( strcmp(key, "format") == 0 ) {
            params->encoder.format = strdup(value);
          }
          else {
            // unknown property
          }
        }
      }
    }

    break;

    case object_type_mixer : {

      while ( fgets(line, sizeof(line), fp) ) {

        if ( sstrncmp(line, "/>", 2) == 0 ) {
          fok = true;
          break;
        }

        if ( get_param(line, key, value) ) {
          if ( strcmp(key, "smap") == 0 ) {
            params->mixer.smap = strdup(value);
          }
          else if ( strcmp(key, "sources") == 0 ) {
            char * p1 = value, *p2;
            while ( (p2 = strsep(&p1, " \t,;")) ) {
              params->mixer.sources = realloc(params->mixer.sources, (params->mixer.nb_sources + 1) * sizeof(char*));
              params->mixer.sources[params->mixer.nb_sources++] = strdup(p2);
            }
          }
          else {
            // unknown property
          }
        }
      }
    }

    break;

    default :
      break;
  }


end:

  if ( fp ) {
    fclose(fp);
  }

  if ( found && !fok ) {
    ffcfg_cleanup_object_params(type, params);
    type = object_type_unknown;
  }

  if ( type == object_type_unknown ) {
    errno = ENOENT;
  }

  return type;
}


static void ffcfg_cleanup_object_params(enum object_type objtype, ffobj_params * params)
{
  switch (objtype) {
    case object_type_input:
      free(params->input.source);
      free(params->input.opts);
    break;
    case object_type_encoder:
      free(params->encoder.source);
      free(params->encoder.format);
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



////////////////////////////////////////////////////////////////////////////////////////////////////////////////////






//bool ffcfg_get_input_params(const char * name, struct ff_input_params * params)
//{
//  FILE * fp = NULL;
//
//  char line[1024] = "";
//  char objname[64] = "";
//
//  char key[128] = "";
//  char value[512] = "";
//
//  bool fok = false;
//  bool found = false;
//
//  memset(params, 0,sizeof(*params));
//
//  if ( !(fp = fopen(cfgfilename, "r")) ) {
//    goto end;
//  }
//
//  while ( fgets(line, sizeof(line), fp) ) {
//    if ( sscanf(line, " <input %63s", objname) == 1 && strcmp(objname, name) == 0 ) {
//      found = true;
//      break;
//    }
//  }
//
//  if ( !found ) {
//    errno = ENOENT;
//    goto end;
//  }
//
//  while ( fgets(line, sizeof(line), fp) ) {
//
//    if ( sstrncmp(line, "/>", 2) == 0 ) {
//      fok = true;
//      break;
//    }
//
//    if ( get_param(line, key, value) ) {
//      if ( strcmp(key, "url") == 0 ) {
//        params->source = strdup(value);
//      }
//      else if ( strcmp(key, "ctxopts") == 0 ) {
//        params->opts = strdup(value);
//      }
//      else if ( strcmp(key, "re") == 0 ) {
//        sscanf(value, "%d", &params->re);
//      }
//      else if ( strcmp(key, "genpts") == 0 ) {
//        sscanf(value, "%d", &params->genpts);
//      }
//      else {
//      }
//    }
//  }
//
//  if ( !fok ) {
//    errno = EPROTO;
//  }
//
//end:
//
//  if ( fp ) {
//    fclose(fp);
//  }
//
//  return fok;
//}
//
//
//void ffcfg_cleanup_input_params(struct ff_input_params * params)
//{
//  free(params->source);
//  free(params->opts);
//}
//
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
//
//bool ffcfg_get_mixer_params(const char * name, struct ff_mixer_params * params)
//{
//  FILE * fp = NULL;
//
//  char line[1024] = "";
//  char objname[64] = "";
//
//  char key[128] = "";
//  char value[512] = "";
//
//  bool fok = false;
//  bool found = false;
//
//  memset(params, 0, sizeof(*params));
//
//  if ( !(fp = fopen(cfgfilename, "r")) ) {
//    goto end;
//  }
//
//  while ( fgets(line, sizeof(line), fp) ) {
//    if ( sscanf(line, " <mix %63s", objname) == 1 && strcmp(objname, name) == 0 ) {
//      found = true;
//      break;
//    }
//  }
//
//  if ( !found ) {
//    errno = ENOENT;
//    goto end;
//  }
//
//  while ( fgets(line, sizeof(line), fp) ) {
//
//    if ( sstrncmp(line, "/>", 2) == 0 ) {
//      fok = true;
//      break;
//    }
//
//    if ( get_param(line, key, value) ) {
//      if ( strcmp(key, "smap") == 0 ) {
//        params->smap = strdup(value);
//      }
//      else if ( strcmp(key, "sources") == 0 ) {
//        char * p1 = value, *p2;
//        while ( (p2 = strsep(&p1, " \t,;")) ) {
//          params->sources = realloc(params->sources, (params->nb_sources + 1) * sizeof(char*));
//          params->sources[params->nb_sources++] = strdup(p2);
//        }
//      }
//      else {
//      }
//    }
//  }
//
//  if ( !fok ) {
//    errno = EPROTO;
//  }
//
//  end :
//
//  if ( fp ) {
//    fclose(fp);
//  }
//
//  return fok;
//}
//
//void ffcfg_cleanup_mixer_params(struct ff_mixer_params * params)
//{
//  for ( size_t i = 0; i < params->nb_sources; ++i ) {
//    free(params->sources[i]);
//  }
//  free(params->sources);
//  free(params->smap);
//}
