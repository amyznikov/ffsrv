/*
 * ffinputsource.c
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#include "ffinput.h"
#include "ffcfg.h"
#include "ffgop.h"
#include "debug.h"


#define INPUT_THREAD_STACK_SIZE   (ffsrv.mem.ffinput)
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
  char * decopts;
  int re;
  bool genpts:1, usesmap:1;
  int rtmo, itmo;

  int (*onrecvpkt)(void * cookie, uint8_t *buf, int buf_size);
  void * cookie;

  coevent * ev;
  pthread_rwlock_t rwlock;
  enum ff_connection_state state;

  uint nb_input_streams;
  uint nb_output_streams;
  struct ffstream ** oss; // [nb_output_streams]
  struct ffstmap * smap;  // [nb_output_streams]

  struct ffgop gop;
};


////////////////////////////////////////////////////////////////////////////////////////////



static void run_input_in_cothread(void * arg);
static void * run_input_in_pthread(void * arg);
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

static int alloc_input_streams(struct ffinput * input, AVFormatContext * ic, AVDictionary * opts)
{
  int status = 0;

  if ( (status = ffmpeg_parse_stream_mapping(opts, (uint[] ) { ic->nb_streams }, 1, &input->smap)) < 0 ) {
    PDBG("[%s] ffmpeg_parse_stream_mapping() fails: %s", objname(input), av_err2str(status));
    goto end;
  }

  if ( (input->nb_output_streams = status) < 1 ) {
    PDBG("[%s] NO OUTPUT STREAMS. CHECK STREAM MAPPING", objname(input));
    status = AVERROR(EINVAL);
    goto end;
  }

  PDBG("nb_output_streams=%u", input->nb_output_streams);


  input->nb_input_streams = ic->nb_streams;

  if ( !(input->oss = ffmpeg_alloc_ptr_array(input->nb_output_streams, sizeof(struct ffstream))) ) {
    status = AVERROR(ENOMEM);
  }
  else {

    for ( uint i = 0; i < input->nb_output_streams; ++i ) {
      int istidx = input->smap[i].isidx;
      if ( (status = ffstream_init(input->oss[i], ic->streams[istidx])) ) {
        PDBG("[%s] ffstream_init(ist=%u ost=%d) fails", objname(input), istidx, i);
        break;
      }
    }
  }

end:

  return status;
}

static void free_input_streams(struct ffinput * input)
{
  if ( input->oss ) {
    for ( uint i = 0; i < input->nb_output_streams; ++i ) {
      if ( input->oss[i] ) {
        ffstream_cleanup(input->oss[i]);
      }
    }
    ffmpeg_free_ptr_array(&input->oss, input->nb_output_streams);
    free(input->smap);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////





static void on_destroy_input(void * obj)
{
  struct ffinput * input = obj;
  free(input->url);
  free(input->ctxopts);
  free(input->decopts);
  ffgop_cleanup(&input->gop);
  free_input_streams(input);
  coevent_delete(&input->ev);
  rwlock_destroy(input);
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
    PDBG("BAD input->state=%d refs=%d", input->state, input->base.refs);
    status = AVERROR_EOF;
  }
  else {
    *streams = (const ffstream * const *) input->oss;
    *nb_streams = input->nb_output_streams;
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


const char * ff_input_decopts(const struct ffinput * input)
{
  return input->decopts;
}

int ff_create_input(struct ffobject ** obj, const struct ff_create_input_args * args)
{
  static const struct ffobject_iface iface = {
    .on_add_ref = NULL,
    .on_destroy = on_destroy_input,
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

  if ( !(input = create_object(sizeof(struct ffinput), ffobjtype_input, args->name, &iface)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  status = ffgop_init(&input->gop, &(struct ffgop_init_args ) {
        .type = ffgop_pkt,
        .capacity = INPUT_FFGOP_SIZE
      });

  if ( status ) {
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
  input->decopts = (args->params && args->params->decopts ) ? strdup(args->params->decopts) : NULL;
  input->re = args->params->re;
  input->genpts = args->params->genpts != 0;
  input->onrecvpkt = args->recv_pkt;
  input->cookie = args->cookie;
  input->rtmo = args->params->rtmo > 0 ? args->params->rtmo : 15;
  input->itmo = args->params->itmo > 0 ? args->params->itmo : 5;

  // select one of pcl or pthread operation mode

  input->state = ff_connection_state_connecting;

  if ( input->url ) {

    add_object_ref(&input->base);

    if ( can_start_in_cothread(input) ) {
      if ( !co_schedule(run_input_in_cothread, input, INPUT_THREAD_STACK_SIZE) ) {
        status = AVERROR(errno);
        PDBG("co_schedule(input_worker_thread) fails: %s", strerror(errno));
        release_object(&input->base);
      }
    }
    else {
      pthread_t pid;
      if ( (status = pthread_create(&pid, NULL, run_input_in_pthread, input))) {
        PDBG("pthread_create(input_thread_helper) fails: %s", strerror(status));
        status = AVERROR(status);
        release_object(&input->base);
      }
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



static void * run_input_in_pthread(void * arg)
{
  pthread_detach(pthread_self());
  ff_run_input_stream(arg);
  return NULL;
}

static void run_input_in_cothread(void * arg)
{
  ff_run_input_stream(arg);
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


////////////////////////////////////////////////////////////////////////////////////////////


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


int ff_run_input_stream(struct ffinput * input)
{
  AVDictionary * opts = NULL;
  AVDictionary * input_opts = NULL;
  AVFormatContext * ic = NULL;
  AVStream * is;
  const struct ffstream * os;
  char * url = NULL;

  AVPacket pkt;
  int isidx, * osidxs, nbos;


  // for rate emulation
  int64_t * firstdts = NULL;
  int64_t * prevdts = NULL;
  int64_t ts0 = 0, now = 0 ;

  int64_t pkt_pts, pkt_dts, pkt_duration;

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

  if ( input->url && *input->url ) {
    PDBG("[%s] C co_resolve_url_4(%s)", objname(input), input->url);
    if ( !(url = co_resolve_url_4(input->url, 20)) ) {
      status = AVERROR(errno);
      PDBG("co_resolve_url_4(%s) fails: %s", input->url, av_err2str(status));
      goto end;
    }
    PDBG("[%s] R co_resolve_url_4(%s) -> %s", objname(input), input->url, url);
  }


  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] ffmpeg_open_input('%s')", objname(input), url);

  set_timeout_interrupt_callback(input, &tcb);

  if ( opts ) {
    av_dict_copy(&input_opts, opts, 0);
  }

  if ( (status = ffmpeg_open_input(&ic, url, pb, &tcb.icb, &input_opts)) ) {
    PDBG("[%s] ffmpeg_open_input() fails: %s", objname(input), av_err2str(status));
    goto end;
  }


  ////////////////////////////////////////////////////////////////////

  PDBG("[%s] ffmpeg_probe_input()", objname(input));
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

  if ( (status = alloc_input_streams(input, ic, opts))) {
    PDBG("[%s] alloc_streams() fails", objname(input));
    goto end;
  }

  ffgop_set_streams(&input->gop, (const ffstream**) input->oss, input->nb_output_streams);


  ////////////////////////////////////////////////////////////////////


  PDBG("[%s] ESTABLISHED. RE=%d nb_output_streams=%d", objname(input), input->re, input->nb_output_streams);

  set_input_state(input, ff_connection_state_established);

  av_init_packet(&pkt), pkt.data = NULL, pkt.size = 0;


  osidxs = alloca(input->nb_output_streams * sizeof(*osidxs));
  firstdts = alloca(input->nb_input_streams * sizeof(*firstdts));
  prevdts = alloca(sizeof(*prevdts) * input->nb_input_streams);
  for ( uint i = 0; i < input->nb_input_streams; ++i ) {
    prevdts[i] = firstdts[i] = AV_NOPTS_VALUE;
  }


  if ( input->re > 0 ) {
    ts0 = ffmpeg_gettime_us();
  }

  idle_time = 0;

  while ( 42 ) {

    if ( input->base.refs > 1 ) {
      idle_time = 0;
    }
    else if ( input->itmo > 0 ) {
      if ( !idle_time ) {
        idle_time = ffmpeg_gettime_us() + input->itmo * FFMPEG_TIME_SCALE;
      }
      else if ( ffmpeg_gettime_us() >= idle_time ) {
        PDBG("[%s] EXIT BY IDLE TIMEOUT %d: refs = %d", objname(input), input->itmo, input->base.refs );
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
      break;
    }

    //    PDBG("[%s] IPKT [st=%d] %s pts=%s dts=%s", objname(input), pkt.stream_index,
    //        av_get_media_type_string(ic->streams[istidx]->codecpar->codec_type), av_ts2str(pkt.pts), av_ts2str(pkt.dts));


    isidx = pkt.stream_index;
    if ( (nbos = ffmpeg_map_input_stream(input->smap, input->nb_output_streams, 0, isidx, osidxs)) < 1 ) {
      av_packet_unref(&pkt);
      continue;
    }


    is = ic->streams[isidx];


    if ( input->genpts ) {
      pkt.pts = pkt.dts = av_rescale_ts(ffmpeg_gettime_us(), TIME_BASE_USEC, is->time_base);
      if ( pkt.dts <= prevdts[isidx] ) {
        pkt.pts = pkt.dts = prevdts[isidx] + 1;
      }
    }
    else if ( input->re > 0 && isidx == input->re - 1 && firstdts[isidx] != AV_NOPTS_VALUE ) {

      int64_t ts = av_rescale_ts(pkt.dts - firstdts[isidx], is->time_base, TIME_BASE_USEC);

      if ( (now = ffmpeg_gettime_us() - ts0) < ts ) {
        // PDBG("[%s] sleep %s", objname(input), av_ts2str(ts - now));
        ff_usleep(ts - now);
      }
    }

    if ( firstdts[isidx] == AV_NOPTS_VALUE ) {
      firstdts[isidx] = pkt.dts; /* save first dts */
    }

    if ( pkt.dts <= prevdts[isidx] ) {
      PDBG("[%s] [ist=%d] WAIT FOR REORDER: dts=%s pdts=%s", objname(input), isidx, av_ts2str(pkt.dts), av_ts2str(prevdts[isidx]));
      av_packet_unref(&pkt);
      continue;
    }

    prevdts[isidx] = pkt.dts;

    pkt_pts = pkt.pts;
    pkt_dts = pkt.dts;
    pkt_duration = pkt.duration;

    for ( int j = 0; j < nbos; ++j ) {

      int ostidx = osidxs[j];

      os = input->oss[ostidx];

      // broadcat packet
      if ( pkt.pts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE ) {
        pkt.pts = pkt.dts;
      }

      if ( is->time_base.num != os->time_base.num || is->time_base.den != os->time_base.den ) {
        pkt.pts = pkt_pts;
        pkt.dts = pkt_dts;
        pkt.duration = pkt_duration;
        av_packet_rescale_ts(&pkt, is->time_base, os->time_base);
      }

      pkt.stream_index = ostidx;

//       PDBG("[%s] OPKT [ost=%d] %s pts=%s dts=%s itb=%s otb=%s", objname(input), ostidx,
//         av_get_media_type_string(is->codecpar->codec_type), av_ts2str(pkt.pts), av_ts2str(pkt.dts),
//         av_tb2str(is->time_base), av_tb2str(os->time_base));

      ffgop_put_pkt(&input->gop, &pkt);
    }

    av_packet_unref(&pkt);
  }

  ////////////////////////////////////////////////////////////////////


end: ;

  ffgop_put_eof(&input->gop, status);

  set_input_state(input, ff_connection_state_disconnecting);

  if ( ic ) {
    ffmpeg_close_input(&ic);
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

  av_dict_free(&opts);

  free(url);

  if ( input->url ) {
    // release ref counter only when running in own (co)thread
    release_object(&input->base);
  }

  PDBG("FINISHED");
  return status;
}

