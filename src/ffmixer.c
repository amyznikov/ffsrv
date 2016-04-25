/*
 * ffinput.c
 *
 *  Created on: Mar 21, 2016
 *      Author: amyznikov
 *
 *  See http://www.fsl.cs.sunysb.edu/~vass/linux-aio.txt
 */

#include <stdarg.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include "sockopt.h"
#include "ccarray.h"
#include "ffmpeg.h"
#include "ffgop.h"
#include "libffms.h"
#include "debug.h"

#if 0
#define INPUT_IO_BUF_SIZE         (4*1024)
#define INPUT_THREAD_STACK_SIZE   (256*1024)

#define TCP_OUTPUT_IO_BUF_SIZE    (32*1024)
#define RTP_OUTPUT_IO_BUF_SIZE    1472



#define TIME_BASE_USEC() \
  (AVRational){1,1000000}


struct ffmixer {
  struct ffms_input_params p;
  struct ff_start_input_args args;

  //struct coevent * evt;

  AVFormatContext * ic;
  struct ffgop * gop;

  pthread_rwlock_t rwlock;
//  AVPacket * gop; // gop array
//  size_t gopsize; // capacity
//  size_t gopidx;  // gop index
//  size_t wpos;   // gop write pos

  enum ff_input_state state;
  int refs;
};



static ccarray_t g_inputs; // <struct ffinput *>
//static pthread_mutex_t g_inputs_lock = PTHREAD_MUTEX_INITIALIZER;


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//static void lock_inputs(void)
//{
//  pthread_mutex_lock(&g_inputs_lock);
//}
//
//static void unlock_inputs(void)
//{
//  pthread_mutex_unlock(&g_inputs_lock);
//}

static inline void w_lock(struct ffmixer * input)
{
  pthread_rwlock_wrlock(&input->rwlock);
}

static inline void w_unlock(struct ffmixer * input)
{
  pthread_rwlock_unlock(&input->rwlock);
}

static inline void r_lock(struct ffmixer * input)
{
  pthread_rwlock_rdlock(&input->rwlock);
}

static inline void r_unlock(struct ffmixer * input)
{
  pthread_rwlock_unlock(&input->rwlock);
}

//
//static void ff_broadcast_input_event(struct ffinput * input)
//{
//  coevent_set(input->evt);
//}










static void ff_input_thread(void * arg)
{
  struct ffmixer * input = arg;

  AVPacket pkt;
  AVStream * is;
  int stream_index;

  AVIOContext * pb = NULL;

  enum {
    pb_type_none = 0,
    pb_type_callback = 1,
    pb_type_popen = 2,
  } pb_type = pb_type_none;



  // rate emulation
  int64_t pts, now, ts0 = 0, * dts, * dts0, *ppts;

  // pts correction
  bool * wrap_correction_done;
  int64_t ts_offset;

  int64_t idle_time;

  int status;


  ////////////////////////////////////////////////////////////////////

//  input->wpos = 0;
//  input->gopidx = 0;
//
//
//  if ( !(input->gop = calloc(input->gopsize = 2 * 1024, sizeof(AVPacket))) ) {
//    PDBG("calloc(GOP) fails");
//    status = AVERROR(ENOMEM);
//    goto end;
//  }
//
//  for ( size_t i = 0; i < input->gopsize; ++i ) {
//    av_init_packet(&input->gop[i]);
//  }

  ////////////////////////////////////////////////////////////////////

  if ( input->p.source && strncasecmp(input->p.source, "popen://", 8) == 0 ) {

    if ( (status = ff_create_popen_input_context(&pb, input->p.source + 8)) ) {
      PDBG("ff_create_popen_input_context() fails: %s", av_err2str(status));
      goto end;
    }

    pb_type = pb_type_popen;
  }
  else if ( input->args.onrecvpkt ) {

    uint8_t * iobuf = av_malloc(INPUT_IO_BUF_SIZE);
    if ( !iobuf ) {
      PDBG("av_malloc(iobuf) fails");
      status = AVERROR(ENOMEM);
      goto end;
    }

    pb = avio_alloc_context(iobuf, INPUT_IO_BUF_SIZE, false, input->args.cookie, input->args.onrecvpkt, NULL, NULL);
    if ( !pb ) {
      av_free(iobuf);
      status = AVERROR(ENOMEM);
      goto end;
    }

    pb_type = pb_type_callback;
  }

  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] ffmpeg_open_input()", input->p.name);
  if ( (status = ffmpeg_open_input(&input->ic, input->p.source, input->p.format, pb, NULL, input->p.ctxopts)) ) {
    PDBG("[%s] ffmpeg_open_input() fails", input->p.name);
    goto end;
  }

  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] C ffmpeg_probe_input()", input->p.name);
  if ( (status = ffmpeg_probe_input(input->ic, 1)) < 0 ) {
    PDBG("[%s] ffmpeg_probe_input() fails", input->p.name);
    goto end;
  }
  PDBG("[%s] R ffmpeg_probe_input()", input->p.name);

  if ( input->p.genpts ) {
    for ( uint i = 0; i < input->ic->nb_streams; ++i ) {
      AVStream * st = input->ic->streams[i];
      st->time_base = TIME_BASE_USEC();
      if ( st->codec ) {
        st->codec->time_base = TIME_BASE_USEC();
      }
    }
  }


  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] ESTABLISHED", input->p.name);

  input->state = ff_input_state_established;
  //ff_broadcast_input_event(input);
  ffgop_set_event(input->gop);

  av_init_packet(&pkt), pkt.data = NULL, pkt.size = 0;


  dts = alloca(sizeof(*dts) * input->ic->nb_streams);
  dts0= alloca(sizeof(*dts0) * input->ic->nb_streams);
  ppts= alloca(sizeof(*dts0) * input->ic->nb_streams);
  wrap_correction_done = alloca(sizeof(*wrap_correction_done) * input->ic->nb_streams);
  for ( uint i = 0; i < input->ic->nb_streams; ++i ) {
    dts[i] = AV_NOPTS_VALUE;
    dts0[i] = AV_NOPTS_VALUE;
    ppts[i] = AV_NOPTS_VALUE;
    wrap_correction_done[i] = false;
  }

  if ( input->p.re > 0 ) {
    ts0 = ffmpeg_gettime();
  }

  ts_offset = input->ic->start_time == AV_NOPTS_VALUE ? 0 : -input->ic->start_time;
  if ( ts_offset == -input->ic->start_time && (input->ic->iformat->flags & AVFMT_TS_DISCONT) ) {
    int64_t new_start_time = INT64_MAX;
    for ( uint i = 0; i < input->ic->nb_streams; i++ ) {
      is = input->ic->streams[i];
      if ( is->discard == AVDISCARD_ALL || is->start_time == AV_NOPTS_VALUE ) {
        continue;
      }
      new_start_time = FFMIN(new_start_time, av_rescale_q(is->start_time, is->time_base, AV_TIME_BASE_Q));
    }
    if ( new_start_time > input->ic->start_time ) {
      PDBG("Correcting start time by %"PRId64"\n", new_start_time - input->ic->start_time);
      ts_offset = -new_start_time;
    }
  }



  idle_time = 0;

  while ( 42 ) {

    bool process_packet = true;

    if ( input->refs > 0 ) {
      idle_time = 0;
    }
    else if ( input->p.idle_timeout > 0 ) {
      if ( !idle_time ) {
        idle_time = ffmpeg_gettime() + input->p.idle_timeout * (int64_t) 1000000;
      }
      else if ( ffmpeg_gettime() >= idle_time ) {
        PDBG("[%s] EXIT BY IDLE TIMEOUT", input->p.name);
        status = AVERROR(ECHILD);
        break;
      }
    }


    while ( (status = av_read_frame(input->ic, &pkt)) == AVERROR(EAGAIN) ) {
      PDBG("[%s] EAGAIN: status = %d", input->p.name, status);
      ff_usleep(10 * 1000);
    }


    if ( status ) {
      PDBG("[%s] BREAK: %s", input->p.name, av_err2str(status));
      av_packet_unref(&pkt);
      break;
    }


    // do some pts processing here
    // ...
    stream_index = pkt.stream_index;
    is = input->ic->streams[stream_index];

    // PDBG("PKT st=%d pts=%"PRId64" dts=%"PRId64"", stream_index, pkt.pts, pkt.dts);

    if ( !wrap_correction_done[stream_index] && is->start_time != AV_NOPTS_VALUE && is->pts_wrap_bits < 64 ) {

      int64_t stime, stime2;

      stime = av_rescale_q(is->start_time, AV_TIME_BASE_Q, is->time_base);
      stime2 = stime + (1ULL << is->pts_wrap_bits);
      wrap_correction_done[stream_index] = true;

      if ( stime2 > stime && pkt.dts != AV_NOPTS_VALUE && pkt.dts > stime + (1LL << (is->pts_wrap_bits - 1)) ) {
        pkt.dts -= 1ULL << is->pts_wrap_bits;
        wrap_correction_done[stream_index] = false;
      }

      if ( stime2 > stime && pkt.pts != AV_NOPTS_VALUE && pkt.pts > stime + (1LL << (is->pts_wrap_bits - 1)) ) {
        pkt.pts -= 1ULL << is->pts_wrap_bits;
        wrap_correction_done[stream_index] = false;
      }
    }

    if ( pkt.dts != AV_NOPTS_VALUE ) {
      pkt.dts += av_rescale_q(ts_offset, AV_TIME_BASE_Q, is->time_base);
    }

    if ( pkt.pts != AV_NOPTS_VALUE ) {
      pkt.pts += av_rescale_q(ts_offset, AV_TIME_BASE_Q, is->time_base);
    }

    if ( input->p.genpts ) {
      pkt.pts = pkt.dts = ffmpeg_gettime();
      if ( pkt.pts <= ppts[stream_index] ) {
        pkt.pts = pkt.dts = ppts[stream_index] + 1;
      }
    }
    else if ( input->p.re > 0 && stream_index == input->p.re - 1
        && (pkt.dts != AV_NOPTS_VALUE || pkt.pts != AV_NOPTS_VALUE) ) {

      int64_t ts = pkt.dts != AV_NOPTS_VALUE ? pkt.dts : pkt.pts;

      dts[stream_index] = av_rescale_q_rnd(ts, is->time_base, TIME_BASE_USEC(),
          AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

      if ( dts0[stream_index] == AV_NOPTS_VALUE ) {
        dts0[stream_index] = dts[stream_index];
      }

      if ( (now = ffmpeg_gettime() - ts0) < (pts = dts[stream_index] - dts0[stream_index]) ) {
        // PDBG("sleep %"PRId64"", pts - now);
        ff_usleep(pts - now);
      }
    }

    if ( ppts[stream_index] == AV_NOPTS_VALUE ) {
      if ( pkt.pts != AV_NOPTS_VALUE ) {
        ppts[stream_index] = pkt.pts;
      }
    }
    else if ( pkt.pts != AV_NOPTS_VALUE && pkt.pts <= ppts[stream_index] ) {
      PDBG("[%s][%d] WAIT REORDERING", input->p.name, stream_index);
      process_packet = false;
    }


    // broadcat packet
    if ( process_packet ) {
      ff_write_gop(input, &pkt);
    }

    av_packet_unref(&pkt);
  }

  ////////////////////////////////////////////////////////////////////


end: ;

  w_lock(input);
  input->state = ff_input_state_disconnecting;

  if ( input->ic ) {
    PDBG("C ffmpeg_close_input(): AVFMT_FLAG_CUSTOM_IO == %d input->ic->pb=%p",
        (input->ic->flags & AVFMT_FLAG_CUSTOM_IO), input->ic->pb);
    ffmpeg_close_input(&input->ic);
    PDBG("R ffmpeg_close_input()");
  }

  if ( pb ) {
    switch ( pb_type ) {
      case pb_type_callback :
        av_free(pb->buffer);
        av_freep(&pb);
      break;
      case pb_type_popen :
        ff_close_popen_input_context(&pb);
      break;
      default :
        PDBG("BUG: pb_type = %d", pb_type);
        exit(1);
      break;
    }
  }

//  input->wpos = input->gopidx = 0;
//  if ( input->gop ) {
//    for ( size_t i = 0; i < input->gopsize; ++i ) {
//      av_packet_unref(&input->gop[i]);
//    }
//  }

  if ( input->args.onfinish ) {
    input->args.onfinish(input->args.cookie);
  }

  input->state = ff_input_state_idle;
  w_unlock(input);

  //ff_broadcast_input_event(input);
  //ffgop_set_event(input->gop);

  ffgop_destroy(&input->gop);



  PDBG("leave");
}





////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool ffinput_initialize(void)
{
  bool fok = false;

  av_register_all();
  avdevice_register_all();
  avformat_network_init();
  //av_log_set_level(AV_LOG_TRACE);

  if ( !ccarray_init(&g_inputs, 1024, sizeof(struct ffmixer*)) ) {
    goto end;
  }

  extern int (*ff_poll)(struct pollfd *__fds, nfds_t __nfds, int __timeout);
  ff_poll = co_poll;

  fok = true;

end: ;

  return fok;
}


int ff_create_input(struct ffmixer ** pp, struct ff_create_input_args args)
{
  struct ffmixer * input = NULL;
  bool fok = false;

  if ( ccarray_size(&g_inputs) >= ccarray_capacity(&g_inputs) ) {
    errno = ENOMEM;
    goto end;
  }

  if ( !(input = calloc(1, sizeof(struct ffmixer))) ) {
    goto end;
  }

//  if ( !(input->gop = ffgop_create(256, ffgop_pkt)) ) {
//    PDBG("ffgop_create() fails: %s", strerror(errno));
//    goto end;
//  }


//  pthread_rwlock_init(&input->rwlock, NULL);
//
//
//  if ( !(input->evt = coevent_create()) ) {
//    PDBG("co_create_event() fails");
//    goto end;
//  }

  input->p.name =  args.p.name ? strdup(args.p.name) : NULL;
  input->p.source = args.p.source ? strdup(args.p.source) : NULL;
  input->p.ctxopts = args.p.ctxopts ? strdup(args.p.ctxopts) : NULL;
  input->p.format = args.p.format ? strdup(args.p.format) : NULL;
  input->p.re = args.p.re;
  input->p.genpts = args.p.genpts;
  input->p.idle_timeout = args.p.idle_timeout;

  memset(&input->args, 0, sizeof(input->args));
  input->ic = NULL;
  input->refs = 0;
//  input->wpos = 0;
//  input->gopidx = 0;
  input->refs = 0;
  input->state = ff_input_state_idle;

  ccarray_ppush_back(&g_inputs, input);
  fok = true;

end: ;

  if ( !fok ) {
    if ( input ) {
//      ffgop_destroy(&input->gop);
//      coevent_delete(&input->evt);
//      pthread_rwlock_destroy(&input->rwlock);
      input = NULL;
    }
  }

  return input;
}


void ff_destroy_input(struct ffmixer * input)
{
//  if ( input ) {
//
//    free(input->gop);
//    coevent_delete(&input->evt);
//
//    free((void*) input->p.name);
//    free((void*) input->p.source);
//    free((void*) input->p.format);
//    free((void*) input->p.ctxopts);
//
//    pthread_rwlock_destroy(&input->rwlock);
//
//    for ( cclist_node * node = cclist_head(&g_inputs); node != NULL; node = node->next ) {
//      if ( input == cclist_peek(node) ) {
//        cclist_erase(&g_inputs, node);
//        break;
//      }
//    }
//  }
}


const char * ff_get_input_name(const struct ffmixer * input)
{
  return input->p.name;
}

struct ffmixer * ff_get_input_by_name(const char * name)
{
  for ( size_t i = 0, n = ccarray_size(&g_inputs); i < n; ++i ) {
    struct ffmixer * input = ccarray_ppeek(&g_inputs, i);
    if ( strcmp(name, input->p.name) == 0 ) {
      return input;
    }
  }
  return NULL;
}

int ff_start_input_stream(struct ffmixer ** pp, const char * name, struct ff_start_input_args * args)
{
  struct ffmixer * input = NULL;
  int status = 0;

  *pp = NULL;

  if ( !(input = ff_get_input_by_name(name)) ) {
    PDBG("ff_get_input_by_name(%s) fails", name);
    status = AVERROR(ENOENT);
    goto end;
  }

  w_lock(input);

  if ( input->state != ff_input_state_idle ) {
    if ( args ) {
      status = AVERROR(EACCES);
    }
    goto end;
  }

  if ( !args ) {
    memset(&input->args, 0, sizeof(input->args));
  }
  else if ( input->p.source != NULL ) {
    status = AVERROR(EINVAL);
    goto end;
  }
  else {
    input->args = *args;
  }

  input->state = ff_input_state_connecting;

  if ( !(input->gop = ffgop_create(256, ffgop_pkt)) ) {
    status = AVERROR(errno);
    PDBG("ffgop_create() fails: %s", strerror(errno));
    goto end;
  }


  // select one of pcl or pthread start mode
  if ( can_start_in_cothread(input) ) {
    if ( !ffms_start_cothread(ff_input_thread, input, INPUT_THREAD_STACK_SIZE) ) {
      PDBG("ffms_start_cothread() fails: %s", strerror(errno));
      status = AVERROR(errno);
    }
  }
  else {
    pthread_t pid;
    if ( (status = pthread_create(&pid, NULL, ff_input_thread_helper, input))) {
      PDBG("pthread_create() fails: %s", strerror(status));
      status = AVERROR(status);
    }
  }

end:;

  if ( input ) {
    if ( !status ) {
      *pp = input;
    }
    else {
      ffgop_destroy(&input->gop);
      input->state = ff_input_state_idle;
    }
    w_unlock(input);
  }

  return status;
}







//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#endif // 0
