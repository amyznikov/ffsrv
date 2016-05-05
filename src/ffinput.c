/*
 * ffinputsource.c
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#include "ffinput.h"
#include <pthread.h>
#include "debug.h"


#define INPUT_THREAD_STACK_SIZE   (256*1024)
#define INPUT_IO_BUF_SIZE         (4*1024)

#define TIME_BASE_USEC() \
  (AVRational){1,1000000}


#define objname(inp) \
    (inp)->base.name

enum ff_connection_state {
  ff_connection_state_idle = 0,
  ff_connection_state_connecting = 1,
  ff_connection_state_established = 2,
  ff_connection_state_disconnecting = 3
};


struct ffinput {
  struct ffobject base;

  char * url;
  char * ctxopts;
  int re;
  bool genpts;
  int idle_timeout;

  int (*onrecvpkt)(void * cookie, uint8_t *buf, int buf_size);
  void (*onfinish)(void * cookie, int status);
  void * cookie;

  AVFormatContext * ic;
  struct ffgop gop;
  struct ffgoplistener * gl;

  enum ff_connection_state state;
};


////////////////////////////////////////////////////////////////////////////////////////////



static void input_worker_thread(void * arg);
static void * input_thread_helper(void * arg);
static bool can_start_in_cothread(const struct ffinput * input);




////////////////////////////////////////////////////////////////////////////////////////////


static void on_release_input(void * obj)
{
  struct ffinput * input = obj;

  PDBG("[%s] ENTER ", objname(input));

  free(input->url);
  free(input->ctxopts);
  ffgop_delete_listener(&input->gl);
  ffgop_cleanup(&input->gop);

  PDBG("[%s] LEAVE", objname(input));
}

static int get_input_format_context(void * obj, struct AVFormatContext ** cc)
{
  struct ffinput * input = obj;
  int status = 0;

  while ( input->state == ff_connection_state_connecting ) {
    ffgop_wait_event(input->gl, -1);
  }

  if ( input->state != ff_connection_state_established ) {
    PDBG("BAD input->state=%d", input->state);
    status = AVERROR_EOF;
  }
  else {
    *cc = input->ic;
  }

  return status;
}

static struct ffgop * get_input_gop(void * obj)
{
  return &((struct ffinput*) obj)->gop;
}


int ff_create_input(struct ffobject ** obj, const struct ff_create_input_args * args)
{
  static const struct ff_object_iface iface = {
    .on_add_ref = NULL,
    .on_release = on_release_input,
    .get_format_context = get_input_format_context,
    .get_gop = get_input_gop,
  };

  struct ffinput * input = NULL;

  int status = 0;

  if ( !args || !args->name ) {
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( (args->params && args->params->source && *args->params->source) && args->recv_pkt ) {
    status = AVERROR(EPERM);
    goto end;
  }

  if ( !(args->params && args->params->source && *args->params->source) && !args->recv_pkt ) {
    status = AVERROR(EPERM);
    goto end;
  }

  if ( !(input = ff_create_object(sizeof(struct ffinput), object_type_input, args->name, &iface)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( (status = ffgop_init(&input->gop, 256, ffgop_pkt)) ) {
    goto end;
  }

  if ( (status = ffgop_create_listener(&input->gop, &input->gl)) ) {
    goto end;
  }

  input->url = (args->params && args->params->source && *args->params->source) ? strdup(args->params->source) : NULL;
  input->ctxopts = (args->params && args->params->opts) ? strdup(args->params->opts) : NULL;
  input->re = args->params->re;
  input->genpts = args->params->genpts != 0;
  input->idle_timeout = 10;
  input->onrecvpkt = args->recv_pkt;
  input->onfinish = args->on_finish;
  input->cookie = args->cookie;

  // select one of pcl or pthread operation mode

  input->state = ff_connection_state_connecting;
  add_object_ref(&input->base);

  if ( can_start_in_cothread(input) ) {
    if ( !co_schedule(input_worker_thread, input, INPUT_THREAD_STACK_SIZE) ) {
      status = AVERROR(errno);
      PDBG("co_schedule(input_worker_thread) fails: %s", strerror(errno));
      release_object(&input->base);
    }
  }
  else {
    pthread_t pid;
    if ( (status = pthread_create(&pid, NULL, input_thread_helper, input))) {
      PDBG("pthread_create(input_thread_helper) fails: %s", strerror(status));
      status = AVERROR(status);
      release_object(&input->base);
    }
  }

end :

  if ( status && input ) {
    release_object(&input->base);
    input = NULL;
  }

  *obj = (void*)input;

  return status;
}




////////////////////////////////////////////////////////////////////////////////////////////



static void * input_thread_helper(void * arg)
{
  pthread_detach(pthread_self());
  input_worker_thread(arg);
  return NULL;
}



static bool can_start_in_cothread(const struct ffinput * ffsrc)
{
  bool fok = false;

  if ( ffsrc->onrecvpkt ) {
    fok = true;
  }
  else if ( ffsrc->url != NULL ) {

    static const char * netprotos[] = {
      "rtsp://", "rtmp://", "hls://", "http://", "https://", "mmsh://", "mmst://",
      "rtp://", "sctp://", "srtp://", "tcp://", "tls://", "udp://", "udplite://", "unix://", "ftp://", "gopher://",
      "httpproxy://", "rtmpe://", "rtmps://", "rtmpt://", "rtmpte://", "ffrtmpcrypt://", "ffrtmphttp://",
    };

    for ( size_t i = 0; i < sizeof(netprotos) / sizeof(netprotos[0]); ++i ) {
      if ( strncmp(ffsrc->url, netprotos[i], strlen(netprotos[i])) == 0 ) {
        fok = true;
        break;
      }
    }
  }

  return fok;
}


#define FF_POPEN_INPUT_BUF_SIZE (256*1024)
struct ff_popen_context {
  char * command;
  FILE * fp;
  uint8_t * iobuf;
  int fd;
};

static int ff_popen_read_pkt(void * opaque, uint8_t * buf, int buf_size)
{
  struct ff_popen_context * ctx = opaque;
  int status = 0;

  if ( !ctx->fp && !(ctx->fp = popen(ctx->command, "r")) ) {
    status = AVERROR(errno);
    goto end;
  }

  if ( ctx->fd == -1 && (ctx->fd = fileno(ctx->fp)) ) {
    status = AVERROR(errno);
    goto end;
  }

  //PDBG("C co_read(buf=%p buf_size=%d)", buf, buf_size);
  status = co_read(ctx->fd, buf, buf_size);
  //PDBG("C co_read(): status=%d", status);

end:

  return status;
}

static int ff_create_popen_context(AVIOContext ** pb, const char * command)
{
  struct ff_popen_context * ctx = NULL;
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

static void ff_close_popen_context(AVIOContext ** pb)
{
  if ( pb && *pb ) {

    struct ff_popen_context * ctx = (*pb)->opaque;

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




static void ff_usleep(int64_t usec)
{
  if ( is_in_cothread() ) {
    co_sleep(usec);
  }
  else {
    ffmpeg_usleep(usec);
  }
}

static void input_worker_thread(void * arg)
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


  PDBG("[%s] ENTER", objname(input));


  ////////////////////////////////////////////////////////////////////

  if ( input->url && strncasecmp(input->url, "popen://", 8) == 0 ) {

    if ( (status = ff_create_popen_context(&pb, input->url + 8)) ) {
      PDBG("ff_create_popen_context() fails: %s", av_err2str(status));
      goto end;
    }

    pb_type = pb_type_popen;
  }
  else if ( input->onrecvpkt ) {

    uint8_t * iobuf = av_malloc(INPUT_IO_BUF_SIZE);
    if ( !iobuf ) {
      PDBG("av_malloc(iobuf) fails");
      status = AVERROR(ENOMEM);
      goto end;
    }

    pb = avio_alloc_context(iobuf, INPUT_IO_BUF_SIZE, false, input->cookie, input->onrecvpkt, NULL, NULL);
    if ( !pb ) {
      av_free(iobuf);
      status = AVERROR(ENOMEM);
      goto end;
    }

    pb_type = pb_type_callback;
  }

  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] ffmpeg_open_input('%s')", objname(input), input->url);
  if ( (status = ffmpeg_open_input(&input->ic, input->url, NULL, pb, NULL, input->ctxopts)) ) {
    PDBG("[%s] ffmpeg_open_input() fails: %s", objname(input), av_err2str(status));
    goto end;
  }

  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] C ffmpeg_probe_input()", objname(input));
  if ( (status = ffmpeg_probe_input(input->ic, 1)) < 0 ) {
    PDBG("[%s] ffmpeg_probe_input() fails", objname(input));
    goto end;
  }

  if ( input->genpts ) {
    for ( uint i = 0; i < input->ic->nb_streams; ++i ) {
      AVStream * st = input->ic->streams[i];
      st->time_base = TIME_BASE_USEC();
      if ( st->codec ) {
        st->codec->time_base = TIME_BASE_USEC();
      }
    }
  }

  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] ESTABLISHED", objname(input));

  input->state = ff_connection_state_established;
  ffgop_set_event(&input->gop);


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

  if ( input->re > 0 ) {
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

    if ( input->base.refs > 1 ) {
      idle_time = 0;
    }
    else if ( input->idle_timeout > 0 ) {
      if ( !idle_time ) {
        idle_time = ffmpeg_gettime() + input->idle_timeout * (int64_t) 1000000;
      }
      else if ( ffmpeg_gettime() >= idle_time ) {
        PDBG("[%s] EXIT BY IDLE TIMEOUT: refs = %d", objname(input), input->base.refs );
        status = AVERROR(ECHILD);
        break;
      }
    }


    while ( (status = av_read_frame(input->ic, &pkt)) == AVERROR(EAGAIN) ) {
      PDBG("[%s] EAGAIN: status = %d", objname(input), status);
      ff_usleep(10 * 1000);
    }


    if ( status ) {
      PDBG("[%s] BREAK: %s", objname(input), av_err2str(status));
      ff_avpacket_unref(&pkt);
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

    if ( input->genpts ) {
      pkt.pts = pkt.dts = ffmpeg_gettime();
      if ( pkt.pts <= ppts[stream_index] ) {
        pkt.pts = pkt.dts = ppts[stream_index] + 1;
      }
    }
    else if ( input->re > 0 && stream_index == input->re - 1
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
      PDBG("[%s][%d] WAIT REORDERING", objname(input), stream_index);
      process_packet = false;
    }


    // broadcat packet
    if ( process_packet ) {
      ffgop_put_pkt(&input->gop, &pkt, input->ic->streams[pkt.stream_index]->codec->codec_type);
    }

    ff_avpacket_unref(&pkt);
  }

  ////////////////////////////////////////////////////////////////////


end: ;

  ffgop_put_eof(&input->gop, status);

  input->state = ff_connection_state_disconnecting;

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
        ff_close_popen_context(&pb);
      break;
      default :
        PDBG("BUG: pb_type = %d", pb_type);
        exit(1);
      break;
    }
  }

  input->state = ff_connection_state_idle;

  if ( input->onfinish ) {
    input->onfinish(input->cookie, status);
  }

  PDBG("[%s] C release_object(): refs=%d", objname(input), input->base.refs);

  release_object(&input->base);

  PDBG("FINISHED");
}

