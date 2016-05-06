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
 *   source = cam1 cam2
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
static struct comtx * g_comtx = NULL;


//typedef pthread_spinlock_t mtx_t;
typedef pthread_mutex_t mtx_t;

static mtx_t avframe_ref_mtx;
static mtx_t avpacket_ref_mtx;
static mtx_t create_object_mtx;

static inline void lock(mtx_t * mtx) {
  pthread_mutex_lock(mtx);
}

static inline void unlock(mtx_t * mtx) {
  pthread_mutex_unlock(mtx);
}




static enum object_type ffcfg_get_object(const char * name, ffobj_params * params);
static void ffcfg_cleanup_object_params(enum object_type objtype, ffobj_params * params);



bool ff_object_init(void)
{
  bool fok = false;

  if ( (errno = pthread_mutex_init(&avframe_ref_mtx, 0)) != 0 ) {
    PDBG("FATAL: pthread_spin_init(avframe_ref_mtx) fails: %s", strerror(errno));
    goto end;
  }

  if ( (errno = pthread_mutex_init(&avpacket_ref_mtx, 0)) != 0 ) {
    PDBG("FATAL: pthread_spin_init(avpacket_ref_mtx) fails: %s", strerror(errno));
    goto end;
  }

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


void ff_avframe_ref(AVFrame *dst, const AVFrame *src)
{
  lock(&avframe_ref_mtx);
  av_frame_ref(dst, src);
  unlock(&avframe_ref_mtx);
}

void ff_avframe_unref(AVFrame * f)
{
  lock(&avframe_ref_mtx);
  av_frame_unref(f);
  unlock(&avframe_ref_mtx);
}

void ff_avpacket_ref(AVPacket *dst, const AVPacket *src)
{
  lock(&avpacket_ref_mtx);
  av_packet_ref(dst, src);
  unlock(&avpacket_ref_mtx);
}


void ff_avpacket_unref(AVPacket *pkt)
{
  lock(&avpacket_ref_mtx);
  av_packet_unref(pkt);
  unlock(&avpacket_ref_mtx);
}


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

static const char * objtype2str(enum object_type type)
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


void * ff_create_object(size_t objsize, enum object_type type, const char * name, const struct ff_object_iface * iface)
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

  PDBG("ENTER: name=%s type_mask=0x%0X", name, type_mask);

  if ( (obj = ff_find_object(name, type_mask)) ) {
    PDBG("FOUND %s %s", objtype2str(obj->type), obj->name);
    goto end;
  }

  PDBG("C ffcfg_get_object(%s)", name);
  if ( (objtype = ffcfg_get_object(name, &objparams)) == object_type_unknown ) {
    status = AVERROR(errno);
    goto end;
  }
  PDBG("R ffcfg_get_object(%s): type=%s", name, objtype2str(objtype));

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

  ffcfg_cleanup_object_params(objtype, &objparams);

  * pp = obj;

  return status;
}





////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




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
      PDBG("FOUND CONFING FOR %s %s", stype, sname);
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
            params->input.source = *value ? strdup(value) : NULL;
          }
          else if ( strcmp(key, "opts") == 0 ) {
            params->input.opts = *value ? strdup(value) : NULL;
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
  enum object_type type = object_type_unknown;
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

  if ( (type = ffcfg_get_object(stream_name, &objparams)) != object_type_input ) {
    status = AVERROR(errno);
    PDBG("ffcfg_get_object(%s) fails: type=%s %s", stream_name, objtype2str(type), av_err2str(status));
    goto end;
  }

  status = ff_create_input(&obj, &(struct ff_create_input_args ) {
        .name = stream_name,
        .params = &objparams.input,
        .cookie = args->cookie,
        .recv_pkt = args->recv_pkt,
        .on_finish = args->on_finish
      });

  if ( status ) {
    PDBG("ff_create_input(%s) fails: %s", stream_name, av_err2str(status));
  }
  else {
    *input = (struct ffinput *) obj;
  }

end:

  ffcfg_cleanup_object_params(object_type_input, &objparams);

  return status;
}

void ffms_release_input(struct ffinput ** input)
{
  if ( input && *input ) {
    release_object((struct ffobject * )*input);
    *input = NULL;
  }
}
