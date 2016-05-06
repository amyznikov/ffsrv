/*
 * ffgop.c
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#include "ffobj.h"
#include "coscheduler.h"
#include <malloc.h>
#include <pthread.h>
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


int ffgop_init(struct ffgop * gop, uint gopsize, enum ffgoptype type)
{
  int status = 0;

  memset(gop, 0, sizeof(*gop));

  pthread_rwlock_init(&gop->rwlock, NULL);

  if ( !(gop->gopev = coevent_create()) ) {
    status = AVERROR(errno);
    goto end;
  }

  switch ( gop->goptype = type ) {

    case ffgop_pkt :
      if ( !(gop->pkts = av_mallocz((gop->gopsize = gopsize) * sizeof(AVPacket))) ) {
        status = AVERROR(errno=ENOMEM);
        goto end;
      }
      for ( uint i = 0; i < gop->gopsize; ++i ) {
        av_init_packet(&gop->pkts[i]);
      }
    break;

    case ffgop_frm :
      if ( !(gop->frms = av_mallocz((gop->gopsize = gopsize) * sizeof(AVFrame*))) ) {
        status = AVERROR(errno=ENOMEM);
        goto end;
      }
      for ( uint i = 0; i < gop->gopsize; ++i ) {
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

    switch ( gop->goptype ) {

      case ffgop_pkt :
        av_free(gop->pkts);
      break;

      case ffgop_frm:
        if ( gop->frms ) {
          for ( uint i = 0; i < gop->gopsize && gop->frms[i] != NULL; ++i ) {
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
  switch ( gop->goptype ) {

    case ffgop_pkt :
      if ( gop->pkts ) {
        for ( uint i = 0; i < gop->gopsize; ++i ) {
          ff_avpacket_unref(&gop->pkts[i]);
        }
        av_free(gop->pkts), gop->pkts = NULL;
      }
    break;

    case ffgop_frm:
      if ( gop->frms ) {
        for ( uint i = 0; i < gop->gopsize; ++i ) {
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


struct ffgop * ffgop_create(uint gopsize, enum ffgoptype type)
{
  struct ffgop * gop = NULL;
  int status = 0;

  if ( !(gop = malloc(sizeof(*gop))) ) {
    status = AVERROR(errno = ENOMEM);
  }
  else {
    status = ffgop_init(gop, gopsize, type);
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
  return gop->goptype;
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





int ffgop_put_pkt(struct ffgop * gop, AVPacket * pkt, enum AVMediaType media_type)
{
  int status = 0;

  w_lock(gop);

  if ( gop->eof_status ) {
    status = gop->eof_status;
  }
  else if ( gop->goptype != ffgop_pkt ) {
    status = AVERROR(EINVAL);
  }
  else {

    bool is_key_frame = (media_type == AVMEDIA_TYPE_VIDEO) && (pkt->flags & AV_PKT_FLAG_KEY);


    if ( is_key_frame || gop->gopwpos >= gop->gopsize ) {
      gop->gopwpos = 0;
      ++gop->gopidx;
    }

    ff_avpacket_unref(&gop->pkts[gop->gopwpos]);
    ff_avpacket_ref(&gop->pkts[gop->gopwpos], pkt);
    ++gop->gopwpos;

    ffgop_set_event(gop);
  }

  w_unlock(gop);

  return status;
}


int ffgop_get_pkt(struct ffgoplistener * gl, AVPacket * pkt)
{
  int status = 0;

  r_lock(gl->gop);

  if ( gl->gop->eof_status ) {
    status = gl->gop->eof_status;
  }
  else if ( gl->gop->goptype != ffgop_pkt ) {
    PDBG("invalid request: goptype=%d", gl->gop->goptype);
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

      if ( gl->gopidx < gl->gop->gopidx ) {
        gl->goprpos = 0, gl->gopidx = gl->gop->gopidx;
      }

      if ( gl->goprpos < gl->gop->gopwpos ) {
        //ff_avpacket_ref(pkt, &gl->gop->pkts[gl->goprpos++]);
        av_copy_packet(pkt,&gl->gop->pkts[gl->goprpos++]);
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


int ffgop_put_frm(struct ffgop * gop, AVFrame * frm, enum AVMediaType media_type)
{
  int status = 0;

  w_lock(gop);

  if ( gop->eof_status ) {
    status = gop->eof_status;
  }
  else if ( gop->goptype != ffgop_frm ) {
    status = AVERROR(EINVAL);
  }
  else {

    bool is_key_frame = (media_type == AVMEDIA_TYPE_VIDEO) && (frm->key_frame);


    if ( is_key_frame || gop->gopwpos >= gop->gopsize ) {
      gop->gopwpos = 0;
      ++gop->gopidx;
    }

    ff_avframe_unref(gop->frms[gop->gopwpos]);
    ff_avframe_ref(gop->frms[gop->gopwpos], frm);

    ++gop->gopwpos;

    ffgop_set_event(gop);
  }

  w_unlock(gop);

  return status;
}

int ffgop_get_frm(struct ffgoplistener * gl, AVFrame * frm)
{
  int status = 0;

  r_lock(gl->gop);

  if ( gl->gop->eof_status ) {
    status = gl->gop->eof_status;
  }
  else if ( gl->gop->goptype != ffgop_frm ) {
    PDBG("invalid request: goptype=%d", gl->gop->goptype);
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

      if ( gl->gopidx < gl->gop->gopidx ) {
        gl->goprpos = 0, gl->gopidx = gl->gop->gopidx;
      }

      if ( gl->goprpos < gl->gop->gopwpos ) {
        //ff_avframe_ref(frm, gl->gop->frms[gl->goprpos++]);
        ffmpeg_copy_frame(frm, gl->gop->frms[gl->goprpos++]);
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

