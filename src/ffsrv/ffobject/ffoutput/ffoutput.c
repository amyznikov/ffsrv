/*
 * ffoutput.c
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#include "ffoutput.h"
#include "ffgop.h"
#include "debug.h"

#define TCP_OUTPUT_IO_BUF_SIZE    (32*1024)
#define RTP_OUTPUT_IO_BUF_SIZE    1472

struct ffoutput {
  struct ffobject * source;

  struct AVOutputFormat * oformat;

  const struct ffstream * const * iss;
  uint nb_streams;

  struct ffgoplistener * gl;

  void * cookie;
  int  (*sendpkt)(void * cookie, int stream_index, uint8_t * buf, int buf_size);
  bool (*getoutspc)(void * cookie, int * outspc, int * maxspc);

  enum {
    output_type_tcp = 0,
    output_type_rtp = 1,
  } output_type;


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
      uint current_stream_index;
    } rtp;
  };

  bool running :1, finish :1;
};



static int ffoutput_send_tcp_pkt(void * opaque, uint8_t * buf, int buf_size)
{
  struct ffoutput * output = opaque;
  return output->sendpkt(output->cookie, -1, buf, buf_size);
}

static int ffoutput_send_rtp_pkt(void * opaque, uint8_t * buf, int buf_size)
{
  struct ffoutput * output = opaque;
  return output->sendpkt(output->cookie, output->rtp.current_stream_index, buf, buf_size);
}


static int ff_create_output_context(struct ffoutput * output)
{
  int status = 0;

  if ( output->output_type == output_type_tcp ) {

    if ( !(output->tcp.iobuf = av_malloc(TCP_OUTPUT_IO_BUF_SIZE)) ) {
      PDBG("av_malloc(iobuf) fails");
      status = AVERROR(ENOMEM);
      goto end;
    }

    if ( (status = avformat_alloc_output_context2(&output->tcp.oc, output->oformat, NULL, NULL)) ) {
      PDBG("avformat_alloc_output_context2() fails: %s", av_err2str(status));
      goto end;
    }

    output->tcp.oc->flags |= AVFMT_FLAG_CUSTOM_IO;
    output->tcp.oc->pb = avio_alloc_context(output->tcp.iobuf, TCP_OUTPUT_IO_BUF_SIZE, true, output, NULL,
        ffoutput_send_tcp_pkt, NULL);
    if ( !output->tcp.oc->pb ) {
      status = AVERROR(ENOMEM);
      goto end;
    }

    // av_dict_set(&output->tcp.oc->metadata, "title", "No Title", 0);

    if ( (status = ffstreams_to_context(output->iss, output->nb_streams, output->tcp.oc)) ) {
      PDBG("ffstreams_to_context() fails: %s", av_err2str(status));
      goto end;
    }

  }
  else {

    output->rtp.oc = av_malloc_array(output->nb_streams, sizeof(AVFormatContext*));
    output->rtp.iobuf = av_malloc_array(output->nb_streams, sizeof(uint8_t*));

    PDBG("COPY STREAMS output->format=%p %s", output->oformat, output->oformat ? output->oformat->name : "");

    for ( uint i = 0; i < output->nb_streams; ++i ) {

      if ( (status = avformat_alloc_output_context2(&output->rtp.oc[i], output->oformat, "rtp", NULL)) ) {
        PDBG("avformat_alloc_output_context2() fails: %s", av_err2str(status));
        break;
      }

      output->rtp.oc[i]->flags |= AVFMT_FLAG_CUSTOM_IO;
      // av_dict_set(&output->rtp.oc[i]->metadata, "title", "No Title", 0);

      if ( !(output->rtp.iobuf[i] = av_malloc(RTP_OUTPUT_IO_BUF_SIZE)) ) {
        PDBG("av_malloc(iobuf) fails");
        status = AVERROR(ENOMEM);
        break;
      }

      output->rtp.oc[i]->pb = avio_alloc_context(output->rtp.iobuf[i], RTP_OUTPUT_IO_BUF_SIZE, true, output, NULL,
          ffoutput_send_rtp_pkt, NULL);
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

      if ( (status = ffstream_to_context(output->iss[i], output->rtp.oc[i]->streams[0])) ) {
        PDBG("ffstream_to_context() fails: %s", av_err2str(status));
        break;
      }

      output->rtp.oc[i]->streams[0]->index = 0;
    }

    PDBG("COPY FINISH");
    if ( status ) {
      goto end;
    }
  }

end :

  return status;
}

static void ff_destroy_output_context(struct ffoutput * output)
{
  if ( output ) {
    if ( output->output_type == output_type_tcp ) {
      if ( output->tcp.oc ) {
        av_freep(&output->tcp.oc->pb->buffer);
        av_freep(&output->tcp.oc->pb);
        avformat_free_context(output->tcp.oc);
        output->tcp.oc = NULL;
      }
    }
    else if ( output->output_type == output_type_rtp ) {
      for ( uint i = 0; i < output->nb_streams; ++i ) {
        if ( output->rtp.oc[i] ) {
          av_freep(&output->rtp.oc[i]->pb->buffer);
          av_freep(&output->rtp.oc[i]->pb);
          avformat_free_context(output->rtp.oc[i]);
          output->rtp.oc[i] = NULL;
        }
      }
    }
  }
}


int ff_create_output(struct ffoutput ** pps, const struct ff_create_output_args * args)
{
  struct ffoutput * output = NULL;
  const char * output_format_name = "matroska";
  AVOutputFormat * format = NULL;
  bool rtp = false;


  int status = 0;

  if ( !args || !args->sendpkt || !args->source ) {
    PDBG("Invalid args: args=%p args->sendpkt=%p args->source=%p", args, args? args->sendpkt: NULL,
        args?args->source: NULL);
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( !args->source->iface->get_streams || !args->source->iface->get_gop ) {
    PDBG("Invalid args->iface: get_streams=%p get_gop=%p", args->source->iface->get_streams, args->source->iface->get_gop);
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

  if ( strcasecmp(output_format_name, "rtp") == 0 ) {
    rtp = true;
  }

  if ( !(output = calloc(1, sizeof(*output))) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( (status = get_streams(args->source, &output->iss, &output->nb_streams)) ) {
    PDBG("get_streams() fails: %s", av_err2str(status));
    goto end;
  }


  output->source = args->source;
  output->cookie = args->cookie;
  output->sendpkt = args->sendpkt;
  output->getoutspc = args->getoutspc;
  output->oformat = format;
  output->output_type = rtp ? output_type_rtp : output_type_tcp;
  output->finish = false;

end:

  if ( status && output ) {
    free(output), output = NULL;
  }

  *pps = output;

  return status;
}

void ff_delete_output(struct ffoutput ** output)
{
  if ( output && *output ) {

    ffgop_delete_listener(&(*output)->gl);
    ff_destroy_output_context(*output);
    release_object((*output)->source);
    free(*output);
    *output = NULL;
  }
}



const char * ff_get_output_mime_type(struct ffoutput * output)
{
  const char * mime_type = NULL;
  if ( output->oformat->mime_type && *output->oformat->mime_type ) {
    mime_type = output->oformat->mime_type;
  }
  else {
    mime_type = "application/x-octet-stream";
  }
  return mime_type;
}


int ff_get_output_nb_streams(struct ffoutput * output, uint * nb_streams)
{
  *nb_streams = output->nb_streams;
  return 0;
}


int ff_get_output_sdp(struct ffoutput * output, char * sdp, int sdpmax)
{
  AVFormatContext * oc = NULL;

  int status = 0;

  if ( (status = avformat_alloc_output_context2(&oc, output->oformat, NULL, NULL)) ) {
    PDBG("avformat_alloc_output_context2() fails: %s", av_err2str(status));
    goto end;
  }


  if ( (status = ffstreams_to_context(output->iss, output->nb_streams, oc)) ) {
    PDBG("ffstreams_to_context() fails: %s", av_err2str(status));
    goto end;
  }

//  PDBG("CODECID = %d 0x%0X", oc->streams[0]->codec->codec_id, oc->streams[0]->codec->codec_id);
//  do {
//    int payload_type = ff_rtp_get_payload_type(output->format, oc->streams[0]->codec, 0);
//    PDBG("payload_type=%d", payload_type);
//  } while ( 0 );

  if ( (status = av_sdp_create(&oc, 1, sdp, sdpmax)) ) {
    PDBG("av_sdp_create() fails: %s", av_err2str(status));
    goto end;
  }


end:

  if ( oc ) {
    avformat_free_context(oc);
  }

  return status;
//  (void)(output);
//  (void)(sdp);
//  (void)(sdpmax);
//  return AVERROR(ENOSYS);
}



int ff_run_output_stream(struct ffoutput * output)
{
  AVPacket pkt;
  AVFormatContext * oc;

  struct ffgop * gop = NULL;

  const struct ffstream * is;
  AVStream * os;

  int stidx;
  uint nb_streams;

  int64_t firstdts = AV_NOPTS_VALUE;
  int64_t * tsoffset = NULL;

  bool flush_packets = true;
  bool write_header_ok = false;


  int status = 0;


  av_init_packet(&pkt);
  pkt.data = NULL, pkt.size = 0;


  if ( !output ) {
    status = AVERROR(EINVAL);
    goto end;
  }

  output->running = true;

  if ( !(gop = get_gop(output->source)) || ffgop_get_type(gop) != ffgop_pkt ) {
    PDBG("get_gop() fails");
    status = AVERROR(EINVAL);
    goto end;
  }

  status = ffgop_create_listener(gop, &output->gl, &(struct ffgop_create_listener_args ) {
        .getoutspc = output->getoutspc,
        .cookie = output->cookie
      });

  if ( status ) {
    PDBG("ffgop_create_listener() fails: %s", av_err2str(status));
    goto end;
  }



  nb_streams = output->nb_streams;



  /* Create output context
   * */
  if ( output->output_type == output_type_tcp ) {
    if ( !output->tcp.oc && (status = ff_create_output_context(output)) ) {
      PDBG("ff_create_output_context() fails: %s", av_err2str(status));
      goto end;
    }
  }
  else if ( output->output_type == output_type_rtp ) {
    if ( !output->rtp.oc && (status = ff_create_output_context(output)) ) {
      PDBG("ff_create_output_context() fails: %s", av_err2str(status));
      goto end;
    }
  }
  else {
    PDBG("BUG: invalid output type %d", output->output_type);
    status = AVERROR_BUG;
    goto end;
  }

  tsoffset = alloca(nb_streams * sizeof(*tsoffset));
  for ( uint i = 0; i < nb_streams; ++i ) {
    tsoffset[i] = AV_NOPTS_VALUE;
  }


  PDBG("WRITE HEADER");

  if ( output->output_type == output_type_tcp ) {
    output->tcp.oc->flush_packets = 1;
    if ( (status = avformat_write_header(output->tcp.oc, NULL)) >= 0 ) {
      write_header_ok = true;
    }
    else {
      PDBG("avformat_write_header() fails: %s", av_err2str(status));
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
        PDBG("avformat_write_header(st=%u) fails: %s", i, av_err2str(status));
        goto end;
      }
    }
  }

  PDBG("WRITE HEADER OK");

  /* Main loop
   * */

  // ffgop_enable_skip_video(output->gl, true);

  while ( status >= 0 && !output->finish ) {

    if ( (status = ffgop_get_pkt(output->gl, &pkt)) ) {
      PDBG("ffgop_get_pkt() fails: %s", av_err2str(status));
      continue;
    }


    stidx = pkt.stream_index;
    is = output->iss[stidx];

    if ( output->output_type == output_type_tcp ) {
      oc = output->tcp.oc;
      os = oc->streams[stidx];
    }
    else {
      oc = output->rtp.oc[stidx];
      os = oc->streams[0];
    }

//    {
//      int64_t upts = av_rescale_ts(pkt.pts, is->time_base, (AVRational){1, 1000});
//      int64_t udts = av_rescale_ts(pkt.dts, is->time_base, (AVRational){1, 1000});
//      PDBG("IPKT [st=%2d] pts=%s dts=%s key=%d\t upts=%s udts=%s", stidx,
//          av_ts2str(pkt.pts), av_ts2str(pkt.dts), (pkt.flags & AV_PKT_FLAG_KEY), av_ts2str(upts), av_ts2str(udts));
//    }

    if ( firstdts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE ) {

      const AVRational utb = (AVRational ) { 1, 1000000 };

      firstdts = av_rescale_ts(pkt.dts, is->time_base, utb);

      for ( uint i = 0; i < output->nb_streams; ++i ) {
        tsoffset[i] = av_rescale_ts(firstdts, utb, output->iss[i]->time_base);
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

    if ( output->output_type == output_type_rtp ) {
      output->rtp.current_stream_index = stidx;
      pkt.stream_index = 0;
      status = av_write_frame(oc, &pkt);
    }
    else if ( nb_streams > 1 ) {
      status = av_interleaved_write_frame(oc, &pkt);
    }
    else if ( (status = av_write_frame(oc, &pkt)) == 0 && flush_packets ) {
      status = av_write_frame(oc, NULL);
    }

    if ( status < 0 ) {
      PDBG("write frame fails: %s", av_err2str(status));
    }

    av_packet_unref(&pkt);

    co_yield();
  }

end : ;

  if ( write_header_ok ) {
    if ( output->output_type == output_type_tcp ) {
      av_write_trailer(output->tcp.oc);
    }
    else {
      for ( uint i = 0; i < nb_streams; ++i ) {
        av_write_trailer(output->rtp.oc[i]);
      }
    }
  }

  if ( output ) {
    ff_destroy_output_context(output);
    output->running = false;
  }

  return status;
}


