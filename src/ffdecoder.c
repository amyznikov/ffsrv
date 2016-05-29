/*
 * ffdecoder.c
 *
 *  Created on: Apr 25, 2016
 *      Author: amyznikov
 */

#include "ffdecoder.h"
#include "ffgop.h"
#include "debug.h"


#define DECODER_THREAD_STACK_SIZE (1024*1024)
#define DECODER_FFGOP_SIZE        512

#define objname(obj) \
    (obj)->base.name


struct ostream {
  struct ffstream base;
  int64_t ppts;
  AVCodecContext * codec;
};

struct ffdec {
  struct ffobject base;

  struct ffobject * source;
  struct ffgop gop;

  const struct ffstream * const * iss; // [nb_streams]
  struct ostream ** oss; // [nb_streams]
  uint nb_streams;
};


static int alloc_streams(struct ffdec * dec, uint nb_streams)
{
  int status = 0;

  if ( !(dec->oss = ffmpeg_alloc_ptr_array(nb_streams, sizeof(struct ostream))) ) {
    status = AVERROR(ENOMEM);
  }

  return status;
}

static void free_streams(struct ffdec * dec)
{
  if ( dec->oss ) {
    for ( uint i = 0; i < dec->nb_streams; ++i ) {
      if ( dec->oss[i] ) {
        ffstream_cleanup(&dec->oss[i]->base);
        if ( dec->oss[i]->codec ) {
          if ( avcodec_is_open(dec->oss[i]->codec) ) {
            avcodec_close(dec->oss[i]->codec);
          }
          avcodec_free_context(&dec->oss[i]->codec);
        }
      }
    }
    ffmpeg_free_ptr_array(&dec->oss, dec->nb_streams);
  }
}

static void on_destroy_decoder(void * ffobject)
{
  struct ffdec * dec = ffobject;

  ffgop_cleanup(&dec->gop);
  free_streams(dec);
  release_object(dec->source);

}

static int get_decoded_streams(void * ffobject, const ffstream * const ** streams, uint * nb_streams)
{
  struct ffdec * dec = ffobject;
  *streams = (const ffstream * const *)dec->oss;
  *nb_streams = dec->nb_streams;
  return 0;
}

static struct ffgop * get_decoded_gop(void * ffobject)
{
  struct ffdec * dec = ffobject;
  return &dec->gop;
}

static int start_decoding(struct ffdec * dec, struct ffobject * source, const char * options)
{
  AVDictionary * opts = NULL;
  AVDictionary * codec_opts = NULL;
  const struct ffstream * is;
  struct ostream * os;
  const AVCodec * codec;

  int status;

  if ( (status = get_streams(source, &dec->iss, &dec->nb_streams)) ) {
    goto end;
  }

  if ( (status = alloc_streams(dec, dec->nb_streams)) ) {
    goto end;
  }

  if ( (status = ffgop_init(&dec->gop, DECODER_FFGOP_SIZE, ffgop_frm, NULL, 0)) ) {
    goto end;
  }


  if ( options && *options && (status = av_dict_parse_string(&opts, options, " \t", " \t", 0)) ) {
    PDBG("[%s] av_dict_parse_string(%s) fails: %s", objname(dec), options, av_err2str(status));
    status = 0; /* ignore this error */
  }

  for ( uint i = 0; i < dec->nb_streams; ++i ) {

    is = dec->iss[i];
    os = dec->oss[i];

    os->ppts = AV_NOPTS_VALUE;

    if ( !is->codecpar->codec_id ) {
      PDBG("[%s] FIXME : NOT IMPLEMENTED codecpar->codec_id BRANCH FOR STREAM %u", objname(dec), i);
      exit(1);
    }
    else {

      if ( !(codec = avcodec_find_decoder(is->codecpar->codec_id)) ) {
        PDBG("[%s] avcodec_find_decoder(st=%u codec_id=%d) fails", objname(dec), i, is->codecpar->codec_id);
        status = AVERROR_DECODER_NOT_FOUND;
        goto end;
      }

      if ( (status = ffmpeg_filter_codec_opts(opts, codec, AV_OPT_FLAG_DECODING_PARAM, &codec_opts)) ) {
        PDBG("[%s] ffmpeg_filter_codec_opts() fails", objname(dec));
        break;
      }

      if ( !(os->codec = avcodec_alloc_context3(codec)) ) {
        PDBG("[%s] avcodec_alloc_context3(st=%u codec_id=%d) fails", objname(dec), i, codec->id);
        status = AVERROR(ENOMEM);
        goto end;
      }

      if ( (status = avcodec_parameters_to_context(os->codec, is->codecpar)) < 0 ) {
        PDBG("[%s] avcodec_parameters_to_context(st=%u codec=%s) fails: %s", objname(dec), i, codec->name,av_err2str(status));
        goto end;
      }

      if ( (status = avcodec_open2(os->codec, codec, &codec_opts)) < 0 ) {
        PDBG("[%s] avcodec_open2(st=%u codec=%s) fails: %s", objname(dec), i, codec->name, av_err2str(status));
        goto end;
      }

      if ( !(os->base.codecpar = avcodec_parameters_alloc()) ) {
        PDBG("[%s] avcodec_parameters_alloc(st=%u) fails", objname(dec), i);
        status = AVERROR(ENOMEM);
        goto end;
      }

      if ( (status = avcodec_parameters_from_context(os->base.codecpar, os->codec)) < 0 ) {
        PDBG("[%s] avcodec_parameters_from_context(st=%u) fails: %s", objname(dec), i, av_err2str(status));
        goto end;
      }

      if ( (status = ffstream_copy(&os->base, is, false, true)) ) {
        PDBG("[%s] ffstream_copy(st=%u) fails: %s", objname(dec), i, av_err2str(status));
        goto end;
      }

    }

    if ( codec_opts ) {
      av_dict_free(&codec_opts);
    }
  }

  dec->source = source;
  ffgop_set_streams(&dec->gop, (const ffstream**)dec->oss, dec->nb_streams);

end :

  if ( status ) {
    free_streams(dec);
  }

  if ( codec_opts ) {
    av_dict_free(&codec_opts);
  }

  if ( opts ){
    av_dict_free(&opts);
  }

  return status;
}


static void decoder_thread(void * arg)
{
  struct ffdec * dec = arg;

  AVPacket pkt;
  AVFrame * frame;

  struct ostream * os;
  int stidx;

  int gotframe;

  struct ffgop * igop = NULL;
  struct ffgoplistener * gl = NULL;

  int status = 0;


  if ( !(frame = av_frame_alloc()) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(igop = get_gop(dec->source)) ) {
    status = AVERROR(EFAULT);
    goto end;
  }

  if ( (status = ffgop_create_listener(igop, &gl)) ) {
    goto end;
  }

  av_init_packet(&pkt);
  pkt.data = NULL, pkt.size = 0;

  while ( status >= 0 && dec->base.refs > 1 ) {

    // const struct ffstream * is;

    if ( (status = ffgop_get_pkt(gl, &pkt)) ) {
      break;
    }


    stidx = pkt.stream_index; // todo: use stream maps

    os = dec->oss[stidx];
//    is = dec->iss[stidx];

//    PDBG("[%s] IPKT [st=%d]%c %s pts=%6s dts=%6s  size=%5d itb=%s otb=%s ctb=%s", objname(dec), stidx,
//        gl->skip_video ? '-' : '*', av_get_media_type_string(is->codecpar->codec_type), av_ts2str(pkt.pts),
//        av_ts2str(pkt.dts), pkt.size, av_tb2str(is->time_base), av_tb2str(os->base.time_base),
//        av_tb2str(os->codec->time_base));

    while ( pkt.size > 0 ) {

      frame->pts = AV_NOPTS_VALUE;
      gotframe = false;

      if ( (status = ffmpeg_decode_packet(os->codec, &pkt, frame, &gotframe)) < 0 ) {
        PDBG("[%s] ffmpeg_decode_frame(st=%d) fails: %s", objname(dec), stidx, av_err2str(status));
        break;
      }

      if ( os->codec->codec_type == AVMEDIA_TYPE_VIDEO ) {
        pkt.size = 0;
      }
      else {
        pkt.size -= status;
        pkt.data += status;
      }

      if ( gotframe ) {

        if ( (frame->pts = av_frame_get_best_effort_timestamp(frame)) <= os->ppts ) {
          PDBG("[%s] out of order: st=%d pts=%s ppts=%s", objname(dec), stidx, av_ts2str(frame->pts), av_ts2str(os->ppts));
        }

        os->ppts = frame->pts;
        frame->opaque = (void*)(ssize_t)(stidx);


        if ( (status = ffgop_put_frm(&dec->gop, frame)) ) {
          PDBG("[%s] ffgop_put_frm() fails: st=%d %s", objname(dec), stidx, av_err2str(status));
          break;
        }
      }
    }

    av_packet_unref(&pkt);
  }

end:

  ffgop_put_eof(&dec->gop, status);

  ffgop_delete_listener(&gl);

  av_frame_free(&frame);

  release_object(&dec->base);
}


int ff_create_decoder(struct ffobject ** obj, const struct ff_create_decoder_args * args)
{
  static const struct ff_object_iface iface = {
    .on_add_ref = NULL,
    .on_destroy = on_destroy_decoder,
    .get_streams = get_decoded_streams,
    .get_gop = get_decoded_gop,
  };

  struct ffdec * dec = NULL;

  int status = 0;

  if ( !args || !args->name || !args->source ) {
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( !(dec = ff_create_object(sizeof(struct ffdec), object_type_decoder, args->name, &iface)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

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
