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

#include "pathfuncs.h"
#include "co-scheduler.h"
#include "strfuncs.h"
#include "ccarray.h"
#include "ffinput.h"
#include "ffoutput.h"
#include "ffsegments.h"
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







void * create_object(size_t objsize, enum ffobjtype type, const char * name, const struct ffobject_iface * iface)
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
  if ( obj ) {
    if ( obj->iface->on_release_ref ) {
      obj->iface->on_release_ref(obj);
    }
    if ( --obj->refs == 0 ) {
      delete_object(obj);
    }
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

static int get_object(struct ffobject ** pp, const char * urlpath, uint type_mask)
{
  struct ffobject * obj = NULL;

  enum ffobjtype objtype = ffobjtype_unknown;
  union ffobjparams objparams;

  int status = 0;

  memset(&objparams, 0, sizeof(objparams));

  if ( (obj = find_object(urlpath, type_mask)) ) {
    PDBG("FOUND %s %s", objtype2str(obj->type), obj->name);
    goto end;
  }

  PDBG("urlpath=%s", urlpath);
  if ( !ffdb_load_object_params(urlpath, &objtype, &objparams) || objtype == ffobjtype_unknown ) {
    PDBG("ffdb_load_object_params() fails: objtype=%s %s", objtype2str(objtype), strerror(errno));
    status = AVERROR(errno);
    goto end;
  }

  switch ( objtype ) {

    case ffobjtype_input : {

      ffobject * input = NULL;

      if ( ! (type_mask & (ffobjtype_input | ffobjtype_decoder)) ) {
        status = AVERROR(ENOENT);
        goto end;
      }

      if ( (type_mask & ffobjtype_decoder) && (input = find_object(urlpath, ffobjtype_input)) ) {
        PDBG("FOUND EXISTING %s %s", objtype2str(input->type), input->name);
      }
      else {
        status = ff_create_input(&input, &(struct ff_create_input_args ) {
              .name = urlpath,
              .params = &objparams.input
            });
        if ( status ) {
          PDBG("ff_create_input(%s) fails: %s", urlpath, av_err2str(status));
          goto end;
        }
      }

      if ( !(type_mask & ffobjtype_decoder) ) {
        obj = input;
      }
      else {

        // decoder requires input as source

        status = ff_create_decoder(&obj, &(struct ff_create_decoder_args) {
              .name = urlpath,
              .source = input,
              .opts = ff_input_decopts((const struct ffinput * )input),
            });

        if ( status ) {
          PDBG("ff_create_decoder(%s) fails: %s", urlpath, av_err2str(status));
          release_object(input);
        }
      }
    }
    break;

    case ffobjtype_encoder : {

      // encoder requires decoder as source

      const char * decoder_name = objparams.encoder.source;
      struct ffobject * decoder = NULL;

      if ( (status = get_object(&decoder, decoder_name, ffobjtype_decoder)) ) {
        PDBG("ff_get_object(decoder='%s') fails: %s", decoder_name, av_err2str(status));
        break;
      }

      status = ff_create_encoder(&obj, &(struct ff_create_encoder_args ) {
            .name = urlpath,
            .source = decoder,
            .params = &objparams.encoder
          });

      if ( status ) {
        PDBG("ff_create_encoder(%s) fails: %s", urlpath, av_err2str(status));
        release_object(decoder);
      }
    }
    break;

    case ffobjtype_segments : {

      // segments requires packetized source
      const char * source_name = objparams.segments.source;
      struct ffobject * source = NULL;

      if ( (status = get_object(&source, source_name, ffobjtype_input | ffobjtype_encoder | ffobjtype_mixer)) ) {
        PDBG("ff_get_object(source='%s') fails: %s", source_name, av_err2str(status));
        break;
      }

      status = ff_create_segments_stream(&obj, &(struct ff_create_segments_args ) {
            .name = urlpath,
            .source = source,
            .params = &objparams.segments,
          });

      if ( status ) {
        PDBG("ff_create_segments_stream(%s) fails: %s", urlpath, av_err2str(status));
        release_object(source);
      }
    }
    break;

    case ffobjtype_mixer :
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







int create_segments_stream(struct ffsegments ** obj, const char * urlpath)
{
  struct ffobject * stream = NULL;
  int status = 0;

  *obj = NULL;

  comtx_lock(g_comtx);

  if ( (status = get_object(&stream, urlpath, ffobjtype_segments)) == 0 ) {
    *obj = (struct ffsegments *) stream;
  }
  else {
    PDBG("get_object(segments:'%s') fails %s", urlpath, av_err2str(status));
  }

  comtx_unlock(g_comtx);

  return status;
}

void release_segments_stream(struct ffsegments ** stream)
{
  if ( stream && *stream ) {
    release_object((struct ffobject *) *stream);
    *stream = NULL;
  }
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int create_output_stream(struct ffoutput ** output, const char * urlpath,
    const struct create_output_args * args)
{

  struct ffobject * source = NULL;
  int status;

  comtx_lock(g_comtx);

  if ( (status = get_object(&source, urlpath, ffobjtype_input | ffobjtype_mixer | ffobjtype_encoder)) ) {
    PDBG("get_object(%s) fails: %s", urlpath, av_err2str(status));
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
      PDBG("ff_create_output() fails: %s", av_err2str(status));
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


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////



static int add_sink(struct ffobject * input, const char * sinkname, const char * destination)
{
  struct ffobject * sink = NULL;
  int status = 0;

  add_object_ref(input);

  status = ff_create_sink(&sink, &(struct ff_create_sink_args ) {
        .name = sinkname,
        .destination = destination,
        .source = input
      });

  if ( status == 0 ) {
    release_object(sink);
  }
  else {
    PDBG("ff_create_sink('%s') fails: %s", sinkname, av_err2str(status));
    release_object(input);
  }

  return status;
}

int create_input_stream(struct ffinput ** obj, const char * stream_path,
    const struct create_input_args * args)
{
  struct ffobject * input = NULL;
  enum ffobjtype type = ffobjtype_unknown;
  ffobjparams objparams;

  char input_name[128];
  char stream_params[256];
  int status = 0;

  * obj = NULL;

  memset(&objparams, 0, sizeof(objparams));

  split_url(stream_path, input_name, sizeof(input_name), stream_params, sizeof(stream_params));

  if ( (input = find_object(input_name, ffobjtype_input)) ) {
    PDBG("ff_find_object(%s): already exists", input_name);
    release_object(input);
    status = AVERROR(EACCES);
    goto end;
  }

  if ( !ffdb_load_object_params(input_name, &type, &objparams) || type != ffobjtype_input ) {
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

  if ( objparams.input.sink && *objparams.input.sink && (status = add_sink(input, input->name, objparams.input.sink)) ) {
    PDBG("[%s] add_sink('%s') fails: %s", input->name, objparams.input.sink, av_err2str(status));
    status = 0;    // ignore this error
  }


end:

  ffdb_cleanup_object_params(ffobjtype_input, &objparams);

  return status;
}

void release_input_stream(struct ffinput ** input)
{
  if ( input && *input ) {
    release_object((struct ffobject * )*input);
    *input = NULL;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct access_hook_item {
  char * path;
  size_t plen;
  void (*fn)(void * arg);
  void * arg;
};

static ccarray_t access_hooks; // <struct access_hook_item * >
static pthread_mutex_t access_hooks_lock = PTHREAD_MUTEX_INITIALIZER;


// temporary hack for segmenter idle timer reset
void * addaccesshook(const char * path, void (*fn)(void * arg), void * arg)
{
  struct access_hook_item * ticket = NULL;
  bool fok = false;

  pthread_mutex_lock(&access_hooks_lock);

  if ( !access_hooks.capacity && !ccarray_init(&access_hooks, 1024, sizeof(struct access_hook_item*)) ) {
    goto end;
  }

  if ( ccarray_size(&access_hooks) == ccarray_capacity(&access_hooks) ) {
    errno = ENOBUFS;
    goto end;
  }

  if ( !(ticket = malloc(sizeof(*ticket))) ) {
    goto end;
  }

  if ( !(ticket->path = strdup(path)) ) {
    goto end;
  }

  ticket->plen = strlen(path);
  ticket->fn = fn;
  ticket->arg = arg;

  ccarray_ppush_back(&access_hooks, ticket);

  fok = true;

end:

  if ( !fok && ticket ) {
    free(ticket->path);
    free(ticket);
  }

  pthread_mutex_unlock(&access_hooks_lock);

  return ticket;
}

void rmaccesshook(void * ticket)
{
  struct access_hook_item * p = ticket;
  if ( p ) {
    pthread_mutex_lock(&access_hooks_lock);
    ccarray_erase_item(&access_hooks, &p);
    pthread_mutex_unlock(&access_hooks_lock);
    free(p->path);
    free(p);
  }
}

void processaccesshooks(const char * path)
{
  size_t n;
  pthread_mutex_lock(&access_hooks_lock);

  if ( (n = ccarray_size(&access_hooks)) ) {
    for ( size_t i = 0, plen = strlen(path); i < n; ++i ) {
      const struct access_hook_item * p = ccarray_ppeek(&access_hooks, i);
      if ( plen >= p->plen && strncmp(path, p->path, p->plen) == 0 ) {
        p->fn(p->arg);
        break;
      }
    }
  }
  pthread_mutex_unlock(&access_hooks_lock);
}

