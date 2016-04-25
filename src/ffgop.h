/*
 * ffgop.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */


#pragma once
#ifndef __ffms_ffgop_h__
#define __ffms_ffgop_h__

#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include "ffmpeg.h"
#include "coscheduler.h"

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
  uint  gopsize; // capacity
  uint  gopidx;  // gop index
  uint  gopwpos;  // gop write pos
  uint  goptype;
  uint  refs;
  int   eof_status;
  bool  finish:1;

};

struct ffgoplistener {
  struct ffgop * gop;
  struct coevent_waiter * w;
  uint gopidx, goprpos;
  bool finish;
};

int ffgop_init(struct ffgop * gop, uint gopsize, enum ffgoptype type);
void ffgop_cleanup(struct ffgop * gop);

struct ffgop * ffgop_create(uint gopsize, enum ffgoptype type);
void ffgop_destroy(struct ffgop ** gop);

enum ffgoptype ffgop_get_type(const struct ffgop * gop);

int ffgop_create_listener(struct ffgop * gop, struct ffgoplistener ** pgl);
void ffgop_delete_listener(struct ffgoplistener ** gl);

void ffgop_put_eof(struct ffgop * gop, int reason);

int ffgop_put_pkt(struct ffgop * gop, AVPacket * pkt, enum AVMediaType media_type);
int ffgop_get_pkt(struct ffgoplistener * gl, AVPacket * pkt);

int ffgop_put_frm(struct ffgop * gop, AVFrame * frm, enum AVMediaType media_type);
int ffgop_get_frm(struct ffgoplistener * gl, AVFrame * frm);

int ffgop_wait_event(struct ffgoplistener * gl, int tmo);
int ffgop_set_event(struct ffgop * gop);


#ifdef __cplusplus
}
#endif

#endif /* __ffms_ffgop_h__ */
