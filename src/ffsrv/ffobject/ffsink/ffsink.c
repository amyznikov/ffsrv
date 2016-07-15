/*
 * ffsink.c
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */

#define _GNU_SOURCE

#include <stdio.h>
#include "ffcfg.h"
#include "ffsink.h"
#include "ffgop.h"
#include "co-scheduler.h"
#include "strfuncs.h"
#include "pathfuncs.h"
#include "debug.h"

#define SINK_THREAD_STACK_SIZE (1024*1024)

#define objname(sink) \
    (sink)->base.name


struct ffsink {
  struct ffobject base;
  struct ffobject * source;
  char * format;
  char * destination;
};

static void on_destroy_sink(void * ffobject)
{
  struct ffsink * sink = ffobject;
  free(sink->destination);
  release_object(sink->source);
  PDBG("[%s] DESTROYED", objname(sink));
}


static void sink_thread(void * arg)
{
  struct ffsink * sink = arg;
  struct ffgop * gop = NULL;
  struct ffgoplistener * gl = NULL;

  const ffstream * const * iss = NULL;
  uint nb_streams = 0;

  AVFormatContext * oc = NULL;
  AVPacket pkt;

  const ffstream * is;
  AVStream * os;
  int stidx;

  int64_t firstdts = AV_NOPTS_VALUE;
  int64_t * tsoffset = NULL;

  bool flush_packets = false;
  bool write_header_ok = false;

  int status = 0;


  av_init_packet(&pkt);
  pkt.data = NULL, pkt.size = 0;

  if ( !(gop = get_gop(sink->source)) || ffgop_get_type(gop) != ffgop_pkt ) {
    PDBG("[%s] Invalid gop specified", objname(sink));
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( (status = ffgop_create_listener(gop, &gl, NULL)) ) {
    PDBG("[%s] ffgop_create_listener() fails: %s", objname(sink), av_err2str(status));
    goto end;
  }

  if ( (status = get_streams(sink->source, &iss, &nb_streams)) ) {
    PDBG("[%s] get_streams() fails: %s", objname(sink), av_err2str(status));
    goto end;
  }

  tsoffset = alloca(nb_streams * sizeof(*tsoffset));
  for ( uint i = 0; i < nb_streams; ++i ) {
    tsoffset[i] = AV_NOPTS_VALUE;
  }


  if ( (status = ffmpeg_create_output_context(&oc, sink->format, iss, nb_streams)) ) {
    PDBG("[%s] ffmpeg_create_output_context('%s') fails: %s", objname(sink), sink->destination, av_err2str(status));
    goto end;
  }

  if ( !(oc->oformat->flags & AVFMT_NOFILE) ) {
    if ( (status = avio_open(&oc->pb, sink->destination, AVIO_FLAG_WRITE)) < 0 ) {
      fprintf(stderr, "[%s] avio_open('%s') fails: %s", objname(sink), sink->destination, av_err2str(status));
      goto end;
    }
  }

  PDBG("[%s] WRITE HEADER: nb_streams=%u '%s'", objname(sink), nb_streams, sink->destination);
  // oc->flush_packets = 1;

  if ( (status = avformat_write_header(oc, NULL)) >= 0 ) {
    write_header_ok = true;
  }
  else {
    PDBG("[%s] avformat_write_header() fails: %s", objname(sink), av_err2str(status));
    goto end;
  }

  PDBG("[%s] WRITE HEADER OK", objname(sink));


  /* Main loop
   * */

  while ( status >= 0 ) {

    if ( (status = ffgop_get_pkt(gl, &pkt)) ) {
      PDBG("[%s] ffgop_get_pkt() fails: %s", objname(sink), av_err2str(status));
      continue;
    }

    stidx = pkt.stream_index;
    is = iss[stidx];
    os = oc->streams[stidx];

//    {
//      int64_t upts = av_rescale_ts(pkt.pts, is->time_base, (AVRational){1, 1000});
//      int64_t udts = av_rescale_ts(pkt.dts, is->time_base, (AVRational){1, 1000});
//      PDBG("IPKT [st=%2d]%c pts=%s dts=%s key=%d\t upts=%s udts=%s", stidx, cc ? '-' : '*' ,
//          av_ts2str(pkt.pts), av_ts2str(pkt.dts), (pkt.flags & AV_PKT_FLAG_KEY), av_ts2str(upts), av_ts2str(udts));
//    }

    if ( firstdts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE ) {

      const AVRational utb = (AVRational ) { 1, 1000000 };

      firstdts = av_rescale_ts(pkt.dts, is->time_base, utb);

      for ( uint i = 0; i < nb_streams; ++i ) {
        tsoffset[i] = av_rescale_ts(firstdts, utb, iss[i]->time_base);
      }
    }

    if ( tsoffset[stidx] != AV_NOPTS_VALUE ) {
      if ( pkt.dts != AV_NOPTS_VALUE ) {
        pkt.dts -= tsoffset[stidx];
      }
      if ( pkt.pts != AV_NOPTS_VALUE ) {
        pkt.pts -= tsoffset[stidx];
      }
    }

    if ( is->time_base.num != os->time_base.num || is->time_base.den != os->time_base.den ) {
      av_packet_rescale_ts(&pkt, is->time_base, os->time_base);
    }

//    {
//      int64_t upts = av_rescale_ts(pkt.pts, is->time_base, (AVRational){1, 1000});
//      int64_t udts = av_rescale_ts(pkt.dts, is->time_base, (AVRational){1, 1000});
//      PDBG("OPKT [st=%2d] pts=%s dts=%s key=%d\t upts=%s udts=%s", stidx,
//          av_ts2str(pkt.pts), av_ts2str(pkt.dts), (pkt.flags & AV_PKT_FLAG_KEY), av_ts2str(upts), av_ts2str(udts));
//    }

//    {
//      const int bytes_per_sec = 100000;    // [B/sec]
//      uint64_t delay = (uint64_t) pkt.size * 1000000 / bytes_per_sec;
//      PDBG("%c st=%2d dp=%3u rp=%3u gidx=%3u size=%4d delay %"PRIu64" ms", output->gl->skip_video ? '-' : '*', pkt.stream_index, dp, output->gl->rpos, output->gl->gopidx,
//          pkt.size, delay / 1000);
//      co_sleep(delay);
//    }

    if ( nb_streams > 1 ) {
      status = av_interleaved_write_frame(oc, &pkt);
    }
    else if ( (status = av_write_frame(oc, &pkt)) == 0 && flush_packets ) {
      status = av_write_frame(oc, NULL);
    }

    if ( status < 0 ) {
      PDBG("[%s] write frame fails: %s", objname(sink), av_err2str(status));
    }

    av_packet_unref(&pkt);
    co_yield();
  }

end : ;

  if ( write_header_ok ) {
    av_write_trailer(oc);
  }

  PDBG("[%s] FINISHED", objname(sink));
  release_object(&sink->base);
}





int ff_create_sink(struct ffobject ** obj, const struct ff_create_sink_args * args)
{
  static const struct ffobject_iface iface = {
    .on_add_ref = NULL,
    .on_destroy = on_destroy_sink,
    .get_streams = NULL,
    .get_gop = NULL,
  };

  struct ffsink * sink = NULL;
  char * destination = NULL;
  char * path = NULL;
  char * pathname = NULL;

  const char * default_format = "matroska";
  const AVOutputFormat * ofmt = NULL;

  int status = 0;

  if ( !args || !args->source || !args->destination || !*args->destination ) {
    PDBG("Invalid args");
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( !(sink = create_object(sizeof(*sink), ffobjtype_sink, args->name, &iface)) ) {
    PDBG("create_object(%s) fails", args->name);
    status = AVERROR(ENOMEM);
    goto end;
  }

  destination = strpattexpand(args->destination);

  if ( looks_like_url(destination) ) {
    pathname = strdup(destination);
  }
  else if ( !(pathname = strmkpath("%s/%s", ffsrv.db.root, destination)) ) {
    status = AVERROR(errno);
  }
  else if ( !create_path(DEFAULT_MKDIR_MODE, path = getdirname(pathname)) ) {
    status = AVERROR(errno);
  }

  if ( status ) {
    goto end;
  }

  sink->source = args->source;
  sink->destination = strdup(pathname);
  if ( (ofmt = av_guess_format(NULL, pathname, NULL)) ) {
    sink->format = strdup(ofmt->name);
  }
  else {
    sink->format = strdup(default_format);
  }


  add_object_ref(&sink->base);
  if ( !co_schedule(sink_thread, sink, SINK_THREAD_STACK_SIZE) ) {
    status = AVERROR(errno);
    PDBG("[%s] co_schedule(sink_thread) fails: %s", objname(sink), strerror(errno));
    release_object(&sink->base);
    goto end;
  }


end:

  free(path);
  free(pathname);
  free(destination);

  if ( status && sink ) {
    release_object(&sink->base);
    sink = NULL;
  }

  *obj = (void*)sink;

  return status;
}
