/*
 * ffgop.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */
// #pragma once

#ifndef __ffgop_h__
#define __ffgop_h__

#include "ffmpeg.h"
#include "co-scheduler.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ffgoptype {
  ffgop_pkt = 0,
  ffgop_frm = 1,
};

enum {
  ffgop_ctrl_finish = 0x01,
  ffgop_ctrl_wait_key = 0x02,
  ffgop_has_audio = 0x04,
  ffgop_has_video = 0x08,
};

struct ffgop {
  coevent * gopev;
  union { // gop array
    AVPacket * pkts;
    AVFrame ** frms;
  };

  pthread_rwlock_t rwlock;

  const ffstream ** streams;
  uint nb_streams;

  uint  capacity;
  uint  type;

  uint  oldsize; // the size of last finished gop
  uint  idx;  // gop index
  uint  wpos;  // gop write pos

  uint  refs;
  uint  ctrl;
  int   eof_status;
};



struct ffgoplistener {
  struct ffgop * gop;
  struct coevent_waiter * w;
  uint gidx, rpos;
  bool finish:1, skip_video:1;
  bool (*getoutspc)(void * cookie, int * outspc, int * maxspc);
  void * cookie;
};


struct ffgop_init_args {
  enum ffgoptype type;
  uint capacity;
  const ffstream ** streams;
  uint nb_streams;
};

int ffgop_init(struct ffgop * gop, const struct ffgop_init_args * args);
struct ffgop * ffgop_create(const struct ffgop_init_args * args);
void ffgop_set_streams(struct ffgop * gop, const ffstream ** streams, uint nb_streams);
void ffgop_cleanup(struct ffgop * gop);
void ffgop_destroy(struct ffgop ** gop);


int ffgop_set_event(struct ffgop * gop);
enum ffgoptype ffgop_get_type(const struct ffgop * gop);
bool ffgop_is_waiting_key(struct ffgop * gop);


void ffgop_put_eof(struct ffgop * gop, int reason);
int ffgop_put_pkt(struct ffgop * gop, AVPacket * pkt);
int ffgop_get_pkt(struct ffgoplistener * gl, AVPacket * pkt);


/** stream index MUST be stored as (int) (ssize_t) frm->opaque */
int ffgop_put_frm(struct ffgop * gop, AVFrame * frm);
int ffgop_get_frm(struct ffgoplistener * gl, AVFrame * frm);

struct ffgop_create_listener_args {
  bool (*getoutspc)(void * cookie, int * outspc, int * maxspc);
  void * cookie;
};

int ffgop_create_listener(struct ffgop * gop, struct ffgoplistener ** pgl,
    const struct ffgop_create_listener_args * args);

void ffgop_delete_listener(struct ffgoplistener ** gl);
int ffgop_wait_event(struct ffgoplistener * gl, int tmo);

#ifdef __cplusplus
}
#endif

#endif /* __ffgop_h__ */
