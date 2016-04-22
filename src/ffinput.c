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
#include "libffms.h"
#include "sockopt.h"
#include "ccarray.h"
#include "ffmpeg.h"
#include "debug.h"

#define INPUT_IO_BUF_SIZE         (4*1024)
#define INPUT_THREAD_STACK_SIZE   (256*1024)

#define TCP_OUTPUT_IO_BUF_SIZE    (32*1024)
#define RTP_OUTPUT_IO_BUF_SIZE    1472



#define TIME_BASE_USEC() \
  (AVRational){1,1000000}


struct ffoutput {
  struct ffinput * input;
  struct AVOutputFormat * format;

  int (*onsendpkt)(void * cookie, int stream_index, uint8_t * buf, int buf_size);
  void * cookie;

  enum {
    output_type_tcp = 0,
    output_type_rtp = 1,
  } output_type;

  bool running :1, finish :1;

  union {
    // tcp-specific
    struct {
      AVFormatContext * oc;
      uint8_t * iobuf; // [TCP_OUTPUT_IO_BUF_SIZE]
    } tcp;

    // rtp-specific
    struct {
      AVFormatContext ** oc; // [nb_streams]
      uint8_t ** iobuf; // nb_streams * (OUTPUT_IO_BUF_SIZE | RTSP_TCP_MAX_PACKET_SIZE)
      uint nb_streams;
      uint current_stream_index;
    } rtp;
  };
};

struct ffinput {
  struct ffms_input_params p;
  struct ff_start_input_args args;
  struct coevent * evt;
  AVFormatContext * ic;
  pthread_rwlock_t rwlock;
  AVPacket * gop; // gop array
  size_t gopsize; // capacity
  size_t gopwp;   // gop write pos
  size_t gopidx;  // gop index

  enum ff_input_state state;
  int refs;
};



static ccarray_t g_inputs; // <struct ffinput *>


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static inline void w_lock(struct ffinput * input)
{
  pthread_rwlock_wrlock(&input->rwlock);
}

static inline void w_unlock(struct ffinput * input)
{
  pthread_rwlock_unlock(&input->rwlock);
}

static inline void r_lock(struct ffinput * input)
{
  pthread_rwlock_rdlock(&input->rwlock);
}

static inline void r_unlock(struct ffinput * input)
{
  pthread_rwlock_unlock(&input->rwlock);
}

static void ff_broadcast_input_event(struct ffinput * input)
{
  coevent_set(input->evt);
}

static void ff_write_gop(struct ffinput * input, AVPacket * pkt)
{
  enum AVMediaType media_type = input->ic->streams[pkt->stream_index]->codec->codec_type;
  bool is_key_frame = (media_type == AVMEDIA_TYPE_VIDEO) && (pkt->flags & AV_PKT_FLAG_KEY);

  w_lock(input);

  if ( is_key_frame || input->gopwp >= input->gopsize ) {
    input->gopwp = 0;
    ++input->gopidx;
  }

  av_packet_unref(&input->gop[input->gopwp]);
  av_packet_ref(&input->gop[input->gopwp], pkt);
  ++input->gopwp;

  w_unlock(input);

  ff_broadcast_input_event(input);
}

static void ff_usleep(int64_t usec)
{
  if ( is_in_cothread() ) {
    co_sleep(usec);
  }
  else {
    ffmpeg_usleep(usec);
  }
}


#define FF_POPEN_INPUT_BUF_SIZE (64*1024)
struct ff_popen_input_context {
  char * command;
  FILE * fp;
  uint8_t * iobuf;
  int fd;
};

static int ff_popen_read_pkt(void * opaque, uint8_t * buf, int buf_size)
{
  struct ff_popen_input_context * ctx = opaque;
  int status = 0;

  if ( !ctx->fp && !(ctx->fp = popen(ctx->command, "r")) ) {
    status = AVERROR(errno);
    goto end;
  }

  if ( ctx->fd == -1 && (ctx->fd = fileno(ctx->fp)) ) {
    status = AVERROR(errno);
    goto end;
  }

  status = co_read(ctx->fd, buf, buf_size);

end:

  return status;
}

static int ff_create_popen_input_context(AVIOContext ** pb, const char * command)
{
  struct ff_popen_input_context * ctx = NULL;
  const size_t bufsize = FF_POPEN_INPUT_BUF_SIZE;
  int status = 0;

  *pb = NULL;

  if ( !(ctx = av_mallocz(sizeof(*ctx))) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  ctx->fd = -1;

  if ( !(ctx->command = strdup(command)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(ctx->iobuf = av_malloc(bufsize)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(*pb = avio_alloc_context(ctx->iobuf, bufsize, false, ctx, ff_popen_read_pkt, NULL, NULL)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }


end:

  if ( status ) {
    if ( ctx ) {
      free(ctx->command);
      av_free(ctx->iobuf);
      av_free(ctx);
    }
  }

  return status;
}

static void ff_close_popen_input_context(AVIOContext ** pb)
{
  if ( pb && *pb ) {

    struct ff_popen_input_context * ctx = (*pb)->opaque;

    if ( ctx ) {

      if ( ctx->fp ) {
        PDBG("C pclose(fp=%p)", ctx->fp);
        pclose(ctx->fp);
        PDBG("R pclose(fp=%p)", ctx->fp);
      }
      free(ctx->command);
    }

    av_free((*pb)->buffer);
    av_freep(pb);
  }
}



static void ff_input_thread(void * arg)
{
  struct ffinput * input = arg;

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

  input->gopwp = 0;
  input->gopidx = 0;


  if ( !(input->gop = calloc(input->gopsize = 2 * 1024, sizeof(AVPacket))) ) {
    PDBG("calloc(GOP) fails");
    status = AVERROR(ENOMEM);
    goto end;
  }

  for ( size_t i = 0; i < input->gopsize; ++i ) {
    av_init_packet(&input->gop[i]);
  }

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
  ff_broadcast_input_event(input);

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

    //PDBG("PKT st=%d pts=%"PRId64" dts=%"PRId64"", stream_index, pkt.pts, pkt.dts);

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

  input->gopwp = input->gopidx = 0;
  if ( input->gop ) {
    for ( size_t i = 0; i < input->gopsize; ++i ) {
      av_packet_unref(&input->gop[i]);
    }
  }

  if ( input->args.onfinish ) {
    input->args.onfinish(input->args.cookie);
  }

  input->state = ff_input_state_idle;
  w_unlock(input);

  ff_broadcast_input_event(input);


  PDBG("leave");
}



static void * ff_input_thread_helper(void * arg)
{
  pthread_detach(pthread_self());
  ff_input_thread(arg);
  return NULL;
}



static bool can_start_in_cothread(const struct ffinput * input)
{
  bool fok = false;

  if ( input->args.onrecvpkt != NULL ) {
    fok = true;
  }
  else if ( input->p.source != NULL ) {

    static const char * netprotos[] = {
      "rtsp://", "rtmp://", "hls://", "http://", "https://", "mmsh://", "mmst://",
      "rtp://", "sctp://", "srtp://", "tcp://", "tls://", "udp://", "udplite://", "unix://", "ftp://", "gopher://",
      "httpproxy://", "rtmpe://", "rtmps://", "rtmpt://", "rtmpte://", "ffrtmpcrypt://", "ffrtmphttp://",
    };

    for ( size_t i = 0; i < sizeof(netprotos) / sizeof(netprotos[0]); ++i ) {
      if ( strncmp(input->p.source, netprotos[i], strlen(netprotos[i])) == 0 ) {
        fok = true;
        break;
      }
    }
  }

  return fok;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool ffinput_initialize(void)
{
  bool fok = false;

  av_register_all();
  avdevice_register_all();
  avformat_network_init();
  //av_log_set_level(AV_LOG_TRACE);

  if ( !ccarray_init(&g_inputs, 1024, sizeof(struct ffinput*)) ) {
    goto end;
  }

  extern int (*ff_poll)(struct pollfd *__fds, nfds_t __nfds, int __timeout);
  ff_poll = co_poll;

  fok = true;

end: ;

  return fok;
}


struct ffinput * ff_create_input(struct ff_create_input_args args)
{
  struct ffinput * input = NULL;
  bool fok = false;

  if ( ccarray_size(&g_inputs) >= ccarray_capacity(&g_inputs) ) {
    errno = ENOMEM;
    goto end;
  }

  if ( !(input = calloc(1, sizeof(struct ffinput))) ) {
    goto end;
  }

  pthread_rwlock_init(&input->rwlock, NULL);


  if ( !(input->evt = coevent_create()) ) {
    PDBG("co_create_event() fails");
    goto end;
  }

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
  input->gopwp = 0;
  input->gopidx = 0;
  input->refs = 0;
  input->state = ff_input_state_idle;

  ccarray_ppush_back(&g_inputs, input);
  fok = true;

end: ;

  if ( !fok ) {
    if ( input ) {
      free(input->gop);
      coevent_delete(&input->evt);
      pthread_rwlock_destroy(&input->rwlock);
      input = NULL;
    }
  }

  return input;
}


void ff_destroy_input(struct ffinput * input)
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


const char * ff_get_input_name(const struct ffinput * input)
{
  return input->p.name;
}

struct ffinput * ff_get_input_by_name(const char * name)
{
  for ( size_t i = 0, n = ccarray_size(&g_inputs); i < n; ++i ) {
    struct ffinput * input = ccarray_ppeek(&g_inputs, i);
    if ( strcmp(name, input->p.name) == 0 ) {
      return input;
    }
  }
  return NULL;
}

int ff_start_input_stream(struct ffinput ** pp, const char * name, struct ff_start_input_args * args)
{
  struct ffinput * input = NULL;
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
    if ( status ) {
      input->state = ff_input_state_idle;
    }
    else {
      *pp = input;
    }
    w_unlock(input);
  }

  return status;
}







//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static int ff_wait_for_input_start(struct ffinput * input)
{
  struct coevent_waiter * w = NULL;
  int status = 0;

  w = coevent_add_waiter(input->evt);

  while ( (input->state == ff_input_state_connecting) ) {
    r_unlock(input);
    coevent_wait(w,-1);
    r_lock(input);
  }

  if ( input->state != ff_input_state_established ) {
    PDBG("NOT ESTABLISHED: state=%d", input->state);
    status = AVERROR_EXIT;
  }

  coevent_remove_waiter(input->evt, w);

  return status;
}

static int ff_output_send_tcp_pkt(void * opaque, uint8_t * buf, int buf_size)
{
  struct ffoutput * output = opaque;
  return output->onsendpkt(output->cookie, -1, buf, buf_size);
}

static int ff_output_send_rtp_pkt(void * opaque, uint8_t * buf, int buf_size)
{
  struct ffoutput * output = opaque;
  return output->onsendpkt(output->cookie, output->rtp.current_stream_index, buf, buf_size);
}


static int ff_create_output_context(struct ffoutput * output)
{
  int status = 0;

  r_lock(output->input);
  status = ff_wait_for_input_start(output->input);
  r_unlock(output->input);

  if ( status ) {
    PDBG("ff_wait_for_input_start() fails: %s", av_err2str(status));
    goto end;
  }

  if ( output->output_type == output_type_tcp ) {

    if ( !( output->tcp.iobuf = av_malloc(TCP_OUTPUT_IO_BUF_SIZE)) ) {
      PDBG("av_malloc(iobuf) fails");
      status = AVERROR(ENOMEM);
      goto end;
    }

    if ( (status = avformat_alloc_output_context2(&output->tcp.oc, output->format, NULL, NULL)) ) {
      PDBG("avformat_alloc_output_context2() fails: %s", av_err2str(status));
      goto end;
    }

    output->tcp.oc->flags |= AVFMT_FLAG_CUSTOM_IO;
    output->tcp.oc->pb = avio_alloc_context(output->tcp.iobuf, TCP_OUTPUT_IO_BUF_SIZE, true, output, NULL,
        ff_output_send_tcp_pkt, NULL);
    if ( !output->tcp.oc->pb ) {
      status = AVERROR(ENOMEM);
      goto end;
    }

    av_dict_set(&output->tcp.oc->metadata, "title", "No Title", 0);

    if ( (status = ffmpeg_copy_streams(output->input->ic, output->tcp.oc)) ) {
      PDBG("ffmpeg_copy_stream() fails: %s", av_err2str(status));
      goto end;
    }

  }
  else {

    struct ffinput * input = output->input;

    PDBG("BEGIN");

    if ( (status = ff_wait_for_input_start(input)) ) {
      PDBG("ff_wait_for_input_start() fails: %s", av_err2str(status));
      goto end;
    }

    output->rtp.nb_streams = input->ic->nb_streams;
    output->rtp.oc = av_malloc_array(output->rtp.nb_streams, sizeof(AVFormatContext*));
    output->rtp.iobuf = av_malloc_array(output->rtp.nb_streams, sizeof(uint8_t*));

    PDBG("COPY STREAMS output->format=%p %s", output->format, output->format ? output->format->name : "");



    for ( uint i = 0; i < output->rtp.nb_streams; ++i ) {

      if ( (status = avformat_alloc_output_context2(&output->rtp.oc[i], output->format, "rtp", NULL)) ) {
        PDBG("avformat_alloc_output_context2() fails: %s", av_err2str(status));
        break;
      }

      output->rtp.oc[i]->flags |= AVFMT_FLAG_CUSTOM_IO;
      av_dict_set(&output->rtp.oc[i]->metadata, "title", "No Title", 0);


      if ( !(output->rtp.iobuf[i] = av_malloc(RTP_OUTPUT_IO_BUF_SIZE))) {
        PDBG("av_malloc(iobuf) fails");
        status = AVERROR(ENOMEM);
        break;
      }

      output->rtp.oc[i]->pb = avio_alloc_context(output->rtp.iobuf[i], RTP_OUTPUT_IO_BUF_SIZE, true, output, NULL,
          ff_output_send_rtp_pkt, NULL);
      if ( !output->rtp.oc[i]->pb ) {
        PDBG("avio_alloc_context() fails");
        status = AVERROR(ENOMEM);
        break;
      }

      output->rtp.oc[i]->pb->max_packet_size = RTP_OUTPUT_IO_BUF_SIZE;

      if ( !avformat_new_stream(output->rtp.oc[i], NULL) ) {
        PDBG("avformat_new_stream() fails");
        status = AVERROR(ENOMEM);
        break;
      }

      if ( (status = ffmpeg_copy_stream(input->ic->streams[i], output->rtp.oc[i]->streams[0], output->format)) ) {
        PDBG("ffmpeg_copy_stream() fails: %s", av_err2str(status));
        break;
      }

      output->rtp.oc[i]->streams[0]->index = 0;
    }

    PDBG("COPY FINISH");

    if ( status ) {
      goto end;
    }
  }

end:

  return status;
}


static void ff_destroy_output_context(struct ffoutput * output)
{
  if ( output->output_type == output_type_tcp ) {
    if ( output->tcp.oc ) {
      av_freep(&output->tcp.oc->pb->buffer);
      av_freep(&output->tcp.oc->pb);
      ffmpeg_close_output(&output->tcp.oc);
    }
  }
  else if ( output->output_type == output_type_rtp ) {
    for ( uint i = 0; i < output->rtp.nb_streams; ++i ) {
      if ( output->rtp.oc[i] ) {
        av_freep(&output->rtp.oc[i]->pb->buffer);
        av_freep(&output->rtp.oc[i]->pb);
        ffmpeg_close_output(&output->rtp.oc[i]);
      }
    }
  }
}



int ff_create_output_stream(struct ffoutput ** pp, const char * name, const struct ff_start_output_args * args)
{
  struct ffoutput * output = NULL;

  const char * output_format_name = "matroska";
  AVOutputFormat * format = NULL;
  bool rtp = false;

  int status = 0;

//  PDBG("ENTER");

  if ( !args->onsendpkt ) {
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( args->format ) {
    output_format_name = args->format;
  }

  if ( !(format = av_guess_format(output_format_name, NULL, NULL)) ) {
    PDBG("av_guess_format(%s) fails", output_format_name);
    status = AVERROR_MUXER_NOT_FOUND;
    goto end;
  }

  if ( !(output = calloc(1, sizeof(struct ffoutput))) ) {
    PDBG("calloc() fails");
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( strcasecmp(output_format_name, "rtp") == 0 ) {
    rtp = true;
  }

  output->format = format;
  output->output_type = rtp ? output_type_rtp : output_type_tcp;
  output->cookie = args->cookie;
  output->onsendpkt = args->onsendpkt;
  output->finish = false;

  if ( (status = ff_start_input_stream(&output->input, name, NULL)) ) {
    PDBG("ff_start_input_stream() fails: %s", av_err2str(status));
    goto end;
  }

  w_lock(output->input);
  ++output->input->refs;
  w_unlock(output->input);

  if ( !rtp && (status = ff_create_output_context(output)) ) {
    PDBG("ff_create_output_context() fails: %s", av_err2str(status));
    goto end;
  }

end : ;

  if ( status && output ) {

    /* FIXME: free output context here
     * */

    if ( output->input ) {
      w_lock(output->input);
      --output->input->refs;
      w_unlock(output->input);
    }

    free(output);
    output = NULL;
  }

  *pp = output;

  return status;
}

void ff_delete_output_stream(struct ffoutput * output)
{
  if ( output ) {

    while ( output->running ) {
      output->finish = true;
      coevent_set(output->input->evt);
      co_sleep(100 * 1000);
    }

    if ( output->input ) {
      w_lock(output->input);
      --output->input->refs;
      w_unlock(output->input);
    }

    ff_destroy_output_context(output);
    free(output);
  }
}

const char * ff_get_output_mime_type(struct ffoutput * output)
{
  const char * mime_type = NULL;
  if ( output->format->mime_type && *output->format->mime_type ) {
    mime_type = output->format->mime_type;
  }
  else {
    mime_type = "application/x-octet-stream";
  }
  return mime_type;
}


int ff_get_output_sdp(struct ffoutput * output, char * sdp, int sdpmax)
{
  struct ffinput * input = output->input;
  AVFormatContext * oc = NULL;

  int status = 0;

  r_lock(output->input);
  status = ff_wait_for_input_start(output->input);
  r_unlock(output->input);

  if ( status ) {
    PDBG("ff_wait_for_input_start() fails: %s", av_err2str(status));
    goto end;
  }

  if ( (status = avformat_alloc_output_context2(&oc, NULL, "rtp", "rtp://0.0.0.0")) ) {
    PDBG("avformat_alloc_output_context2() fails: %s", av_err2str(status));
    goto end;
  }

  if ( (status = ffmpeg_copy_streams(input->ic, oc)) ) {
    PDBG("ffmpeg_copy_streams() fails: %s", av_err2str(status));
    goto end;
  }

//  PDBG("codec_id=0x%0X %d AV_CODEC_ID_SPEEX=0x%0X %d", oc->streams[1]->codec->codec_id, oc->streams[1]->codec->codec_id,
//      AV_CODEC_ID_SPEEX, AV_CODEC_ID_SPEEX);


  if ((status = av_sdp_create(&oc, 1, sdp, sdpmax))) {
    PDBG("av_sdp_create() fails: %s", av_err2str(status));
    goto end;
  }


end:

  if ( oc ) {
    avformat_free_context(oc);
  }

  return status;
}

int ff_get_output_nb_streams(struct ffoutput * output, uint * nb_streams)
{
  *nb_streams = output->input->ic->nb_streams;
  return 0;
}

static inline bool tbequal(const AVRational * tb1, const AVRational * tb2)
{
  return tb1->num == tb2->num && tb1->den == tb2->den;
}



int ff_run_output_stream(struct ffoutput * output)
{
  struct ffinput * input = output->input;

  AVPacket pkt;
  AVFormatContext * oc;
  AVStream * os;
  int stream_index;
  uint nb_streams;

  bool gotpkt = false;

  int64_t * dts0 = NULL;
  int64_t * ppts = NULL;

  bool flush_packets = true;
  bool write_header_ok = false;
  //bool skip_gop = false;
  bool gotkeyframe = false;

  size_t rpos = 0;
  size_t gopidx = 0;

  struct coevent_waiter * w = NULL;

  int status = 0;

  output->running = true;

  av_init_packet(&pkt);
  pkt.data = NULL, pkt.size = 0;


  /* Create output context
   * */
  if ( output->output_type == output_type_tcp ) {

    if ( !output->tcp.oc && (status = ff_create_output_context(output)) ) {
      PDBG("ff_create_output_context() fails: %s", av_err2str(status));
      goto end;
    }

    nb_streams = output->tcp.oc->nb_streams;

  }
  else if ( output->output_type == output_type_rtp ) {

    if ( !output->rtp.oc && (status = ff_create_output_context(output)) ) {
      PDBG("ff_create_output_context() fails: %s", av_err2str(status));
      goto end;
    }

    nb_streams = output->rtp.nb_streams;

  }
  else {
    PDBG("BUGL invalid output type %d", output->output_type);
    status = AVERROR_BUG;
    goto end;
  }

  //PDBG("OC: nb_streams=%u", output->oc->nb_streams);

  dts0 = alloca(nb_streams * sizeof(*dts0));
  ppts = alloca(nb_streams * sizeof(*ppts));

  for ( uint i = 0; i < nb_streams; ++i ) {
    dts0[i] = AV_NOPTS_VALUE;
    ppts[i] = 0;
  }


  PDBG("WRITE HEADER");

  if ( output->output_type == output_type_tcp ) {
    output->tcp.oc->flush_packets = 1;
    if ( (status = avformat_write_header(output->tcp.oc, NULL)) >= 0 ) {
      write_header_ok = true;
    }
    else {
      PDBG("[%s] avformat_write_header() fails: %s", input->p.name, av_err2str(status));
      goto end;
    }
  }
  else {
    for ( uint i = 0; i < nb_streams; ++i ) {
      output->rtp.oc[i]->flush_packets = 1;
      if ( (status = avformat_write_header(output->rtp.oc[i], NULL)) >= 0 ) {
        write_header_ok = true;
      }
      else {
        PDBG("[%s] avformat_write_header(st=%u) fails: %s", input->p.name, i, av_err2str(status));
        goto end;
      }
    }
  }

  PDBG("WRITE HEADER OK");

  /* Main loop
   * */

  w = coevent_add_waiter(input->evt);

  while ( status >= 0 && !output->finish && input->state == ff_input_state_established ) {

    gotpkt = false;

    r_lock(input);

    if ( gopidx < input->gopidx ) {
      rpos = 0, gopidx = input->gopidx;    //, skip_gop = 0;
    }

    if ( rpos < input->gopwp ) {
      av_packet_ref(&pkt, &input->gop[rpos++]);
      gotpkt = true;
    }

    r_unlock(input);

    if ( !gotpkt ) {
      //PDBG("WE");
      coevent_wait(w, -1);
      //PDBG("GE");
      continue;
    }

    stream_index = pkt.stream_index;
    //PDBG("stream_index=%d", stream_index);

    if ( output->output_type == output_type_tcp ) {
      oc = output->tcp.oc;
      os = oc->streams[stream_index];
    }
    else {
      oc = output->rtp.oc[stream_index];
      os = oc->streams[0];
    }

    if ( !gotkeyframe && (pkt.flags & AV_PKT_FLAG_KEY) && (os->codec->codec_type == AVMEDIA_TYPE_VIDEO) ) {
      gotkeyframe = true;
    }

   // PDBG("st=%d pts=%"PRId64" dts=%"PRId64" key=%d", stream_index, pkt.pts, pkt.dts, gotkeyframe);

    if ( gotkeyframe && (pkt.pts != AV_NOPTS_VALUE || pkt.dts != AV_NOPTS_VALUE) ) {

      if ( pkt.pts == AV_NOPTS_VALUE ) {
        PDBG("st[%d]: NO PTS", stream_index);
      }

      if ( pkt.dts == AV_NOPTS_VALUE ) {
        PDBG("st[%d]: NO DTS", stream_index);
      }

      if ( dts0[stream_index] == AV_NOPTS_VALUE ) {
        dts0[stream_index] = pkt.dts == AV_NOPTS_VALUE ? pkt.pts : pkt.dts; /* save first dts */
      }

      if ( pkt.pts != AV_NOPTS_VALUE ) {
        pkt.pts -= dts0[stream_index];
      }

      if ( pkt.dts != AV_NOPTS_VALUE ) {
        pkt.dts -= dts0[stream_index];
      }

      if ( !tbequal(&input->ic->streams[stream_index]->time_base, &os->time_base) ) {
        ffmpeg_rescale_timestamps(&pkt, input->ic->streams[stream_index]->time_base,
            os->time_base);
      }

      ppts[stream_index] = pkt.pts;
      //PDBG("[%d] opts=%"PRId64" odts=%"PRId64" key=%d", pkt.stream_index, pkt.pts, pkt.dts, pkt.flags & AV_PKT_FLAG_KEY);

      if ( output->output_type == output_type_rtp ) {
        output->rtp.current_stream_index = stream_index;
        pkt.stream_index = 0;
        status = av_write_frame(oc, &pkt);
        //PDBG("av_write_frame(): %d", status);
      }
      else if ( nb_streams > 1 ) {
        status = av_interleaved_write_frame(oc, &pkt);
      }
      else if ( (status = av_write_frame(oc, &pkt)) == 0 && flush_packets ) {
        status = av_write_frame(oc, NULL);
      }

      if ( status < 0 ) {
        PDBG("[%s] write frame fails: %s", input->p.name, av_err2str(status));
      }
    }

    av_packet_unref(&pkt);
    co_yield();
  }

end : ;

  if ( w ) {
    coevent_remove_waiter(input->evt, w);
  }

  if ( write_header_ok ) {
    if ( output->output_type == output_type_tcp ) {
      av_write_trailer(output->tcp.oc);
    }
    else {

      for ( uint i = 0; i < nb_streams; ++i ) {
        if ( av_write_trailer(output->rtp.oc[i]) != 0 ) {
          break;
        }
      }
    }
  }

  output->running = false;

  return status;
}

