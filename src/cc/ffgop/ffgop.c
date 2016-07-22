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


#define RATE_CTL_NW 64
#define RATE_CTL_NR 64
#define RATE_CTL_K  0.5

#define RATE_E_MIN_DT (10*1000)

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

static inline void ffgop_ctrl_set(struct ffgop * gop, uint ctrl_bits, bool set)
{
  if ( set ) {
    gop->ctrl |= ctrl_bits;
  }
  else {
    gop->ctrl &= ~ctrl_bits;
  }
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


static inline enum AVMediaType ffgop_get_stream_type(struct ffgop * gop, int sidx)
{
  return gop->st[sidx].s->codecpar->codec_type;
}


//static inline bool ffgop_has_audio_and_video(struct ffgoplistener * gl)
//{
//  const uint flags = ffgop_has_audio | ffgop_has_video;
//  return ((gl->gop->ctrl & flags) == flags);
//}

static inline bool has_audio(struct ffgoplistener * gl)
{
  const uint flags = ffgop_has_audio;
  return ((gl->gop->ctrl & flags) == flags);
}

static int ffgop_update_read_pos(struct ffgoplistener * gl)
{
  struct ffgop * gop = gl->gop;
  int si = -1;

  int gsize[gop->nb_streams];
  int delta[gop->nb_streams];

  for ( int i = 0; i < (int)gop->nb_streams; ++i ) {

    if ( gl->gidx[i] == gop->st[i].idx ) {
      gsize[i] = gop->st[i].wpos;
      delta[i] = gop->st[i].wpos - gl->rpos[i];
    }
    else if ( gop->st[i].idx > gl->gidx[i] + 1 ) {
      delta[i] = gop->st[i].wpos - gl->rpos[i] + gop->gcap * (gop->st[i].idx - gl->gidx[i]);
      gl->gidx[i] = gl->gop->st[i].idx;
      gl->rpos[i] = 0;
      gsize[i] = gop->st[i].wpos;
    }
    else if ( !(gop->st[i].wpos <= gl->rpos[i] && gl->rpos[i] < gop->st[i].oldsize) ) {
      delta[i] = gop->st[i].oldsize - gl->rpos[i] + gop->st[i].wpos;
      gl->rpos[i] = 0;
      gl->gidx[i] = gl->gop->st[i].idx;
      gsize[i] = gop->st[i].wpos;

    }
    else {  // get rest of previous gop
      delta[i] = gop->st[i].oldsize - gl->rpos[i] + gop->st[i].wpos;
      gsize[i] = gop->st[i].oldsize;
    }

    if ( gl->rpos[i] < gsize[i] ) {

      if ( si < 0 ) {
        si = i;
      }
      else {
        switch ( ffgop_get_stream_type(gop, i) ) {
          case AVMEDIA_TYPE_AUDIO :
            if ( ffgop_get_stream_type(gop, si) != AVMEDIA_TYPE_AUDIO || delta[i] > delta[si] ) {
              si = i;
            }
          break;

          default :
            if ( ffgop_get_stream_type(gop, si) != AVMEDIA_TYPE_AUDIO && delta[i] > delta[si] ) {
              si = i;
            }
          break;
        }
      }
    }
  }

  return si;
}


static void ffgop_free_streams(struct ffgop * gop)
{
  if ( gop->st ) {

    for ( uint idx = 0; idx < gop->nb_streams; ++idx ) {

      switch ( gop->gtype ) {

        case ffgop_pkt :
          if ( gop->st[idx].pkts ) {
            for ( uint i = 0; i < gop->gcap; ++i ) {
              av_packet_unref(&gop->st[idx].pkts[i]);
            }
            av_free(gop->st[idx].pkts);
          }
        break;

        case ffgop_frm :
          if ( gop->st[idx].frms ) {
            for ( uint i = 0; i < gop->gcap; ++i ) {
              if ( gop->st[idx].frms[i] != NULL ) {
                av_frame_free(&gop->st[idx].frms[i]);
              }
            }
            av_free(gop->st[idx].frms);
          }
        break;

        default :
          break;
      }
    }
    av_freep(&gop->st);
  }
  gop->nb_streams = 0;
}

int ffgop_set_streams(struct ffgop * gop, const ffstream ** streams, uint nb_streams)
{
  int status = 0;

  w_lock(gop);

  ffgop_free_streams(gop);

  if ( (gop->nb_streams = nb_streams) > 0 ) {

    if ( !(gop->st = av_mallocz(nb_streams * sizeof(*gop->st))) ) {
      status = AVERROR(ENOMEM);
      goto end;
    }

    for ( uint idx = 0; idx < nb_streams; ++idx ) {

      switch ( (gop->st[idx].s = streams[idx])->codecpar->codec_type ) {
        case AVMEDIA_TYPE_VIDEO :
          ffgop_ctrl_set(gop, ffgop_has_video, true);
        break;
        default :
          ffgop_ctrl_set(gop, ffgop_has_audio, true);
        break;
      }

      switch ( gop->gtype ) {
        case ffgop_pkt :
          if ( !(gop->st[idx].pkts = av_mallocz(gop->gcap * sizeof(AVPacket))) ) {
            status = AVERROR(errno=ENOMEM);
            goto end;
          }
          for ( uint i = 0; i < gop->gcap; ++i ) {
            av_init_packet(&gop->st[idx].pkts[i]);
          }
        break;

        case ffgop_frm :
          if ( !(gop->st[idx].frms = av_mallocz(gop->gcap * sizeof(AVFrame*))) ) {
            status = AVERROR(errno=ENOMEM);
            goto end;
          }
          for ( uint i = 0; i < gop->gcap; ++i ) {
            if ( !(gop->st[idx].frms[i] = av_frame_alloc()) ) {
              status = AVERROR(errno=ENOMEM);
              goto end;
            }
          }
        break;

        default :
          PDBG("BUG: Invalid gop type = %d", gop->gtype);
          status = AVERROR(errno=EINVAL);
          goto end;
      }
    }
  }

end:

  if ( status != 0 ) {
    ffgop_free_streams(gop);
  }

  w_unlock(gop);

  return status;
}

int ffgop_init(struct ffgop * gop, const struct ffgop_init_args * args)
{
  int status = 0;

  memset(gop, 0, sizeof(*gop));

  pthread_rwlock_init(&gop->rwlock, NULL);

  if ( !(gop->gopev = coevent_create()) ) {
    status = AVERROR(errno);
    goto end;
  }

  if ( (gop->gtype = args->type) != ffgop_pkt && args->type != ffgop_frm ) {
    status = AVERROR(errno=EINVAL);
    goto end;
  }

  gop->gcap = args->capacity ? args->capacity : 512;

  if ( args->nb_streams ) {
    status = ffgop_set_streams(gop, args->streams, args->nb_streams);
  }

end:

  if ( status ) {
    coevent_delete(&gop->gopev);
    pthread_rwlock_destroy(&gop->rwlock);
  }

  return status;
}



void ffgop_cleanup(struct ffgop * gop)
{
  while ( gop->refs > 0 ) {
    ffgop_ctrl_set(gop, ffgop_ctrl_finish, true);
    co_sleep(20 * 1000);
  }

  ffgop_free_streams(gop);
  coevent_delete(&gop->gopev);
  pthread_rwlock_destroy(&gop->rwlock);

}


struct ffgop * ffgop_create(const struct ffgop_init_args * args)
{
  struct ffgop * gop = NULL;
  int status = 0;

  if ( !(gop = malloc(sizeof(*gop))) ) {
    status = AVERROR(errno = ENOMEM);
  }
  else if ( (status = ffgop_init(gop, args)) ) {
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
  return gop->gtype;
}

void ffgop_put_eof(struct ffgop * gop, int reason)
{
  w_lock(gop);
  gop->eof_status = reason ? reason : AVERROR_EOF;
  ffgop_free_streams(gop);
  coevent_set(gop->gopev);
  w_unlock(gop);
}



int ffgop_create_listener(struct ffgop * gop, struct ffgoplistener ** pgl,
    const struct ffgop_create_listener_args * args)
{
  struct ffgoplistener * gl = NULL;
  int status = 0;

  * pgl = NULL;

  w_lock(gop);

  if ( gop->ctrl & ffgop_ctrl_finish ) {
    status = AVERROR_EOF;
    goto end;
  }

  if ( !(gl = calloc(1, sizeof(*gl))) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(gl->gidx = calloc(gop->nb_streams, sizeof(*gl->gidx))) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(gl->rpos = calloc(gop->nb_streams, sizeof(*gl->rpos))) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }


  gl->gop = gop;

  if( args ) {
    gl->getoutspc = args->getoutspc;
    gl->cookie = args->cookie;
  }

  if ( !(gl->w = coevent_add_waiter(gop->gopev)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }


  ++gop->refs;

end:

  if ( status && gl ) {
    coevent_remove_waiter(gop->gopev, gl->w);
    free(gl->gidx);
    free(gl->rpos);
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

    free((*gl)->gidx);
    free((*gl)->rpos);
    free(*gl);
    *gl = NULL;
  }
}


int ffgop_put_pkt(struct ffgop * gop, AVPacket * pkt)
{
  int status = 0;
  int sidx;

  w_lock(gop);

  if ( gop->eof_status ) {
    status = gop->eof_status;
  }
  else if ( gop->gtype != ffgop_pkt ) {
    status = AVERROR(EPROTO);
  }
  else if ( !gop->nb_streams ) {
    status = AVERROR(EPROTO);
  }
  else if ( (sidx = pkt->stream_index) < 0 || pkt->stream_index >= (int) gop->nb_streams ) {
    status = AVERROR(EINVAL);
  }
  else {

    const enum AVMediaType media_type = ffgop_get_stream_type(gop, sidx);
    const bool is_key_frame = (media_type == AVMEDIA_TYPE_VIDEO) && (pkt->flags & AV_PKT_FLAG_KEY);

    if ( is_key_frame || (gop->st[sidx].wpos >= (int)gop->gcap && media_type != AVMEDIA_TYPE_VIDEO) ) {
      gop->st[sidx].oldsize = gop->st[sidx].wpos;
      gop->st[sidx].wpos = 0;
      ++gop->st[sidx].idx;
    }

    if ( gop->st[sidx].wpos < (int)gop->gcap ) {
      av_packet_unref(&gop->st[sidx].pkts[gop->st[sidx].wpos]);
      av_packet_ref(&gop->st[sidx].pkts[gop->st[sidx].wpos++], pkt);
      ffgop_set_event(gop);
    }
  }

  w_unlock(gop);

  if ( status == 0 ) {
    co_yield();
  }

  return status;
}

int ffgop_put_frm(struct ffgop * gop, AVFrame * frm)
{
  int status = 0;
  int sidx;

  w_lock(gop);

  if ( gop->eof_status ) {
    status = gop->eof_status;
  }
  else if ( gop->gtype != ffgop_frm ) {
    status = AVERROR(EINVAL);
  }
  else if ( !gop->nb_streams ) {
    status = AVERROR(EPROTO);
  }
  else if ( (sidx = (int) (ssize_t) frm->opaque) < 0 || sidx >= (int) gop->nb_streams ) {
    status = AVERROR(EINVAL);
  }
  else {

    if ( gop->st[sidx].wpos >= (int)gop->gcap ) {
      gop->st[sidx].oldsize = gop->st[sidx].wpos;
      gop->st[sidx].wpos = 0;
      ++gop->st[sidx].idx;
    }

    av_frame_unref(gop->st[sidx].frms[gop->st[sidx].wpos]);
    av_frame_ref(gop->st[sidx].frms[gop->st[sidx].wpos], frm);

    ++gop->st[sidx].wpos;

    ffgop_set_event(gop);
  }

  w_unlock(gop);

  if ( status == 0 ) {
    co_yield();
  }

  return status;
}


static inline bool ffgop_getoutspc(struct ffgoplistener * gl, int * outspc, int * maxspc)
{
  return gl->getoutspc && gl->getoutspc(gl->cookie, outspc, maxspc);
}

int ffgop_get_pkt(struct ffgoplistener * gl, AVPacket * pkt)
{
  struct ffgop * gop;
  int sidx = -1;

  int status = 0;


  gop = gl->gop;

  r_lock(gop);

  if ( gop->eof_status ) {
    status = gop->eof_status;
  }
  else if ( gop->gtype != ffgop_pkt ) {
    PDBG("invalid request: goptype=%d", gop->gtype);
    status = AVERROR(EINVAL);
  }
  else {

    while ( !(status = gop->eof_status) ) {

      if ( gl->finish ) {
        status = AVERROR_EXIT;
        break;
      }

      if ( gop->ctrl & ffgop_ctrl_finish ) {
        status = AVERROR_EOF;
        break;
      }

      if ( (sidx = ffgop_update_read_pos(gl)) >= 0 ) {

        int outspc = 0, maxspc = 0;

        if ( has_audio(gl) && ffgop_get_stream_type(gop, sidx) == AVMEDIA_TYPE_VIDEO ) {
          if ( ffgop_getoutspc(gl, &outspc, &maxspc) && outspc < maxspc / 2 ) {
            if ( gl->rpos[sidx] > 0 ) {
              r_unlock(gop);
              coevent_wait(gl->w, 20);
              r_lock(gop);
              continue;
            }
          }
        }

        av_packet_ref(pkt, &gop->st[sidx].pkts[gl->rpos[sidx]++]);

        break;
      }

      r_unlock(gop);
      coevent_wait(gl->w, -1);
      r_lock(gop);
    }
  }

  r_unlock(gop);

  return status;
}



int ffgop_get_frm(struct ffgoplistener * gl, AVFrame * frm)
{
  struct ffgop * gop = gl->gop;
  int sidx;
  int status = 0;


  r_lock(gop);

  if ( gop->eof_status ) {
    status = gop->eof_status;
  }
  else if ( gop->gtype != ffgop_frm ) {
    PDBG("invalid request: goptype=%d", gop->gtype);
    status = AVERROR(EINVAL);
  }
  else {

    while ( !(status = gop->eof_status) ) {

      if ( gl->finish ) {
        status = AVERROR_EXIT;
        break;
      }

      if ( gop->ctrl & ffgop_ctrl_finish ) {
        status = AVERROR_EOF;
        break;
      }

      if ( (sidx = ffgop_update_read_pos(gl)) >= 0 ) {
        av_frame_ref(frm, gop->st[sidx].frms[gl->rpos[sidx]++]);
        break;
      }

      r_unlock(gop);
      coevent_wait(gl->w, -1);
      r_lock(gop);
    }
  }

  r_unlock(gop);

  return status;
}
