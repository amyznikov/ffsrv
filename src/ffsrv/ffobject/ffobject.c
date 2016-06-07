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
#include <pthread.h>

#include "co-scheduler.h"
#include "ccarray.h"
#include "cctstr.h"
#include "create-directory.h"
#include "url-parser.h"
#include "ffinput.h"
#include "ffoutput.h"
#include "ffmixer.h"
#include "ffencoder.h"
#include "ffdecoder.h"
#include "ffsink.h"
#include "ffcfg.h"
#include "debug.h"





////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




static ccarray_t g_objects;
static struct comtx * g_comtx = NULL;


//typedef pthread_spinlock_t mtx_t;
typedef pthread_mutex_t mtx_t;

static mtx_t create_object_mtx;

static inline void lock(mtx_t * mtx) {
  pthread_mutex_lock(mtx);
}

static inline void unlock(mtx_t * mtx) {
  pthread_mutex_unlock(mtx);
}



bool ffobject_init(void)
{
  bool fok = false;

  if ( (errno = pthread_mutex_init(&create_object_mtx, 0)) != 0 ) {
    PDBG("FATAL: pthread_spin_init(create_object_mtx) fails: %s", strerror(errno));
    goto end;
  }


  if ( !ccarray_init(&g_objects, 1000, sizeof(struct ffobject*)) ) {
    PDBG("FATAL: ccarray_init(g_objects) fails: %s", strerror(errno));
    goto end;
  }

  if ( !(g_comtx = comtx_create())) {
    PDBG("FATAL: comtx_create() fails: %s", strerror(errno));
    goto end;
  }


  fok = true;

end:

  return fok;
}







void * create_object(size_t objsize, enum ffobject_type type, const char * name, const struct ffobject_iface * iface)
{
  struct ffobject * obj;

  if ( (obj = calloc(1, objsize)) ) {
    obj->type = type;
    obj->name = strdup(name);
    obj->iface = iface;
    obj->refs = 1;

    lock(&create_object_mtx);
    ccarray_ppush_back(&g_objects, obj);
    unlock(&create_object_mtx);
  }
  return obj;
}


static void delete_object(struct ffobject * obj)
{
  lock(&create_object_mtx);
  ccarray_erase_item(&g_objects, &obj);
  unlock(&create_object_mtx);

  if ( obj->iface->on_destroy) {
    obj->iface->on_destroy(obj);
  }

  PDBG("DESTROYED %s %s", objtype2str(obj->type), obj->name);
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


static struct ffobject * find_object(const char * name, int obj_type_mask)
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

static int get_object(struct ffobject ** pp, const char * name, uint type_mask)
{
  struct ffobject * obj = NULL;

  enum ffobject_type objtype = object_type_unknown;
  union ffobj_params objparams;

  int status = 0;

  memset(&objparams, 0, sizeof(objparams));

  if ( (obj = find_object(name, type_mask)) ) {
    PDBG("FOUND %s %s", objtype2str(obj->type), obj->name);
    goto end;
  }

  if ( !ffdb_find_object(name, &objtype, &objparams) || objtype == object_type_unknown ) {
    status = AVERROR(errno);
    goto end;
  }

  switch ( objtype ) {

    case object_type_input : {

      ffobject * input = NULL;

      if ( ! (type_mask & (object_type_input | object_type_decoder)) ) {
        status = AVERROR(ENOENT);
        goto end;
      }

      if ( (type_mask & object_type_decoder) && (input = find_object(name, object_type_input)) ) {
        PDBG("FOUND EXISTING %s %s", objtype2str(input->type), input->name);
      }
      else {
        PDBG("C ff_create_input('%s')", name);
        status = ff_create_input(&input, &(struct ff_create_input_args ) {
              .name = name,
              .params = &objparams.input
            });
        PDBG("R ff_create_input('%s'): %s", name, av_err2str(status));
        if ( status ) {
          goto end;
        }
      }

      if ( !(type_mask & object_type_decoder) ) {
        obj = input;
      }
      else {

        // decoder requires input as source

        PDBG("C ff_create_decoder('%s')", name);
        status = ff_create_decoder(&obj, &(struct ff_create_decoder_args) {
              .name = name,
              .source = input,
            });
        PDBG("R ff_create_decoder('%s'): %s", name, av_err2str(status));

        if ( status ) {
          release_object(input);
        }
      }
    }
    break;

    case object_type_encoder : {

      // encoder requires decoder as source

      const char * decoder_name = objparams.encoder.source;
      struct ffobject * decoder = NULL;

      if ( (status = get_object(&decoder, decoder_name, object_type_decoder)) ) {
        PDBG("REQ encoder: ff_get_object(decoder='%s') fails: %s", decoder_name, av_err2str(status));
        break;
      }

      status = ff_create_encoder(&obj, &(struct ff_create_encoder_args ) {
            .name = name,
            .source = decoder,
            .params = &objparams.encoder
          });

      if ( status ) {
        release_object(decoder);
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

  ffdb_cleanup_object_params(objtype, &objparams);

  * pp = obj;

  return status;
}





////////////////////////////////////////////////////////////////////////////////////////////////////////////////////










////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




int create_output_stream(struct ffoutput ** output, const char * stream_path,
    const struct create_output_args * args)
{

  struct ffobject * source = NULL;

  char source_name[128];
  char params[256];


  int status;

  split_stream_path(stream_path, source_name, sizeof(source_name), params, sizeof(params));

  comtx_lock(g_comtx);

  if ( (status = get_object(&source, source_name, object_type_input | object_type_mixer | object_type_encoder)) ) {
    PDBG("get_object(%s) fails: %s", source_name, av_err2str(status));
  }
  else {

    status = ff_create_output(output, &(struct ff_create_output_args ) {
          .source = source,
          .format = args->format,
          .cookie = args->cookie,
          .sendpkt = args->send_pkt,
          .getoutspc = args->getoutspc
        });

    if ( status ) {
      release_object(source);
    }
  }

  comtx_unlock(g_comtx);

  return status;
}


void delete_output_stream(struct ffoutput ** output)
{
  ff_delete_output(output);
}




static int add_sink(struct ffobject * input)
{
  struct ffobject * sink = NULL;
  char path[PATH_MAX] = "";
  char name[256] = "";
  int status = 0;

  if ( !ffsrv.sinks.root || !*ffsrv.sinks.root ) {
    status = AVERROR(EPERM);
    goto end;
  }

  snprintf(path, sizeof(path) - 1, "%s/%s", ffsrv.sinks.root, input->name);
  if ( !create_directory(DEFAULT_MKDIR_MODE, path) ) {
    status = AVERROR(errno);
    PDBG("create_directory('%s') fails: %s", path, strerror(errno));
    goto end;
  }

  add_object_ref(input);

  status = ff_create_sink(&sink, &(struct ff_create_sink_args ) {
        .name = getcctstr2(name),
        .path = path,
        .format = "matroska",
        .source = input
      });

  if ( status == 0 ) {
    PDBG("created sink '%s/%s'", path, name);
    release_object(sink);
  }
  else {
    PDBG("ff_create_sink('%s/%s') fails: %s", path, name, av_err2str(status));
    release_object(input);
  }

end:

  return status;
}

int create_input_stream(struct ffinput ** obj, const char * stream_path,
    const struct create_input_args * args)
{
  struct ffobject * input = NULL;
  enum ffobject_type type = object_type_unknown;
  ffobj_params objparams;

  char input_name[128];
  char stream_params[256];

  int status;

  * obj = NULL;

  memset(&objparams, 0, sizeof(objparams));

  split_stream_path(stream_path, input_name, sizeof(input_name), stream_params, sizeof(stream_params));

  if ( (input = find_object(input_name, object_type_input)) ) {
    PDBG("ff_find_object(%s): already exists", input_name);
    release_object(input);
    status = AVERROR(EACCES);
    goto end;
  }

  if ( !ffdb_find_object(input_name, &type, &objparams) || type != object_type_input ) {
    PDBG("ffdb_find_object(%s) fails: type=%s %s", input_name, objtype2str(type), av_err2str(status));
    status = AVERROR(ENOENT);
    goto end;
  }

  status = ff_create_input(&input, &(struct ff_create_input_args ) {
        .name = input_name,
        .params = &objparams.input,
        .cookie = args->cookie,
        .recv_pkt = args->recv_pkt
      });

  if ( status ) {
    PDBG("ff_create_input(%s) fails: %s", input_name, av_err2str(status));
    goto end;
  }

  *obj = (struct ffinput *) input;


  // TODO: check database if sink is really requested for this input
  if ( ffsrv.sinks.root && *ffsrv.sinks.root ) {
    if ( (status = add_sink(input)) ) {
      PDBG("[%s] add_sink() fails: %s", input_name, av_err2str(status));
    }
    status = 0; // ignore this error
  }

end:

  ffdb_cleanup_object_params(object_type_input, &objparams);

  return status;
}

void release_input_stream(struct ffinput ** input)
{
  if ( input && *input ) {
    release_object((struct ffobject * )*input);
    *input = NULL;
  }
}
