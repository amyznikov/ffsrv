/*
 * ffencoder.c
 *
 *  Created on: Apr 25, 2016
 *      Author: amyznikov
 */

#include "ffencoder.h"
#include "ffgop.h"
#include "debug.h"

#define ENCODER_THREAD_STACK_SIZE               (1024*1024)
#define ENCODER_TIME_BASE                       (AVRational){1,1000}
#define ENCODER_MAX_AUDIO_SAMPLES_PER_FRAME     (8*1024)
#define ENCODER_FFGOP_SIZE                      1024

#define objname(obj) \
    (obj)->base.name



struct ostream {
  ffstream base;
  AVCodecContext * codec;
  int64_t ppts;

  union {

    struct {
      struct SwsContext * sws;
      AVFrame * sws_frame;
    } video;

    struct {
      struct SwrContext * swr;
      AVFrame * swr_frame, * tmp_frame;
    } audio;
  };

};

struct ffenc {
  struct ffobject base;

  struct ffobject * source;
  struct ffgop gop;

  const struct ffstream * const * iss;
  struct ostream ** oss;
  uint nb_streams;

  int64_t t0, forced_key_frames_interval;
};



static int alloc_streams(struct ffenc * enc, uint nb_streams)
{
  int status = 0;

  if ( !(enc->oss = ffmpeg_alloc_ptr_array(nb_streams, sizeof(struct ostream))) ) {
    status = AVERROR(ENOMEM);
  }

  return status;
}

static void free_streams(struct ffenc * enc)
{
  if ( enc->oss ) {
    for ( uint i = 0; i < enc->nb_streams; ++i ) {
      if ( enc->oss[i] ) {
        ffstream_cleanup(&enc->oss[i]->base);
        if ( enc->oss[i]->codec ) {
          if ( avcodec_is_open(enc->oss[i]->codec) ) {
            avcodec_close(enc->oss[i]->codec);
          }
          avcodec_free_context(&enc->oss[i]->codec);
        }
      }
    }
    ffmpeg_free_ptr_array(&enc->oss, enc->nb_streams);
  }
}


static void on_destroy_encoder(void * ffobject)
{
  struct ffenc * enc = ffobject;
  ffgop_cleanup(&enc->gop);
  free_streams(enc);
  release_object(enc->source);
}


static int get_encoded_streams(void * ffobject, const ffstream * const ** streams, uint * nb_streams)
{
  struct ffenc * enc = ffobject;
  *streams = (const ffstream * const *)enc->oss;
  *nb_streams = enc->nb_streams;
  return 0;
}

static struct ffgop * get_encoded_gop(void * ffobject)
{
  struct ffenc * enc = ffobject;
  return &enc->gop;
}

static int start_encoding(struct ffenc * enc, struct ffobject * source, const struct ff_encoder_params * params)
{
  const char * options;
  AVDictionary * opts = NULL;
  AVDictionary * codec_opts = NULL;
  AVDictionaryEntry * e = NULL;


  const struct ffstream * is;
  struct ostream * os;

  AVCodec * codec = NULL;
  const char * codec_name;

  int status = 0;

  if ( params->opts ) {
    options = params->opts;
  }
  else {
    options = "-c:v libx264 -c:a aac3";
  }

  if ( (status = av_dict_parse_string(&opts, options, " \t", " \t", 0)) ) {
    goto end;
  }

  if ( (status = get_streams(source, &enc->iss, &enc->nb_streams)) ) {
    goto end;
  }

  if ( (status = alloc_streams(enc, enc->nb_streams)) ) {
    goto end;
  }

  if ( (status = ffgop_init(&enc->gop, ENCODER_FFGOP_SIZE, ffgop_pkt, NULL, 0)) ) {
    goto end;
  }

  for ( uint i = 0; i < enc->nb_streams; ++i ) {

    is = enc->iss[i];
    os = enc->oss[i];

    codec_name = NULL;

    switch ( is->codecpar->codec_type ) {

      case AVMEDIA_TYPE_VIDEO: {

        int input_width, input_height;
        int output_width, output_height;
        int input_bitrate, output_bitrate;
        enum AVPixelFormat input_fmt, output_fmt;
        int gop_size;


        ///

        if ( (e = av_dict_get(opts, "-c:v", NULL, 0)) ) {
          codec_name = e->value;
        }
        else {
          codec_name = "libx264";
        }

        if ( !(codec = avcodec_find_encoder_by_name(codec_name)) ) {
          PDBG("[%s] avcodec_find_encoder_by_name(%s) fails", objname(enc), codec_name);
          status = AVERROR_ENCODER_NOT_FOUND;
          break;
        }


        ///

        input_width = is->codecpar->width;
        input_height = is->codecpar->height;

        if ( !(e = av_dict_get(opts, "-s", NULL, 0)) ) {
          output_width = input_width;
          output_height = input_height;
        }
        else if ( sscanf(e->value, "%dx%d", &output_width, &output_height) != 2 ) {
          PDBG("[%s] Bad frame size specified: %s", objname(enc), e->value);
          status = AVERROR(EINVAL);
          break;
        }

        if ( input_width < 1 || output_width < 1 || input_height < 1 || output_height < 1 ) {
          PDBG("[%s] Bad frame size: input=%dx%d output=%dx%d", objname(enc), input_width, input_height, output_width, output_height);
          status = AVERROR_INVALIDDATA;
          break;
        }


        ///
        input_fmt = is->codecpar->format;
        if ( !(e = av_dict_get(opts, "-pix_fmt", NULL, 0)) ) {
          if ( (output_fmt = ffmpeg_select_best_format(codec->pix_fmts, input_fmt)) < 0 ) {
            output_fmt = input_fmt;
          }
        }
        else if ( (output_fmt = av_get_pix_fmt(e->value)) == AV_PIX_FMT_NONE ) {
          PDBG("[%s] Bad pixel format specified: %s", objname(enc), e->value);
          status = AVERROR(EINVAL);
          break;
        }

        if ( input_fmt == AV_PIX_FMT_NONE || output_fmt == AV_PIX_FMT_NONE ) {
          PDBG("[%s] Bad pixel format: input_pix_fmt=%d output_pix_fmt=%d", objname(enc), input_fmt, output_fmt);
          status = AVERROR_INVALIDDATA;
          break;
        }


        ///
        input_bitrate = is->codecpar->bit_rate;
        if ( !(e = av_dict_get(opts, "-b:v", NULL, 0)) ) {
          output_bitrate = input_bitrate >= 1000 && input_bitrate < 1000000 ? input_bitrate : 256000;
        }
        else if ( (output_bitrate = (int) av_strtod(e->value, NULL)) < 1000 ) {
          PDBG("[%s] Bad output bitrate specified: %s", objname(enc), e->value);
          status = AVERROR(EINVAL);
          break;
        }


        ///
        if ( !(e = av_dict_get(opts, "-g", NULL, 0)) ) {
          gop_size = 50;
        }
        else if ( sscanf(e->value, "%d", &gop_size) != 1 || gop_size < 1 ) {
          PDBG("[%s] Bad output gop size specified: %s", objname(enc), e->value);
          status = AVERROR(EINVAL);
          break;
        }

        // fixme: qscale (-q:v) profile preset





        /// check if swscale is required
        if ( input_width != output_width || input_height != output_height || input_fmt != output_fmt ) {

          os->video.sws = sws_getContext(input_width, input_height, input_fmt, output_width, output_height,
              output_fmt, SWS_FAST_BILINEAR, NULL, NULL, NULL);

          if ( !os->video.sws ) {
            PDBG("[%s] sws_getContext() fails", objname(enc));
            status = AVERROR_UNKNOWN;
            break;
          }

          if ( (status = ffmpeg_create_video_frame(&os->video.sws_frame, output_fmt, output_width, output_height)) ) {
            PDBG("[%s] ffmpeg_video_frame_create() fails", objname(enc));
            break;
          }
        }




        ///
        if ( (status = ffmpeg_filter_codec_opts(opts, codec, AV_OPT_FLAG_ENCODING_PARAM, &codec_opts)) ) {
          PDBG("[%s] ffmpeg_filter_codec_opts() fails", objname(enc));
          break;
        }

        if ( !(os->codec = avcodec_alloc_context3(codec)) ) {
          PDBG("[%s] avcodec_alloc_context3() fails", objname(enc));
          status = AVERROR(ENOMEM);
          break;
        }

        os->codec->time_base = is->time_base;
        os->codec->pix_fmt = output_fmt;
        os->codec->width = output_width;
        os->codec->height = output_height;
        os->codec->bit_rate = output_bitrate;
        os->codec->gop_size = gop_size;
        os->codec->me_range = 1;
        os->codec->qmin = 1;
        os->codec->qmax = 32;
        os->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
        os->codec->sample_aspect_ratio = is->codecpar->sample_aspect_ratio;


        if ( (status = avcodec_open2(os->codec, codec, &codec_opts)) ) {
          PDBG("[%s] avcodec_open2('%s') fails: %s", objname(enc), codec->name, av_err2str(status));
          break;
        }

        if ( !(os->base.codecpar = avcodec_parameters_alloc())) {
          PDBG("[%s] avcodec_parameters_alloc() fails: %s", objname(enc), av_err2str(status));
          status = AVERROR(ENOMEM);
          break;
        }

        if ( ( status = avcodec_parameters_from_context(os->base.codecpar, os->codec)) < 0 ) {
          PDBG("[%s] avcodec_parameters_from_context('%s') fails: %s", objname(enc), codec->name, av_err2str(status));
          break;
        }

        if ( (status = ffstream_copy(&os->base, is, false, true))) {
          PDBG("[%s] ffstream_copy() fails: %s", objname(enc), av_err2str(status));
          break;
        }

        os->base.time_base = os->codec->time_base;
        os->ppts = AV_NOPTS_VALUE;

      }
      break;



      case AVMEDIA_TYPE_AUDIO: {

        int input_channels, output_channels;
        uint64_t input_channel_layout, output_channel_layout;
        int input_sample_rate, output_sample_rate;
        int input_sample_format, output_sample_format;


        ///
        if ( (e = av_dict_get(opts, "-c:a", NULL, 0)) ) {
          codec_name = e->value;
        }
        else {
          codec_name = "aac";
        }

        if ( !(codec = avcodec_find_encoder_by_name(codec_name)) ) {
          PDBG("[%s] avcodec_find_encoder_by_name(%s) fails", objname(enc), codec_name);
          status = AVERROR_ENCODER_NOT_FOUND;
          break;
        }



        ///
        input_channels = is->codecpar->channels;
        if ( !(e = av_dict_get(opts, "-ac", NULL, 0)) ) {
          output_channels = input_channels;
        }
        else if ( sscanf(e->value, "%d", &output_channels) != 1 || output_channels < 1 ) {
          PDBG("[%s] Bad output audio channels specified: %s", objname(enc), e->value);
          status = AVERROR(EINVAL);
          break;
        }



        ///

        input_channel_layout = is->codecpar->channel_layout;

        if ( !(e = av_dict_get(opts, "-channel_layout", NULL, 0)) ) {
          output_channel_layout = ffmpeg_select_best_channel_layout(codec, input_channel_layout);
        }
        else if ( !(output_channel_layout = av_get_channel_layout(e->value)) ) {
          PDBG("[%s] Bad output audio channel layout specified: %s", objname(enc), e->value);
          status = AVERROR(EINVAL);
          break;
        }
        else if ( !ffmpeg_is_channel_layout_supported(codec, output_channel_layout) ) {
          PDBG("[%s] Specified output audio channel layout %s is not supported by %s", objname(enc), e->value, codec->name);
          status = AVERROR(EINVAL);
          break;
        }




        ///

        input_sample_rate = is->codecpar->sample_rate;

        if ( !(e = av_dict_get(opts, "-ar", NULL, 0)) ) {
          output_sample_rate = ffmpeg_select_samplerate(codec, NULL, input_sample_rate);
        }
        else if ( sscanf(e->value, "%d", &output_sample_rate) != 1 || output_sample_rate < 1 ) {
          PDBG("[%s] Bad output audio sample rate specified: %s", objname(enc), e->value);
          status = AVERROR(EINVAL);
          break;
        }
        else if ( !ffmpeg_is_samplerate_supported(codec, NULL, output_sample_rate) ) {
          PDBG("[%s] Specified output audio sample rate %s is not supported by %s", objname(enc), e->value, codec->name);
          status = AVERROR(EINVAL);
          break;
        }




        ///

        input_sample_format = is->codecpar->format;

        if ( !(e = av_dict_get(opts, "-sample_fmt", NULL, 0)) ) {
          if ( (output_sample_format = ffmpeg_select_best_format(codec->sample_fmts, input_sample_format)) == -1 ) {
            PDBG("[%s] sample format '%s' is not supported by %s", objname(enc),
                av_get_sample_fmt_name(input_sample_format), codec->name);
            status = AVERROR(EINVAL);
            break;
          }
        }
        else {
          if ( (output_sample_format = av_get_sample_fmt(e->value)) == AV_SAMPLE_FMT_NONE ) {
            PDBG("[%s] Bad output sample format specified: %s", objname(enc), e->value);
            status = AVERROR(EINVAL);
            break;
          }
          if ( !ffmpeg_is_format_supported(codec->sample_fmts, output_sample_format) ) {
            PDBG("[%s] specified sample format '%s' is not supported by %s", objname(enc),
                av_get_sample_fmt_name(output_sample_format), codec->name);
            status = AVERROR(EINVAL);
            break;
          }
        }



        /// check if resample is required

        if ( input_sample_rate != output_sample_rate || input_sample_format != output_sample_format
            || input_channels != output_channels ) {

          os->audio.swr = swr_alloc_set_opts(NULL, output_channel_layout, output_sample_format, output_sample_rate,
              input_channel_layout, input_sample_format, input_sample_rate, 0, NULL);

          if ( !os->audio.swr ) {
            PDBG("[%s] swr_alloc_set_opts() fails", objname(enc));
            status = AVERROR(EINVAL);
            break;
          }

          if ( (status = swr_init(os->audio.swr)) ) {
            PDBG("[%s] swr_init() fails: %s", objname(enc), av_err2str(status));
            break;
          }

          status = ffmpeg_create_audio_frame(&os->audio.swr_frame, output_sample_format, output_sample_rate,
              ENCODER_MAX_AUDIO_SAMPLES_PER_FRAME, output_channels, output_channel_layout);

          if ( status ) {
            PDBG("ffmpeg_create_audio_frame() fails");
            break;
          }
        }


        ///

        if ( (status = ffmpeg_filter_codec_opts(opts, codec, AV_OPT_FLAG_ENCODING_PARAM, &codec_opts)) ) {
          PDBG("[%s] ffmpeg_filter_codec_opts() fails", objname(enc));
          break;
        }

        if ( !(os->codec = avcodec_alloc_context3(codec)) ) {
          PDBG("[%s] avcodec_alloc_context3() fails", objname(enc));
          status = AVERROR(ENOMEM);
          break;
        }

        ///

        os->codec->time_base = (AVRational ) { 1, output_sample_rate };
        os->codec->sample_fmt = output_sample_format;
        os->codec->sample_rate = output_sample_rate;
        os->codec->channels = output_channels;
        os->codec->channel_layout = output_channel_layout;
        os->codec->sample_aspect_ratio = is->codecpar->sample_aspect_ratio;

        if ( (status = avcodec_open2(os->codec, codec, &codec_opts)) ) {
          PDBG("[%s] avcodec_open2('%s') fails: %s", objname(enc), codec->name, av_err2str(status));
          break;
        }

        if ( !(os->base.codecpar = avcodec_parameters_alloc())) {
          PDBG("[%s] avcodec_parameters_alloc() fails: %s", objname(enc), av_err2str(status));
          status = AVERROR(ENOMEM);
          break;
        }

        if ( ( status = avcodec_parameters_from_context(os->base.codecpar, os->codec)) < 0 ) {
          PDBG("[%s] avcodec_parameters_from_context('%s') fails: %s", objname(enc), codec->name, av_err2str(status));
          break;
        }

        if ( (status = ffstream_copy(&os->base, is, false, true))) {
          PDBG("[%s] ffstream_copy() fails: %s", objname(enc), av_err2str(status));
          break;
        }

        os->base.time_base = os->codec->time_base;
        os->ppts = AV_NOPTS_VALUE;
      }
      break;

      default:
      break;
    }

    if ( codec_opts ) {
      av_dict_free(&codec_opts);
    }

    if ( status ) {
      break;
    }
  }

  if ( status ) {
    goto end;
  }

  enc->source = source;
  ffgop_set_streams(&enc->gop, (const ffstream **) enc->oss, enc->nb_streams);

end:

  if ( opts ) {
    av_dict_free(&opts);
  }

  if ( status ) {
    free_streams(enc);
  }

  return status;
}


static int encode_and_send(struct ffenc * enc, int stidx, AVFrame * frame)
{
  struct ostream * os;

  AVPacket pkt;
  int64_t t0 = 0;
  int status, gotpkt;

  os = enc->oss[stidx];

  av_init_packet(&pkt);
  pkt.data = NULL;
  pkt.size = 0;



//  {
//    int64_t upts = av_rescale_ts(frame->pts, os->codec->time_base, (AVRational){1, 1000});
//    PDBG("[%s] IFRM [st=%2d] %s pts=%s upts=%s", objname(enc), stidx, av_get_media_type_string(os->codec->codec_type),
//        av_ts2str(frame->pts), av_ts2str(upts));
//  }



  switch ( os->codec->codec_type ) {
    case AVMEDIA_TYPE_AUDIO :
      status = avcodec_encode_audio2(os->codec, &pkt, frame, &gotpkt);
    break;

    case AVMEDIA_TYPE_VIDEO :

//      frame->quality = os->codec->global_quality;
//
//      if ( enc->forced_key_frames_interval <= 0 ) {
//        t0 = ffmpeg_gettime();
//      }
//      else if ( (t0 = ffmpeg_gettime()) >= enc->t0 + enc->forced_key_frames_interval ) {
//        frame->key_frame = 1;
//        frame->pict_type = AV_PICTURE_TYPE_I;
//        enc->t0 = t0;
//      }

      if ( (status = avcodec_encode_video2(os->codec, &pkt, frame, &gotpkt)) < 0 ) {
        PDBG("[%s] [st=%d] avcodec_encode_video2() fails: frame->pts=%s frame->ppts=%s %s", objname(enc), stidx,
            av_ts2str(frame->pts), av_ts2str(os->ppts), av_err2str(status));
      }

      if ( pkt.flags & AV_PKT_FLAG_KEY ) {
        enc->t0 = t0;
      }

    break;

    default :
      status = -1;
    break;
  }

  if ( status >= 0 && gotpkt ) {

    pkt.stream_index = stidx;

    if ( os->base.time_base.den != os->codec->time_base.den || os->base.time_base.num != os->codec->time_base.num ) {
      av_packet_rescale_ts(&pkt, os->codec->time_base, os->base.time_base);
    }

//    {
//      int64_t upts = av_rescale_ts(pkt.pts, os->base.time_base, (AVRational){1, 1000});
//      int64_t udts = av_rescale_ts(pkt.dts, os->base.time_base, (AVRational){1, 1000});
//      PDBG("[%s] OPKT [st=%2d] %s pts=%s dts=%s key=%d\t upts=%s udts=%s", objname(enc), stidx, av_get_media_type_string(os->codec->codec_type),
//          av_ts2str(pkt.pts), av_ts2str(pkt.dts), (pkt.flags & AV_PKT_FLAG_KEY), av_ts2str(upts), av_ts2str(udts));
//    }

    ffgop_put_pkt(&enc->gop, &pkt);

//    PDBG("PUT OK");
  }

  av_packet_unref(&pkt);

  return status;
}


static int encode_video(struct ffenc * enc, AVFrame * in_frame)
{
  AVFrame * out_frame;

  const struct ffstream * is;
  struct ostream * os;

  int stidx;
  int64_t pts;
  int status;

  // TODO: See if (s->oformat->flags & AVFMT_RAWPICTURE && at ffmpeg.c:1082

  stidx = (int) (ssize_t) (in_frame->opaque);
  is = enc->iss[stidx];
  os = enc->oss[stidx];

  pts = av_rescale_ts(in_frame->pts, is->time_base, os->codec->time_base);

//  PDBG("[%s] IFRM [st=%d] in_frame->pts=%s pts=%s itb=%s ctb=%s", objname(enc), stidx, av_ts2str(in_frame->pts),
//      av_ts2str(pts), av_tb2str(is->time_base), av_tb2str(os->codec->time_base));

  if ( pts <= os->ppts ) {
    PDBG("[%s] out of order pts=%s <= ppts=%s", objname(enc), av_ts2str(pts), av_ts2str(os->ppts));
    // status = AVERROR(EPERM);
  }

  if ( !os->video.sws ) {
    out_frame = in_frame;
  }
  else {
    out_frame = os->video.sws_frame;
    out_frame->key_frame = in_frame->key_frame;
    out_frame->pict_type = in_frame->pict_type;
    sws_scale(os->video.sws, (const uint8_t * const *) in_frame->data, in_frame->linesize, 0, in_frame->height,
        out_frame->data, out_frame->linesize);
  }

  out_frame->pts = pts;
  status = encode_and_send(enc, stidx, out_frame);

  os->ppts = pts;

  return status;
}


// warning:
//  To work this function correctly, the codec time_base MUST be set to 1/sample_rate
static int encode_audio(struct ffenc * enc, AVFrame * in_frame )
{
  const struct ffstream * is;
  struct ostream * os;
  int stidx;

  AVFrame * out_frame;
  int nb_samples_consumed;
  int nb_samples = 0;
  int64_t delay = 0;
  int64_t pts;

  // int64_t frame_size_ts;


  int status = 0;


  stidx = (int) (ssize_t) (in_frame->opaque);
  is = enc->iss[stidx];
  os = enc->oss[stidx];


  if ( !os->audio.swr ) {
    out_frame = in_frame;
  }
  else {

    out_frame = os->audio.swr_frame;

    delay = swr_get_delay(os->audio.swr, in_frame->sample_rate);

    nb_samples = av_rescale_rnd(delay + in_frame->nb_samples, out_frame->sample_rate, in_frame->sample_rate,
        AV_ROUND_UP);

    out_frame->nb_samples = swr_convert(os->audio.swr, out_frame->extended_data, nb_samples,
        (const uint8_t **) in_frame->extended_data, in_frame->nb_samples);

  }


  pts = av_rescale_ts(in_frame->pts - delay, is->time_base, os->codec->time_base);
//  {
//    int64_t ufpts = av_rescale_ts(in_frame->pts, os->codec->time_base, (AVRational ) { 1, 1000 });
//    int64_t ucpts = av_rescale_ts(pts, os->codec->time_base, (AVRational ) { 1, 1000 });
//    PDBG("[%s] IFRM [st=%2d] %s f->pts=%s c->pts=%s f->upts=%s c->upts=%s", objname(enc), stidx,
//        av_get_media_type_string(os->codec->codec_type), av_ts2str(in_frame->pts), av_ts2str(ufpts),
//        av_ts2str(pts), av_ts2str(ucpts));
//  }

  out_frame->pts = pts;

  if ( out_frame->pts <= os->ppts ) {
    PDBG("[%s] out of order pts=%s <= ppts=%s", objname(enc), av_ts2str(out_frame->pts), av_ts2str(os->ppts));
  }


  if ( !os->codec->frame_size ) {
    status = encode_and_send(enc, stidx, out_frame);
    goto end;
  }


  if ( !os->audio.tmp_frame ) {

    status = ffmpeg_create_audio_frame(&os->audio.tmp_frame, os->codec->sample_fmt, os->codec->sample_rate,
        os->codec->frame_size, os->codec->channels, os->codec->channel_layout);
    if ( status ) {
      PDBG("[%s] ffmpeg_create_audio_frame() fails: %s", objname(enc), av_err2str(status));
      goto end;
    }

    os->audio.tmp_frame->pts = out_frame->pts;
    os->audio.tmp_frame->nb_samples = 0;

    av_frame_make_writable(os->audio.tmp_frame);
  }



  //frame_size_ts = av_rescale_ts(os->codec->frame_size, );

  if ( out_frame->pts > os->audio.tmp_frame->pts + os->codec->frame_size ) {
    // drop lost fragment
    PDBG("[%s] DROP", objname(enc));
    os->audio.tmp_frame->pts = out_frame->pts;
    os->audio.tmp_frame->nb_samples = 0;
  }


  nb_samples_consumed = 0;

  while ( out_frame->nb_samples > 0 ) {

    int tmp_space = os->codec->frame_size - os->audio.tmp_frame->nb_samples;
    int n = out_frame->nb_samples >= tmp_space ? tmp_space : out_frame->nb_samples;

    av_samples_copy(os->audio.tmp_frame->extended_data, out_frame->extended_data, os->audio.tmp_frame->nb_samples,
        nb_samples_consumed, n, os->codec->channels, os->codec->sample_fmt);

    if ( (os->audio.tmp_frame->nb_samples += n) == os->codec->frame_size ) {

      if ( (status = encode_and_send(enc, stidx, os->audio.tmp_frame)) < 0 ) {
        PDBG("[%s] encode_and_send() fails: %s", objname(enc), av_err2str(status));
        break;
      }

      os->audio.tmp_frame->pts += os->codec->frame_size;
      os->audio.tmp_frame->nb_samples = 0;
    }

    out_frame->nb_samples -= n;
    nb_samples_consumed += n;
  }

end:

  os->ppts = out_frame->pts;

  return status;
}

static void encoder_thread(void * arg)
{
  struct ffenc * enc = arg;

  AVFrame * frame;
  const struct ffstream * is;
  //struct ostream * os;
  int stidx;

  struct ffgop * igop = NULL;
  struct ffgoplistener * gl = NULL;

  int status = 0;

  PDBG("[%s] ENTER", objname(enc));

  if ( !(frame = av_frame_alloc()) ) {
    PDBG("[%s] av_frame_alloc() fails", objname(enc));
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(igop = get_gop(enc->source)) ) {
    status = AVERROR(EFAULT);
    goto end;
  }

  if ( (status = ffgop_create_listener(igop, &gl)) ) {
    goto end;
  }

  // igop->debug = true;

  while ( status >= 0 && enc->base.refs > 1 ) {

    if ( (status = ffgop_get_frm(gl, frame)) ) {
      PDBG("[%s] ffgop_get_frm() fails: %s", objname(enc), av_err2str(status));
      break;
    }


    stidx = (int)(ssize_t)(frame->opaque);
    if ( stidx < 0 || stidx > (int)enc->nb_streams ) {
      PDBG("[%s] invalid stream_index=%d", objname(enc), stidx);
      exit(1);
    }

    is = enc->iss[stidx];
    // os = enc->oss[stidx];

//      PDBG("[%s] IFRM [st=%d] %s pts=%s itb=%s", objname(enc), stidx, av_get_media_type_string(is->codecpar->codec_type),
//          av_ts2str(frame->pts), av_tb2str(is->time_base));

    switch ( is->codecpar->codec_type ) {

      case AVMEDIA_TYPE_VIDEO :
        if ( frame->pts == AV_NOPTS_VALUE ) {
          PDBG("[%s] DROP: frame->pts==AV_NOPTS_VALUE", objname(enc));
        }
        else if ( (status = encode_video(enc, frame)) ) {
          PDBG("[%s] encode_video() fails: stream_index=%d status=%d %s", objname(enc), stidx, status, av_err2str(status));
        }
      break;

      case AVMEDIA_TYPE_AUDIO :
        if ( (status = encode_audio(enc, frame)) ) {
          PDBG("[%s] encode_audio() fails: %s", objname(enc), av_err2str(status));
        }
      break;

      default :
      break;
    }

//    PDBG("[%s] ENC [st=%d %s] %s", objname(enc), stidx, av_get_media_type_string(is->codecpar->codec_type),
//        av_err2str(status));

    av_frame_unref(frame);
  }


end:

  ffgop_delete_listener(&gl);
  ffgop_put_eof(&enc->gop, status);
  av_frame_free(&frame);

  PDBG("[%s] FINIDHED", objname(enc));
  release_object(&enc->base);
}


int ff_create_encoder(struct ffobject ** obj, const struct ff_create_encoder_args * args)
{
  static const struct ff_object_iface iface = {
    .on_add_ref = NULL,
    .on_destroy = on_destroy_encoder,
    .get_streams = get_encoded_streams,
    .get_gop = get_encoded_gop,
  };

  struct ffenc * enc = NULL;
  int status = 0;

  if ( !args || !args->name || !args->source ) {
    status = AVERROR(EINVAL);
    goto end;
  }


  if ( !(enc = ff_create_object(sizeof(struct ffenc), object_type_encoder, args->name, &iface)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }


  if ( (status = start_encoding(enc, args->source, args->params)) ) {
    goto end;
  }

  add_object_ref(&enc->base);

  if ( !co_schedule(encoder_thread, enc, ENCODER_THREAD_STACK_SIZE) ) {
    status = AVERROR(errno);
    PDBG("[%s] co_schedule(encoder_thread) fails: %s", objname(enc), strerror(errno));
    release_object(&enc->base);
    goto end;
  }

end:

  if ( status && enc ) {
    release_object(&enc->base);
    enc = NULL;
  }

  *obj = (void*)enc;

  return status;
}
