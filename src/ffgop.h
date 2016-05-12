/*
 * ffgop.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */
// #pragma once

#ifndef __ffms_ffgop_h__
#define __ffms_ffgop_h__

#include "ffmpeg.h"
#include "coscheduler.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

enum ffgoptype {
  ffgop_pkt = 0,
  ffgop_frm = 1,
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
  int   eof_status;
  bool finish:1, has_video:1, has_audio:1, wait_key:1;
};

struct ffgoplistener {
  struct ffgop * gop;
  struct coevent_waiter * w;
  uint gopidx, rpos, dp;
  bool finish:1, skip_video:1, enable_skip_video:1;
};

int ffgop_init(struct ffgop * gop, uint capacity, enum ffgoptype type, const ffstream ** streams, uint nb_streams);
struct ffgop * ffgop_create(uint capacity, enum ffgoptype type, const ffstream ** streams, uint nb_streams);
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


int ffgop_create_listener(struct ffgop * gop, struct ffgoplistener ** pgl);
void ffgop_delete_listener(struct ffgoplistener ** gl);
void ffgop_enable_skip_video(struct ffgoplistener * gl, bool enable);
int ffgop_wait_event(struct ffgoplistener * gl, int tmo);

#ifdef __cplusplus
}
#endif

#endif /* __ffms_ffgop_h__ */
