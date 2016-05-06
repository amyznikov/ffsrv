/*
 * ffoutput.c
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#include "libffms.h"
#include "ffoutput.h"
#include "coscheduler.h"
#include "debug.h"

#define TCP_OUTPUT_IO_BUF_SIZE    (32*1024)
#define RTP_OUTPUT_IO_BUF_SIZE    1472

struct ffoutput {
  struct ffobject * source;

  struct AVOutputFormat * oformat;
  AVFormatContext * ic;
  struct ffgoplistener * gl;

  void * cookie;
  int (*sendpkt)(void * cookie, int stream_index, uint8_t * buf, int buf_size);

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
      uint nb_streams;
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

static inline bool tbequal(const AVRational * tb1, const AVRational * tb2)
{
  return tb1->num == tb2->num && tb1->den == tb2->den;
}


static int ff_create_output_context(struct ffoutput * output)
{
  int status = 0;

  // get source context first
  AVFormatContext * ic = output->ic;


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

    av_dict_set(&output->tcp.oc->metadata, "title", "No Title", 0);

    if ( (status = ffmpeg_copy_streams(ic, output->tcp.oc)) ) {
      PDBG("ffmpeg_copy_stream() fails: %s", av_err2str(status));
      goto end;
    }

  }
  else {

    output->rtp.nb_streams = ic->nb_streams;
    output->rtp.oc = av_malloc_array(output->rtp.nb_streams, sizeof(AVFormatContext*));
    output->rtp.iobuf = av_malloc_array(output->rtp.nb_streams, sizeof(uint8_t*));

    PDBG("COPY STREAMS output->format=%p %s", output->oformat, output->oformat ? output->oformat->name : "");

    for ( uint i = 0; i < output->rtp.nb_streams; ++i ) {

      if ( (status = avformat_alloc_output_context2(&output->rtp.oc[i], output->oformat, "rtp", NULL)) ) {
        PDBG("avformat_alloc_output_context2() fails: %s", av_err2str(status));
        break;
      }

      output->rtp.oc[i]->flags |= AVFMT_FLAG_CUSTOM_IO;
      av_dict_set(&output->rtp.oc[i]->metadata, "title", "No Title", 0);

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

      if ( (status = ffmpeg_copy_stream(ic->streams[i], output->rtp.oc[i]->streams[0], output->oformat)) ) {
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

end :

  return status;
}

static void ff_destroy_output_context(struct ffoutput * output)
{
  if ( output->output_type == output_type_tcp ) {
    if ( output->tcp.oc ) {
      av_freep(&output->tcp.oc->pb->buffer);
      av_freep(&output->tcp.oc->pb);

      PDBG("C ffmpeg_close_output(&oc=%p)", output->tcp.oc);
      ffmpeg_close_output(&output->tcp.oc);
      PDBG("R ffmpeg_close_output(&oc=%p)", output->tcp.oc);
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


int ff_create_output(struct ffoutput ** pps, const struct ff_create_output_args * args)
{
  struct ffoutput * output = NULL;
  const char * output_format_name = "matroska";
  AVOutputFormat * format = NULL;
  AVFormatContext * ic = NULL;
  struct ffgop * gop = NULL;
  bool rtp = false;


  int status = 0;

  if ( !args || !args->sendpkt || !args->source ) {
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( !args->source->iface->get_format_context || !args->source->iface->get_gop ) {
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( !(gop = ff_get_gop(args->source)) || ffgop_get_type(gop) != ffgop_pkt ) {
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

  if ( (status = get_format_context(args->source, &ic)) ) {
    PDBG("ff_get_format_context() fails: %s", av_err2str(status));
    goto end;
  }

  if ( !(output = calloc(1, sizeof(*output))) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  output->source = args->source;
  output->cookie = args->cookie;
  output->sendpkt = args->sendpkt;
  output->oformat = format;
  output->ic = ic;
  output->output_type = rtp ? output_type_rtp : output_type_tcp;
  output->finish = false;
  ffgop_create_listener(gop, &output->gl);

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
  *nb_streams = output->ic->nb_streams;
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

  if ( (status = ffmpeg_copy_streams(output->ic, oc)) ) {
    PDBG("ffmpeg_copy_streams() fails: %s", av_err2str(status));
    goto end;
  }

//  PDBG("CODECID = %d 0x%0X", oc->streams[0]->codec->codec_id, oc->streams[0]->codec->codec_id);
//  do {
//    int payload_type = ff_rtp_get_payload_type(output->format, oc->streams[0]->codec, 0);
//    PDBG("payload_type=%d", payload_type);
//  } while ( 0 );

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


int ff_run_output_stream(struct ffoutput * output)
{
  AVPacket pkt;
  AVFormatContext * ic;
  AVFormatContext * oc;
  AVStream * os;
  int stream_index;
  uint nb_streams;


  int64_t * dts0 = NULL;
  int64_t * ppts = NULL;

  bool flush_packets = true;
  bool write_header_ok = false;
  bool gotkeyframe = false;

  int status = 0;

  ic = output->ic;

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

  while ( status >= 0 && !output->finish ) {

    if ( (status = ffgop_get_pkt(output->gl, &pkt)) ) {
      PDBG("ffgop_get_pkt() fails: %s", av_err2str(status));
      continue;
    }

    stream_index = pkt.stream_index;
    //PDBG("st=%d", stream_index);

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

    //PDBG("st=%d pts=%"PRId64" dts=%"PRId64" key=%d", stream_index, pkt.pts, pkt.dts, gotkeyframe);

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

      if ( !tbequal(&ic->streams[stream_index]->time_base, &os->time_base) ) {
        ffmpeg_rescale_timestamps(&pkt, ic->streams[stream_index]->time_base,
            os->time_base);
      }

      ppts[stream_index] = pkt.pts;
      // PDBG("[%d] opts=%"PRId64" odts=%"PRId64" key=%d", pkt.stream_index, pkt.pts, pkt.dts, pkt.flags & AV_PKT_FLAG_KEY);

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
        PDBG("write frame fails: %s", av_err2str(status));
      }
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
        if ( av_write_trailer(output->rtp.oc[i]) != 0 ) {
          break;
        }
      }
    }
  }

  output->running = false;

  return status;
}


