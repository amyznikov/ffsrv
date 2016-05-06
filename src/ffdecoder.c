/*
 * ffdecoder.c
 *
 *  Created on: Apr 25, 2016
 *      Author: amyznikov
 */

#include "ffdecoder.h"
#include "debug.h"

#define DECODER_THREAD_STACK_SIZE (1024*1024)

#define objname(obj) \
    (obj)->base.name


struct ffdec {
  struct ffobject base;

  struct ffobject * source;
  //struct ffgoplistener * gl;
  struct ffgop gop;

  AVFormatContext * ic;
};


static void on_release_decoder(void * ffobject)
{
  struct ffdec * dec = ffobject;

  // ffgop_delete_listener(&dec->gl);
  ffgop_cleanup(&dec->gop);
  release_object(dec->source);

}

static int get_decoded_format_context(void * ffobject, struct AVFormatContext ** cc)
{
  struct ffdec * dec = ffobject;
  *cc = dec->ic;
  return 0;
}

static struct ffgop * get_decoded_gop(void * ffobject)
{
  struct ffdec * dec = ffobject;
  return &dec->gop;
}

static int start_decoding(struct ffdec * dec, struct ffobject * source, const char * opts)
{
  AVDictionary * dic = NULL;
  AVFormatContext * ic = NULL;
  AVStream * st;
  const AVCodec * codec;

  int status;

  if ( (status = get_format_context(source, &ic)) ) {
    goto end;
  }

  if ( opts && *opts && (status = ffmpeg_parse_opts(opts, &dic)) ) {
    PDBG("[%s] ffmpeg_parse_opts(%s) fails: %s", objname(dec), opts, av_err2str(status));
    status = 0; /* ignore this error */
  }

  for ( uint i = 0; i < ic->nb_streams; ++i ) {

    if ( (st = ic->streams[i])->codec ) {

      if ( !(codec = avcodec_find_decoder(st->codec->codec_id)) ) {
        PDBG("[%s] avcodec_find_decoder(st=%d codec_id=%d) fails", objname(dec), i, st->codec->codec_id);
        status = AVERROR_DECODER_NOT_FOUND;
        goto end;
      }

      if ( (status = avcodec_open2(st->codec, codec, &dic)) < 0 ) {
        PDBG("[%s] avcodec_open2(st=%d codec_id=%d) fails: %s", objname(dec), i, st->codec->codec_id, av_err2str(status));
        goto end;
      }
    }

  }

  dec->source = source;
  dec->ic = ic;

end :

  if ( status && ic ) {
    for ( uint i = 0; i < ic->nb_streams; ++i ) {
      if ( (st = ic->streams[i])->codec && avcodec_is_open(st->codec) ) {
        avcodec_close(st->codec);
      }
    }
  }

  if ( dic ){
    av_dict_free(&dic);
  }

  return status;
}


static void stop_decoding(struct ffdec * dec)
{
  if ( dec->ic ) {
    for ( uint i = 0; i < dec->ic->nb_streams; ++i ) {
      if ( dec->ic->streams[i]->codec && avcodec_is_open(dec->ic->streams[i]->codec) ) {
        avcodec_close(dec->ic->streams[i]->codec);
      }
    }
  }
}

static void decoder_thread(void * arg)
{
  struct ffdec * dec = arg;

  AVPacket pkt;
  AVFrame * frame;
  AVStream * st;

  int frames_decoded = 0;
  int gotframe;

  int64_t * ppts;

  struct ffgop * igop = NULL;
  struct ffgoplistener * gl = NULL;

  int status = 0;


  if ( !(frame = av_frame_alloc()) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(igop = ff_get_gop(dec->source)) ) {
    status = AVERROR(EFAULT);
    goto end;
  }

  if ( (status = ffgop_create_listener(igop, &gl)) ) {
    goto end;
  }


  ppts = alloca(dec->ic->nb_streams * sizeof(*ppts));
  for ( uint i = 0; i < dec->ic->nb_streams; ++i ) {
    ppts[i] = AV_NOPTS_VALUE;
  }

  av_init_packet(&pkt);
  pkt.data = NULL, pkt.size = 0;

  while ( status >= 0 && dec->base.refs > 1 ) {

    if ( (status = ffgop_get_pkt(gl, &pkt)) ) {
      break;
    }

    st = dec->ic->streams[pkt.stream_index];

    while ( pkt.size > 0 ) {

      frame->pts = AV_NOPTS_VALUE;
      gotframe = false;

      if ( (status = ffmpeg_decode_frame(st->codec, &pkt, frame, &gotframe)) < 0 ) {
        PDBG("[%s] ffmpeg_decode_frame(st=%d) fails: %s", objname(dec), pkt.stream_index, av_err2str(status));
        break;
      }

      if ( gotframe ) {
        if ( (frame->pts = av_frame_get_best_effort_timestamp(frame)) <= ppts[pkt.stream_index] ) {
          PDBG("[%s] out of order: st=%d pts=%"PRId64" ppts=%"PRId64"", objname(dec), pkt.stream_index, frame->pts,
              ppts[pkt.stream_index]);
        }

        ppts[pkt.stream_index] = frame->pts;
        ++frames_decoded;

        frame->opaque = (void*)(ssize_t)(pkt.stream_index);
        if ( (status = ffgop_put_frm(&dec->gop, frame, st->codec->codec_type)) ) {
          PDBG("[%s] ffgop_put_frm() fails: st=%d %s", objname(dec), pkt.stream_index, av_err2str(status));
          break;
        }
      }

      if ( st->codec->codec_type == AVMEDIA_TYPE_VIDEO ) {
        pkt.size = 0;
      }
      else {
        pkt.size -= status;
        pkt.data += status;
      }
    }

    av_packet_unref(&pkt);
  }

end:

  ffgop_put_eof(&dec->gop, status);

  ffgop_delete_listener(&gl);

  av_frame_free(&frame);

  stop_decoding(dec);

  release_object(&dec->base);
}


int ff_create_decoder(struct ffobject ** obj, const struct ff_create_decoder_args * args)
{
  static const struct ff_object_iface iface = {
    .on_add_ref = NULL,
    .on_release = on_release_decoder,
    .get_format_context = get_decoded_format_context,
    .get_gop = get_decoded_gop,
  };

  struct ffdec * dec = NULL;
  //struct ffgop * igop = NULL;

  int status = 0;

  if ( !args || !args->name || !args->source ) {
    status = AVERROR(EINVAL);
    goto end;
  }

//  if ( !(igop = ff_get_gop(args->source)) ) {
//    status = AVERROR(EINVAL);
//    goto end;
//  }

  if ( !(dec = ff_create_object(sizeof(struct ffdec), object_type_decoder, args->name, &iface)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }


  if ( (status = ffgop_init(&dec->gop, 32, ffgop_frm)) ) {
    goto end;
  }


//  if ( (status = ffgop_create_listener(igop, &dec->gl)) ) {
//    goto end;
//  }


  if ( (status = start_decoding(dec, args->source, "")) ) {
    goto end;
  }


  add_object_ref(&dec->base);

  if ( !co_schedule(decoder_thread, dec, DECODER_THREAD_STACK_SIZE) ) {
    status = AVERROR(errno);
    PDBG("[%s] co_schedule(decoder_thread) fails: %s", objname(dec), strerror(errno));
    release_object(&dec->base);
    goto end;
  }


end:

  if ( status && dec ) {
    release_object(&dec->base);
    dec = NULL;
  }


  *obj = (void*)dec;

  return status;
}
