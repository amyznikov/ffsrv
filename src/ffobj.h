/*
 * ffobj.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

// #pragma once

#ifndef __ffms_ffobj_h__
#define __ffms_ffobj_h__

#include <stddef.h>
#include <stdbool.h>
#include <pthread.h>
#include "ffmpeg.h"
#include "coscheduler.h"
#include "ffdb.h"


#ifdef __cplusplus
extern "C" {
#endif


typedef struct ffobject
  ffobject;


struct ff_object_iface {
  void (*on_add_ref)(void * ffobject);
  void (*on_release)(void * ffobject);
  int (*get_streams)(void * ffobject, const ffstream * const ** streams, uint * nb_streams);
  struct ffgop * (*get_gop)(void * ffobject);
};

struct ffobject {
  const struct ff_object_iface * iface;
  char * name;
  enum ffobject_type type;
  int refs;
};


bool ff_object_init(void);



void * ff_create_object(size_t objsize, enum ffobject_type type, const char * name, const struct ff_object_iface * iface);
int ff_get_object(struct ffobject ** pp, const char * name, uint type_mask);

void add_object_ref(struct ffobject * obj);
void release_object(struct ffobject * obj);


static inline enum ffobject_type get_object_type(struct ffobject * obj) {
  return obj->type;
}

static inline const char * get_object_name(struct ffobject * obj) {
  return obj->name;
}

static inline int get_streams(struct ffobject * obj, const ffstream * const ** streams, uint * nb_streams) {
  return obj->iface->get_streams(obj, streams, nb_streams);
}

static inline struct ffgop * get_gop(struct ffobject * obj) {
  return obj->iface->get_gop(obj);
}




struct ffinput;
struct ffms_create_input_args {
  void * cookie;
  int (*recv_pkt)(void * cookie, uint8_t *buf, int buf_size);
  void (*on_finish)(void *cookie, int status);
};

int ffms_create_input(struct ffinput ** input, const char * stream_path,
    const struct ffms_create_input_args * args);

void ffms_release_input(struct ffinput ** input);




struct ffoutput;
struct ffms_create_output_args {
  const char * format;
  int (*send_pkt)(void * cookie, int stream_index, uint8_t * buf, int buf_size);
  void * cookie;
};

int ffms_create_output(struct ffoutput ** output, const char * stream_path,
    const struct ffms_create_output_args * args);

void ffms_delete_output(struct ffoutput ** output);



#ifdef __cplusplus
}
#endif

#endif /* __ffms_ffobj_h__ */
