/*
 * ffinputsource.c
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#include "ffinput.h"
#include "ffgop.h"
#include "debug.h"


#define INPUT_THREAD_STACK_SIZE   (1024*1024)
#define INPUT_IO_BUF_SIZE         (4*1024)

#define INPUT_TIME_BASE           (AVRational){1,1000}  // msec
#define TIME_BASE_USEC            (AVRational){1,1000000}  // usec

#define INPUT_FFGOP_SIZE          1024

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
  int rtmo, itmo;

  int (*onrecvpkt)(void * cookie, uint8_t *buf, int buf_size);
  void (*onfinish)(void * cookie, int status);
  void * cookie;

  coevent * ev;
  pthread_rwlock_t rwlock;
  enum ff_connection_state state;

  uint nb_streams;
  struct ffstream ** oss; // [nb_streams]

  struct ffgop gop;
};


////////////////////////////////////////////////////////////////////////////////////////////



static void input_thread(void * arg);
static void * input_thread_helper(void * arg);
static bool can_start_in_cothread(const struct ffinput * input);


////////////////////////////////////////////////////////////////////////////////////////////


static void r_lock(struct ffinput * input)
{
  pthread_rwlock_rdlock(&input->rwlock);
}

static void r_unlock(struct ffinput * input)
{
  pthread_rwlock_unlock(&input->rwlock);
}

static void w_lock(struct ffinput * input)
{
  pthread_rwlock_wrlock(&input->rwlock);
}

static void w_unlock(struct ffinput * input)
{
  pthread_rwlock_unlock(&input->rwlock);
}

static int rwlock_init(struct ffinput * input)
{
  return pthread_rwlock_init(&input->rwlock, NULL);
}

static int rwlock_destroy(struct ffinput * input)
{
  return pthread_rwlock_destroy(&input->rwlock);
}

static void set_input_state(struct ffinput * input, enum ff_connection_state state)
{
  w_lock(input);
  input->state = state;
  w_unlock(input);
  coevent_set(input->ev);
}

static int alloc_streams(struct ffinput * input, AVFormatContext * ic)
{
  int status = 0;

  if ( !(input->oss = ffmpeg_alloc_ptr_array(ic->nb_streams, sizeof(struct ffstream))) ) {
    status = AVERROR(ENOMEM);
  }
  else {
    input->nb_streams = ic->nb_streams;

    for ( uint i = 0; i < ic->nb_streams; ++i ) {

      if ( (status = ffstream_init(input->oss[i], ic->streams[i])) ) {
        PDBG("[%s] ffstream_init() fails", objname(input));
        break;
      }

      // force this
      //input->oss[i]->time_base = INPUT_TIME_BASE;
    }
  }

  return status;
}

static void free_streams(struct ffinput * input)
{
  if ( input->oss ) {
    for ( uint i = 0; i < input->nb_streams; ++i ) {
      if ( input->oss[i] ) {
        ffstream_cleanup(input->oss[i]);
      }
    }
    ffmpeg_free_ptr_array(&input->oss, input->nb_streams);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////





static void on_release_input(void * obj)
{
  struct ffinput * input = obj;

  PDBG("[%s] ENTER ", objname(input));

  free(input->url);
  free(input->ctxopts);
  ffgop_cleanup(&input->gop);
  coevent_delete(&input->ev);
  rwlock_destroy(input);

  PDBG("[%s] LEAVE", objname(input));
}

static int get_input_streams(void * obj, const ffstream *const ** streams, uint * nb_streams)
{
  struct ffinput * input = obj;
  struct coevent_waiter * w;
  int status = 0;

  r_lock(input);

  if ( !(w = coevent_add_waiter(input->ev)) ) {
    status = AVERROR_UNKNOWN;
    goto end;
  }

  while ( input->state == ff_connection_state_connecting ) {
    r_unlock(input);
    coevent_wait(w, -1);
    r_lock(input);
  }

  if ( input->state != ff_connection_state_established ) {
    PDBG("BAD input->state=%d", input->state);
    status = AVERROR_EOF;
  }
  else {
    *streams = (const ffstream * const *) input->oss;
    *nb_streams = input->nb_streams;
  }

end:

  coevent_remove_waiter(input->ev, w);

  r_unlock(input);

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
    .get_streams = get_input_streams,
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

  if ( (status = ffgop_init(&input->gop, INPUT_FFGOP_SIZE, ffgop_pkt, NULL, 0)) ) {
    goto end;
  }

  if ( !(input->ev = coevent_create()) ) {
    status = AVERROR(errno);
    goto end;
  }

  if ( (status = rwlock_init(input)) ) {
    status = AVERROR(status);
    goto end;
  }

  input->url = (args->params && args->params->source && *args->params->source) ? strdup(args->params->source) : NULL;
  input->ctxopts = (args->params && args->params->opts) ? strdup(args->params->opts) : NULL;
  input->re = args->params->re;
  input->genpts = args->params->genpts != 0;
  input->itmo = args->params->itmo;
  input->onrecvpkt = args->recv_pkt;
  input->onfinish = args->on_finish;
  input->cookie = args->cookie;

  input->rtmo = args->params->rtmo > 0 ? args->params->rtmo : 15;
  input->itmo = args->params->itmo > 0 ? args->params->itmo : 5;

  // select one of pcl or pthread operation mode

  input->state = ff_connection_state_connecting;
  add_object_ref(&input->base);

  if ( can_start_in_cothread(input) ) {
    if ( !co_schedule(input_thread, input, INPUT_THREAD_STACK_SIZE) ) {
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
  input_thread(arg);
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


static void set_timeout_interrupt_callback(struct ffinput * input, struct ff_timeout_interrupt_callback * tcb)
{
  ffmpeg_set_timeout_interrupt_callback(tcb, ffmpeg_gettime_us() + input->rtmo * FFMPEG_TIME_SCALE);
}

static void input_thread(void * arg)
{
  struct ffinput * input = arg;

  AVDictionary * opts = NULL;
  AVFormatContext * ic = NULL;
  AVStream * is;
  const struct ffstream * os;

  AVPacket pkt;
  int stidx;

  // for rate emulation
  int64_t * firstdts = NULL;
  int64_t * prevdts = NULL;
  int64_t ts0 = 0, now = 0 ;

  AVIOContext * pb = NULL;

  enum {
    pb_type_none = 0,
    pb_type_callback = 1,
    pb_type_popen = 2,
  } pb_type = pb_type_none;

  int64_t idle_time;

  struct ff_timeout_interrupt_callback tcb;

  int status;


  PDBG("[%s] ENTER", objname(input));


  ffmpeg_init_timeout_interrupt_callback(&tcb);

  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] C ffmpeg_parse_options(opts='%s')", objname(input), input->ctxopts);
  if ( (status = ffmpeg_parse_options(input->ctxopts, true, &opts)) ) {
    PDBG("[%s] ffmpeg_parse_options() fails: %s", objname(input), av_err2str(status));
    goto end;
  }

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

  set_timeout_interrupt_callback(input, &tcb);
  if ( (status = ffmpeg_open_input(&ic, input->url, pb, &tcb.icb, &opts)) ) {
    PDBG("[%s] ffmpeg_open_input() fails: %s", objname(input), av_err2str(status));
    goto end;
  }

  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] C ffmpeg_probe_input()", objname(input));
  set_timeout_interrupt_callback(input, &tcb);
  if ( (status = ffmpeg_probe_input(ic, true)) < 0 ) {
    PDBG("[%s] ffmpeg_probe_input() fails", objname(input));
    goto end;
  }

  if ( input->genpts ) {
    for ( uint i = 0; i < ic->nb_streams; ++i ) {
      AVStream * st = ic->streams[i];
      st->time_base = INPUT_TIME_BASE;
    }
  }

  ////////////////////////////////////////////////////////////////////

  if ( (status = alloc_streams(input, ic))) {
    PDBG("[%s] alloc_streams() fails", objname(input));
    goto end;
  }

  ffgop_set_streams(&input->gop, (const ffstream**) input->oss, input->nb_streams);


  ////////////////////////////////////////////////////////////////////


  PDBG("[%s] ESTABLISHED. RE=%d", objname(input), input->re);

  set_input_state(input, ff_connection_state_established);

  av_init_packet(&pkt), pkt.data = NULL, pkt.size = 0;


  firstdts = alloca(ic->nb_streams * sizeof(*firstdts));
  prevdts = alloca(sizeof(*prevdts) * ic->nb_streams);
  for ( uint i = 0; i < ic->nb_streams; ++i ) {
    prevdts[i] = firstdts[i] = AV_NOPTS_VALUE;
  }

  if ( input->re > 0 ) {
    ts0 = ffmpeg_gettime_us();
  }

  idle_time = 0;

  while ( 42 ) {

    bool process_packet = true;

    if ( input->base.refs > 1 ) {
      idle_time = 0;
    }
    else if ( input->itmo > 0 ) {
      if ( !idle_time ) {
        idle_time = ffmpeg_gettime_us() + input->itmo * FFMPEG_TIME_SCALE;
      }
      else if ( ffmpeg_gettime_us() >= idle_time ) {
        PDBG("[%s] EXIT BY IDLE TIMEOUT: refs = %d", objname(input), input->base.refs );
        status = AVERROR(ECHILD);
        break;
      }
    }

    set_timeout_interrupt_callback(input, &tcb);
    while ( (status = av_read_frame(ic, &pkt)) == AVERROR(EAGAIN) ) {
      ff_usleep(10 * 1000);
    }


    if ( status ) {
      PDBG("[%s] BREAK: %s", objname(input), av_err2str(status));
      av_packet_unref(&pkt);
      break;
    }


    stidx = pkt.stream_index;
    is = ic->streams[stidx];
    os = input->oss[stidx];

//    PDBG("[%s] IPKT [st=%d] %s pts=%s dts=%s itb=%s otb=%s", objname(input), stidx,
//        av_get_media_type_string(is->codecpar->codec_type), av_ts2str(pkt.pts), av_ts2str(pkt.dts),
//        av_tb2str(is->time_base), av_tb2str(os->time_base));

    if ( input->genpts ) {
      pkt.pts = pkt.dts = av_rescale_ts(ffmpeg_gettime_us(), TIME_BASE_USEC, is->time_base);
      if ( pkt.dts <= prevdts[stidx] ) {
        pkt.pts = pkt.dts = prevdts[stidx] + 1;
      }
    }
    else if ( input->re > 0 && stidx == input->re - 1 && firstdts[stidx] != AV_NOPTS_VALUE ) {

      int64_t ts = av_rescale_ts(pkt.dts - firstdts[stidx], is->time_base, TIME_BASE_USEC);

      if ( (now = ffmpeg_gettime_us() - ts0) < ts ) {
        // PDBG("[%s] sleep %s", objname(input), av_ts2str(ts - now));
        ff_usleep(ts - now);
      }
    }

    if ( firstdts[stidx] == AV_NOPTS_VALUE ) {
      firstdts[stidx] = pkt.dts; /* save first dts */
    }

    if ( pkt.dts <= prevdts[stidx] ) {
      process_packet = false;
      PDBG("[%s] [st=%d] WAIT FOR REORDER: dts=%s pdts=%s", objname(input), stidx, av_ts2str(pkt.dts),
          av_ts2str(prevdts[stidx]));
    }

    if ( process_packet ) {

      prevdts[stidx] = pkt.dts;

      // broadcat packet
      if ( pkt.pts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE ) {
        pkt.pts = pkt.dts;
      }

      if ( is->time_base.num != os->time_base.num || is->time_base.den != os->time_base.den ) {
        av_packet_rescale_ts(&pkt, is->time_base, os->time_base);
      }

//      PDBG("[%s] OPKT [st=%d] %s pts=%s dts=%s itb=%s otb=%s", objname(input), stidx,
//          av_get_media_type_string(is->codecpar->codec_type), av_ts2str(pkt.pts), av_ts2str(pkt.dts),
//          av_tb2str(is->time_base), av_tb2str(os->time_base));

      ffgop_put_pkt(&input->gop, &pkt);

    }

    av_packet_unref(&pkt);
  }

  ////////////////////////////////////////////////////////////////////


end: ;

  ffgop_put_eof(&input->gop, status);

  set_input_state(input, ff_connection_state_disconnecting);

  if ( ic ) {
    PDBG("C ffmpeg_close_input(): AVFMT_FLAG_CUSTOM_IO == %d input->ic->pb=%p",
        (ic->flags & AVFMT_FLAG_CUSTOM_IO), ic->pb);
    ffmpeg_close_input(&ic);
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

  set_input_state(input, ff_connection_state_idle);

  if ( input->onfinish ) {
    input->onfinish(input->cookie, status);
  }


  PDBG("[%s] C free_streams()", objname(input));
  free_streams(input);

  PDBG("[%s] C av_dict_free(&opts)", objname(input));
  av_dict_free(&opts);

  PDBG("[%s] C release_object(): refs=%d", objname(input), input->base.refs);
  release_object(&input->base);

  PDBG("FINISHED");
}

