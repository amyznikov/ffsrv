/*
 * ffgop.c
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#include <malloc.h>
#include <stdlib.h>
#include "ffgop.h"
#include "debug.h"

static inline void w_lock(struct ffgop * gop)
{
  pthread_rwlock_wrlock(&gop->rwlock);
}

static inline void w_unlock(struct ffgop * gop)
{
  pthread_rwlock_unlock(&gop->rwlock);
}

static inline void r_lock(struct ffgop * gop)
{
  pthread_rwlock_rdlock(&gop->rwlock);
}

static inline void r_unlock(struct ffgop * gop)
{
  pthread_rwlock_unlock(&gop->rwlock);
}

int ffgop_set_event(struct ffgop * gop)
{
  coevent_set(gop->gopev);
  return 0;
}

int ffgop_wait_event(struct ffgoplistener * gl, int tmo)
{
  return coevent_wait(gl->w, tmo);
}


static void ffgop_check_stream_types(struct ffgop * gop)
{
  if ( gop->streams ) {
    for ( uint i = 0; i < gop->nb_streams; ++i ) {
      switch ( gop->streams[i]->codecpar->codec_type ) {
        case AVMEDIA_TYPE_VIDEO :
          gop->has_video = true;
        break;
        default :
          gop->has_audio = true;
        break;
      }
    }
  }
}

static enum AVMediaType ffgop_get_stream_type(struct ffgop * gop, uint rpos)
{
  int stidx = -1;
  switch ( gop->type ) {
    case ffgop_pkt :
      stidx = gop->pkts[rpos].stream_index;
    break;

    case ffgop_frm :
      stidx = (int) (ssize_t) (gop->frms[rpos]->opaque);
    break;

    default:
      PDBG("BUG: stidx=%d", stidx);
      exit(1);
      break;
  }

  return gop->streams[stidx]->codecpar->codec_type;
}


static void ffgl_reset(struct ffgoplistener * gl)
{
  gl->rpos = 0;
  gl->gopidx = gl->gop->idx;
  gl->skip_video = false;
}

static void ffgop_update_read_pos(struct ffgoplistener * gl, uint * gsize)
{
  struct ffgop * gop = gl->gop;

  gl->dp = UINT_MAX;


  if ( gl->gopidx == gop->idx ) {
    gl->dp = gop->wpos - gl->rpos;
    *gsize = gop->wpos;
  }
  else if ( (gop->idx > gl->gopidx + 1) || !(gop->wpos <= gl->rpos && gl->rpos < gop->oldsize) ) {
    ffgl_reset(gl), *gsize = gop->wpos;
  }
  else { // try to get rest of previous gop
    *gsize = gop->oldsize;
  }


  if ( !gl->skip_video && gl->enable_skip_video && gop->has_video && gop->has_audio && gl->rpos > 0 && gl->rpos + 5 < *gsize ) {
    // fixme: this skip_video threshold is incorrect
    gl->skip_video = true;
  }

  if ( gl->skip_video ) {

    while ( gl->rpos < *gsize && ffgop_get_stream_type(gl->gop, gl->rpos) == AVMEDIA_TYPE_VIDEO ) {
      ++gl->rpos;
    }

    if ( gl->rpos == *gsize && gl->gopidx != gop->idx ) {
      ffgl_reset(gl), *gsize = gop->wpos;
    }
  }

}


void ffgop_set_streams(struct ffgop * gop, const ffstream ** streams, uint nb_streams)
{
  w_lock(gop);
  gop->nb_streams = nb_streams;
  gop->streams = streams;
  ffgop_check_stream_types(gop);
  w_unlock(gop);
}

int ffgop_init(struct ffgop * gop, uint capacity, enum ffgoptype type, const ffstream ** streams, uint nb_streams)
{
  int status = 0;

  memset(gop, 0, sizeof(*gop));

  pthread_rwlock_init(&gop->rwlock, NULL);

  gop->streams = streams;
  gop->nb_streams = nb_streams;
  ffgop_check_stream_types(gop);

  if ( !(gop->gopev = coevent_create()) ) {
    status = AVERROR(errno);
    goto end;
  }

  switch ( gop->type = type ) {

    case ffgop_pkt :
      gop->wait_key = true;
      if ( !(gop->pkts = av_mallocz((gop->capacity = capacity) * sizeof(AVPacket))) ) {
        status = AVERROR(errno=ENOMEM);
        goto end;
      }
      for ( uint i = 0; i < capacity; ++i ) {
        av_init_packet(&gop->pkts[i]);
      }
    break;

    case ffgop_frm :
      if ( !(gop->frms = av_mallocz((gop->capacity = capacity) * sizeof(AVFrame*))) ) {
        status = AVERROR(errno=ENOMEM);
        goto end;
      }
      for ( uint i = 0; i < capacity; ++i ) {
        if ( !(gop->frms[i] = av_frame_alloc()) ) {
          status = AVERROR(errno=ENOMEM);
          goto end;
        }
      }
    break;

    default :
      status = AVERROR(errno=EINVAL);
      goto end;
    break;
  }

end:

  if ( status ) {

    switch ( gop->type ) {

      case ffgop_pkt :
        av_free(gop->pkts);
      break;

      case ffgop_frm:
        if ( gop->frms ) {
          for ( uint i = 0; i < capacity && gop->frms[i] != NULL; ++i ) {
            av_frame_free(&gop->frms[i]);
          }
          av_free(gop->frms);
        }
        break;

      default :
        break;
    }

    coevent_delete(&gop->gopev);
    pthread_rwlock_destroy(&gop->rwlock);

  }

  return status;
}


static void release_packets(struct ffgop * gop)
{
  switch ( gop->type ) {

    case ffgop_pkt :
      if ( gop->pkts ) {
        for ( uint i = 0; i < gop->capacity; ++i ) {
          av_packet_unref(&gop->pkts[i]);
        }
        av_free(gop->pkts), gop->pkts = NULL;
      }
    break;

    case ffgop_frm:
      if ( gop->frms ) {
        for ( uint i = 0; i < gop->capacity; ++i ) {
          if ( gop->frms[i] != NULL ) {
            av_frame_free(&gop->frms[i]);
          }
        }
        av_free(gop->frms), gop->frms = NULL;
      }
      break;

    default :
      break;
  }
}

void ffgop_cleanup(struct ffgop * gop)
{
  while ( gop->refs > 0 ) {
    gop->finish = true;
    co_sleep(20 * 1000);
  }

  release_packets(gop);
  coevent_delete(&gop->gopev);
  pthread_rwlock_destroy(&gop->rwlock);

}


struct ffgop * ffgop_create(uint capacity, enum ffgoptype type, const ffstream ** streams, uint nb_streams)
{
  struct ffgop * gop = NULL;
  int status = 0;

  if ( !(gop = malloc(sizeof(*gop))) ) {
    status = AVERROR(errno = ENOMEM);
  }
  else {
    status = ffgop_init(gop, capacity, type, streams, nb_streams);
  }

  if ( status && gop ) {
    ffgop_cleanup(gop);
    free(gop), gop = NULL;
  }

  return gop;
}

void ffgop_destroy(struct ffgop ** gop)
{
  if ( gop && *gop ) {
    ffgop_cleanup(*gop);
    free((*gop)), (*gop) = NULL;
  }
}

enum ffgoptype ffgop_get_type(const struct ffgop * gop)
{
  return gop->type;
}

void ffgop_put_eof(struct ffgop * gop, int reason)
{
  w_lock(gop);
  gop->eof_status = reason ? reason : AVERROR_EOF;
  release_packets(gop);
  coevent_set(gop->gopev);
  w_unlock(gop);
}



int ffgop_create_listener(struct ffgop * gop, struct ffgoplistener ** pgl)
{
  struct ffgoplistener * gl = NULL;
  int status = 0;

  * pgl = NULL;

  w_lock(gop);

  if ( gop->finish ) {
    status = AVERROR_EOF;
    goto end;
  }

  if ( !(gl = calloc(1, sizeof(*gl))) ) {
    goto end;
  }

  gl->gop = gop;

  if ( !(gl->w = coevent_add_waiter(gop->gopev)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  ++gop->refs;

end:

  if ( status && gl ) {
    coevent_remove_waiter(gop->gopev, gl->w);
    free(gl), gl = NULL;
  }

  w_unlock(gop);

  *pgl = gl;

  return status;
}


void ffgop_delete_listener(struct ffgoplistener ** gl)
{
  if ( gl && *gl ) {

    struct ffgop * gop = (*gl)->gop;

    w_lock(gop);
    coevent_remove_waiter(gop->gopev, (*gl)->w);
    --gop->refs;
    w_unlock(gop);

    free(*gl), gl = NULL;
  }
}

void ffgop_enable_skip_video(struct ffgoplistener * gl, bool enable)
{
  gl->enable_skip_video = enable;
}







int ffgop_put_pkt(struct ffgop * gop, AVPacket * pkt)
{
  int status = 0;

  w_lock(gop);

  if ( gop->eof_status ) {
    status = gop->eof_status;
  }
  else if ( gop->type != ffgop_pkt ) {
    status = AVERROR(EPROTO);
  }
  else if ( !gop->streams ) {
    status = AVERROR(EPROTO);
  }
  else if ( pkt->stream_index < 0 || pkt->stream_index >= (int) gop->nb_streams ) {
    status = AVERROR(EINVAL);
  }
  else {

    const enum AVMediaType media_type = gop->streams[pkt->stream_index]->codecpar->codec_type;
    const bool is_key_frame = (media_type == AVMEDIA_TYPE_VIDEO) && (pkt->flags & AV_PKT_FLAG_KEY);

    if ( is_key_frame || gop->wpos >= gop->capacity ) {
      gop->wait_key = !is_key_frame;
      gop->oldsize = gop->wpos;
      gop->wpos = 0;
      ++gop->idx;
    }

    if ( !gop->wait_key || media_type != AVMEDIA_TYPE_VIDEO ) {

      av_packet_unref(&gop->pkts[gop->wpos]);
      av_packet_ref(&gop->pkts[gop->wpos++], pkt);

      ffgop_set_event(gop);
    }
  }

  w_unlock(gop);

  if ( status == 0 ) {
    co_yield();
  }

  return status;
}


int ffgop_get_pkt(struct ffgoplistener * gl, AVPacket * pkt)
{
  uint gsize;

  int status = 0;

  r_lock(gl->gop);

  if ( gl->gop->eof_status ) {
    status = gl->gop->eof_status;
  }
  else if ( gl->gop->type != ffgop_pkt ) {
    PDBG("invalid request: goptype=%d", gl->gop->type);
    status = AVERROR(EINVAL);
  }
  else {

    while ( !(status = gl->gop->eof_status) ) {

      if ( gl->finish ) {
        status = AVERROR_EXIT;
        break;
      }

      if ( gl->gop->finish ) {
        status = AVERROR_EOF;
        break;
      }

      ffgop_update_read_pos(gl, &gsize);

      if ( gl->rpos < gsize ) {
        av_packet_ref(pkt, &gl->gop->pkts[gl->rpos++]);
        break;
      }

      r_unlock(gl->gop);
      coevent_wait(gl->w, -1);
      r_lock(gl->gop);
    }
  }

  r_unlock(gl->gop);

  return status;
}


int ffgop_put_frm(struct ffgop * gop, AVFrame * frm)
{
  int status = 0;
  int stidx;

  w_lock(gop);

  if ( gop->eof_status ) {
    status = gop->eof_status;
  }
  else if ( gop->type != ffgop_frm ) {
    status = AVERROR(EINVAL);
  }
  else if ( !gop->streams ) {
    status = AVERROR(EPROTO);
  }
  else if ( (stidx = (int) (ssize_t) frm->opaque) < 0 || stidx >= (int) gop->nb_streams ) {
    status = AVERROR(EINVAL);
  }
  else {

    if ( gop->wpos >= gop->capacity ) {
      gop->oldsize = gop->wpos;
      gop->wpos = 0;
      ++gop->idx;
    }

    av_frame_unref(gop->frms[gop->wpos]);
    av_frame_ref(gop->frms[gop->wpos], frm);

    ++gop->wpos;

    ffgop_set_event(gop);
  }

  w_unlock(gop);

  if ( status == 0 ) {
    co_yield();
  }

  return status;
}

int ffgop_get_frm(struct ffgoplistener * gl, AVFrame * frm)
{
  uint gsize;
  int status = 0;

  r_lock(gl->gop);

  if ( gl->gop->eof_status ) {
    status = gl->gop->eof_status;
  }
  else if ( gl->gop->type != ffgop_frm ) {
    PDBG("invalid request: goptype=%d", gl->gop->type);
    status = AVERROR(EINVAL);
  }
  else {

    while ( !(status = gl->gop->eof_status) ) {

      if ( gl->finish ) {
        status = AVERROR_EXIT;
        break;
      }

      if ( gl->gop->finish ) {
        status = AVERROR_EOF;
        break;
      }

      ffgop_update_read_pos(gl, &gsize);

      if ( gl->rpos < gsize ) {
        av_frame_ref(frm, gl->gop->frms[gl->rpos++]);
        break;
      }

      r_unlock(gl->gop);
      coevent_wait(gl->w, -1);
      r_lock(gl->gop);
    }
  }

  r_unlock(gl->gop);

  return status;
}


bool ffgop_is_waiting_key(struct ffgop * gop)
{
  return gop->wait_key;
}
