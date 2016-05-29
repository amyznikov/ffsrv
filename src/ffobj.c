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
#include "ffinput.h"
#include "ffoutput.h"
#include "ffmixer.h"
#include "ffencoder.h"
#include "ffdecoder.h"
#include "ccarray.h"

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



bool ff_object_init(void)
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







void * ff_create_object(size_t objsize, enum ffobject_type type, const char * name, const struct ff_object_iface * iface)
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

  enum ffobject_type objtype = object_type_unknown;
  union ffobj_params objparams;

  int status = 0;

  memset(&objparams, 0, sizeof(objparams));

  PDBG("ENTER: name=%s type_mask=0x%0X", name, type_mask);

  if ( (obj = ff_find_object(name, type_mask)) ) {
    PDBG("FOUND %s %s", objtype2str(obj->type), obj->name);
    goto end;
  }

  PDBG("C ffdb_get_object(%s)", name);
  if ( !ffms_find_object(name, &objtype, &objparams) || objtype == object_type_unknown ) {
    status = AVERROR(errno);
    goto end;
  }
  PDBG("R ffdb_get_object(%s): type=%s", name, objtype2str(objtype));

//  if ( !(objtype & type_mask) ) {
//    status = AVERROR(ENOENT);
//    goto end;
//  }

  switch ( objtype ) {

    case object_type_input : {

      ffobject * input = NULL;

      if ( ! (type_mask & (object_type_input | object_type_decoder)) ) {
        status = AVERROR(ENOENT);
        goto end;
      }

      if ( (type_mask & object_type_decoder) && (input = ff_find_object(name, object_type_input)) ) {
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

      PDBG("C ff_get_object('%s', object_type_decoder)", decoder_name);
      status = ff_get_object(&decoder, decoder_name, object_type_decoder);
      if ( status ) {
        PDBG("REQ encoder: ff_get_object(decoder='%s') fails: %s", decoder_name, av_err2str(status));
        break;
      }

      PDBG("DECODER=%p", decoder);

      PDBG("C ff_create_encoder('%s')", name);
      status = ff_create_encoder(&obj, &(struct ff_create_encoder_args ) {
            .name = name,
            .source = decoder,
            .params = &objparams.encoder
          });
      PDBG("R ff_create_encoder('%s'): %s", name, av_err2str(status));

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

  ffms_cleanup_object_params(objtype, &objparams);

  * pp = obj;

  return status;
}





////////////////////////////////////////////////////////////////////////////////////////////////////////////////////










////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



static void split_stream_patch(const char * stream_path, char objname[], uint cbobjname, char params[], uint cbparams)
{
  // stream_path = inputname[/outputname][?params]
  char * ps;
  size_t n;

  *objname = 0;
  *params = 0;

  if ( !(ps = strpbrk(stream_path, "?")) ) {
    strncpy(objname, stream_path, cbobjname - 1)[cbobjname - 1] = 0;
    return;
  }

  if ( (n = ps - stream_path) < cbobjname ) {
    cbobjname = n;
  }

  strncpy(objname, stream_path, cbobjname - 1)[cbobjname - 1] = 0;
  stream_path += n + 1;

  if ( *ps == '?' ) {
    strncpy(params, ps + 1, cbparams - 1)[cbparams - 1] = 0;
  }

}


int ffms_create_output(struct ffoutput ** output, const char * stream_path,
    const struct ffms_create_output_args * args)
{

  struct ffobject * source = NULL;

  char source_name[128];
  char params[256];


  int status;

  split_stream_patch(stream_path, source_name, sizeof(source_name), params, sizeof(params));

  comtx_lock(g_comtx);

  if ( (status = ff_get_object(&source, source_name, object_type_input | object_type_mixer | object_type_encoder)) ) {
    PDBG("get_object(%s) fails: %s", source_name, av_err2str(status));
  }
  else {

    status = ff_create_output(output, &(struct ff_create_output_args ) {
          .source = source,
          .format = args->format,
          .cookie = args->cookie,
          .sendpkt = args->send_pkt
        });

    if ( status ) {
      release_object(source);
    }
  }

  comtx_unlock(g_comtx);

  return status;
}


void ffms_delete_output(struct ffoutput ** output)
{
  ff_delete_output(output);
}


int ffms_create_input(struct ffinput ** input, const char * stream_path,
    const struct ffms_create_input_args * args)
{
  struct ffobject * obj = NULL;
  enum ffobject_type type = object_type_unknown;
  ffobj_params objparams;

  char stream_name[128];
  char stream_params[256];

  int status;

  * input = NULL;

  memset(&objparams, 0, sizeof(objparams));

  split_stream_patch(stream_path, stream_name, sizeof(stream_name), stream_params, sizeof(stream_params));

  if ( (obj = ff_find_object(stream_name, object_type_input)) ) {
    PDBG("ff_find_object(%s): already exists", stream_name);
    release_object(obj);
    status = AVERROR(EACCES);
    goto end;
  }

  if ( !ffms_find_object(stream_name, &type, &objparams) || type != object_type_input ) {
    PDBG("ffms_find_object(%s) fails: type=%s %s", stream_name, objtype2str(type), av_err2str(status));
    status = AVERROR(ENOENT);
    goto end;
  }

  status = ff_create_input(&obj, &(struct ff_create_input_args ) {
        .name = stream_name,
        .params = &objparams.input,
        .cookie = args->cookie,
        .recv_pkt = args->recv_pkt
      });

  if ( status ) {
    PDBG("ff_create_input(%s) fails: %s", stream_name, av_err2str(status));
  }
  else {
    *input = (struct ffinput *) obj;
  }

end:

  ffms_cleanup_object_params(object_type_input, &objparams);

  return status;
}

void ffms_release_input(struct ffinput ** input)
{
  if ( input && *input ) {
    release_object((struct ffobject * )*input);
    *input = NULL;
  }
}
