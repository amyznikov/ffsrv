/*
 * ffobj.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

// #pragma once

#ifndef __ffobject_h__
#define __ffobject_h__

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include "ffmpeg.h"
#include "ffdb.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef struct ffobject
  ffobject;


struct ffobject_iface {
  void (*on_add_ref)(void * ffobject);
  void (*on_release_ref)(void * ffobject);
  void (*on_destroy)(void * ffobject);
  int (*get_streams)(void * ffobject, const ffstream * const ** streams, uint * nb_streams);
  struct ffgop * (*get_gop)(void * ffobject);
};

struct ffobject {
  const struct ffobject_iface * iface;
  char * name;
  enum ffobjtype type;
  int refs;
};


bool ffobject_init(void);


void * create_object(size_t objsize, enum ffobjtype type, const char * name, const struct ffobject_iface * iface);
void add_object_ref(struct ffobject * obj);
void release_object(struct ffobject * obj);

static inline int
  get_object_refs(const struct ffobject * obj) {
  return obj->refs;
}

static inline enum ffobjtype
  get_object_type(struct ffobject * obj) {
  return obj->type;
}

static inline const char *
  get_object_name(struct ffobject * obj) {
  return obj->name;
}

static inline int
  get_streams(struct ffobject * obj, const ffstream * const ** streams, uint * nb_streams) {
  return obj->iface->get_streams(obj, streams, nb_streams);
}

static inline struct ffgop *
  get_gop(struct ffobject * obj) {
  return obj->iface->get_gop(obj);
}




struct ffinput;
struct create_input_args {
  void * cookie;
  int (*recv_pkt)(void * cookie, uint8_t *buf, int buf_size);
};

int create_input_stream(struct ffinput ** input,
    const char * stream_path,
    const struct create_input_args * args);
void release_input_stream(struct ffinput ** input);


struct ffsegments;
int create_segments_stream(struct ffsegments ** stream,const char * urlpath);
void release_segments_stream(struct ffsegments ** stream);




struct ffoutput;
struct create_output_args {
  const char * format;
  int (*send_pkt)(void * cookie, int stream_index, uint8_t * buf, int buf_size);
  bool (*getoutspc)(void * cookie, int * outspc, int * maxspc);
  void * cookie;
};

int create_output_stream(struct ffoutput ** output,
    const char * urlpath,
    const struct create_output_args * args);
void delete_output_stream(struct ffoutput ** output);




// temporary hack for segmenter idle timer reset
void * addaccesshook(const char * path, void (*fn)(void * arg), void * arg);
void rmaccesshook(void * ticket);
void processaccesshooks(const char * path);


#ifdef __cplusplus
}
#endif

#endif /* __ffobject_h__ */
