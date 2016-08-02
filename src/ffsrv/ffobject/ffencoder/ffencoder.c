/*
 * ffencoder.c
 *
 *  Created on: Apr 25, 2016
 *      Author: amyznikov
 */

#include "ffencoder.h"
#include "ffcfg.h"
#include "ffgop.h"
#include "debug.h"

#define ENCODER_THREAD_STACK_SIZE               (ffsrv.mem.ffenc)

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

  uint nb_input_streams;
  uint nb_output_streams;
  const struct ffstream * const * iss; // [nb_input_streams]
  struct ostream ** oss;  // [nb_output_streams]
  struct ffstmap * smap;  // [nb_output_streams]

  int64_t t0, force_key_frames;
};



static void free_encoded_streams(struct ffenc * enc)
{
  if ( enc->oss ) {
    for ( uint i = 0; i < enc->nb_output_streams; ++i ) {
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
    ffmpeg_free_ptr_array(&enc->oss, enc->nb_output_streams);
  }

  if ( enc->smap ) {
    free(enc->smap);
    enc->smap = NULL;
  }
}

static int alloc_encoded_streams(struct ffenc * enc, AVDictionary * opts)
{
  int status = 0;

  if ( (status = ffmpeg_parse_stream_mapping(opts, (uint[]){enc->nb_input_streams}, 1, &enc->smap)) < 0 ) {
    PDBG("[%s] ffmpeg_parse_stream_mapping() fails: %s", objname(enc), av_err2str(status));
  }
  else if ( (enc->nb_output_streams = status) < 1 ) {
    PDBG("[%s] NO OUTPUT STREAMS. CHECK STREAM MAPPING", objname(enc));
    status = AVERROR(EINVAL);
  }
  else if ( !(enc->oss = ffmpeg_alloc_ptr_array(enc->nb_output_streams, sizeof(struct ostream))) ) {
    status = AVERROR(ENOMEM);
  }
  else {
    PDBG("nb_output_streams=%u", enc->nb_output_streams);
    status = 0;
  }

  return status;
}


static void on_destroy_encoder(void * ffobject)
{
  struct ffenc * enc = ffobject;
  ffgop_cleanup(&enc->gop);
  free_encoded_streams(enc);
  release_object(enc->source);
  enc->source = NULL;
}


static int get_encoded_streams(void * ffobject, const ffstream * const ** streams, uint * nb_streams)
{
  struct ffenc * enc = ffobject;
  *streams = (const ffstream * const *)enc->oss;
  *nb_streams = enc->nb_output_streams;
  return 0;
}

static struct ffgop * get_encoded_gop(void * ffobject)
{
  struct ffenc * enc = ffobject;
  return &enc->gop;
}


static inline const char * strprintf(char buf[], size_t size, const char * format, ...)
{
  va_list arglist;
  va_start(arglist, format);
  vsnprintf(buf, size - 1, format, arglist);
  va_end(arglist);
  buf[size - 1] = 0;
  return buf;
}

#define ssprintf(...)\
    strprintf((char[128]){}, 128, __VA_ARGS__)


static AVDictionaryEntry * getffopt(AVDictionary * opts, const char * optname, char stype, int sidx)
{
  AVDictionaryEntry * e;

  if ( (e = av_dict_get(opts, ssprintf("%s:%c:%d", optname, stype, sidx), NULL, 0)) ) {
    return e;
  }
  if ( (e = av_dict_get(opts, ssprintf("%s:%d", optname, sidx), NULL, 0)) ) {
    return e;
  }
  if ( (e = av_dict_get(opts, ssprintf("%s:%c", optname, stype), NULL, 0)) ) {
    return e;
  }
  if ( (e = av_dict_get(opts, optname, NULL, 0)) ) {
    return e;
  }
  return NULL;
}


static void dump_ffopts(const char * msg, AVDictionary * opts )
{
  AVDictionaryEntry * e = NULL;
  PDBG("%s:", msg);
  while ( (e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)) ) {
    PDBG("'%s'='%s'", e->key, e->value);
  }
}

static int start_encoding(struct ffenc * enc, struct ffobject * source, const struct ffencoder_params * params)
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

  if ( (status = ffmpeg_parse_options(options, true, &opts)) ) {
    PDBG("[%s] ffmpeg_parse_options() fails: %s", objname(enc), av_err2str(status));
    goto end;
  }

  if ( (status = get_streams(source, &enc->iss, &enc->nb_input_streams)) ) {
    PDBG("[%s] get_streams(source=%p) fails: %s", objname(enc), source, av_err2str(status));
    goto end;
  }

  if ( (status = alloc_encoded_streams(enc, opts)) ) {
    PDBG("[%s] alloc_output_streams(opts=%p) fails: %s", objname(enc), opts, av_err2str(status));
    goto end;
  }

  status = ffgop_init(&enc->gop, &(struct ffgop_init_args ) {
        .type = ffgop_pkt,
        .capacity = ENCODER_FFGOP_SIZE
      });

  if ( status ) {
    goto end;
  }

  for ( uint isidx = 0; isidx < enc->nb_input_streams; ++isidx ) {

    int oidxs[enc->nb_output_streams];
    int nbos = ffmpeg_map_input_stream(enc->smap, enc->nb_output_streams, 0, isidx, oidxs);

    for ( int j = 0; j < nbos; ++j ) {

      uint osidx = oidxs[j];

      is = enc->iss[isidx];
      os = enc->oss[osidx];

      codec_name = NULL;

      switch ( is->codecpar->codec_type ) {

        case AVMEDIA_TYPE_VIDEO : {

          int input_width, input_height;
          int output_width, output_height;
          //int /*input_bitrate, */output_bitrate;
          enum AVPixelFormat input_fmt, output_fmt;
          //int gop_size;
          double qscale = -1;
          double forced_key_frames_interval = -1;

          ///

          if ( (e = getffopt(opts, "c", 'v', osidx) ) ) {
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

          if ( !(e = getffopt(opts, "s", 'v', osidx)) ) {
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
          if ( !(e = getffopt(opts, "pix_fmt", 'v', osidx)) ) {
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

//          //input_bitrate = is->codecpar->bit_rate;
//          if ( !(e = getffopt(opts, "b", 'v', osidx)) ) {
//            output_bitrate = 0;    // input_bitrate >= 1000 && input_bitrate < 1000000 ? input_bitrate : 256000;
//          }
//          else if ( (output_bitrate = (int) av_strtod(e->value, NULL)) < 1000 ) {
//            PDBG("[%s] Bad output bitrate specified: %s", objname(enc), e->value);
//            status = AVERROR(EINVAL);
//            break;
//          }


          ///

//          if ( !(e = getffopt(opts, "g", 'v', osidx)) ) {
//            gop_size = 0;
//          }
//          else if ( sscanf(e->value, "%d", &gop_size) != 1 || gop_size < 1 ) {
//            PDBG("[%s] Bad output gop size specified: %s", objname(enc), e->value);
//            status = AVERROR(EINVAL);
//            break;
//          }


          ///

          if ( (e = getffopt(opts, "q", 'v', osidx)) || (e = getffopt(opts, "qscale", 'v', osidx)) ) {
            if ( sscanf(e->value, "%lf", &qscale) != 1 ) {
              PDBG("[%s] Bad output global_quality qscale specified: %s", objname(enc), e->value);
              status = AVERROR(EINVAL);
              break;
            }
          }


          ///

          if ( (e = getffopt(opts, "force_key_frames", 'v', osidx)) ) {

            int h = 0, m = 0;
            double s = 0;

            if ( sscanf(e->value, "%d:%d:%lf", &h, &m, &s) == 3 ) {
              if ( h < 0 || h >= 24 || m < 0 || m >= 60 || s < 0 || s >= 60 ) {
                status = AVERROR(EINVAL);
              }
              else {
                forced_key_frames_interval = h * 3600 + m * 60 + s;
              }
            }
            else if ( sscanf(e->value, "%d:%lf", &m, &s) == 2 ) {
              if ( m < 0 || m > 59 || s < 0 || s >= 60 ) {
                status = AVERROR(EINVAL);
              }
              else {
                forced_key_frames_interval = m * 60 + s;
              }
            }
            else if ( sscanf(e->value, "%lf", &s) == 1 ) {
              if ( s < 0 || s >= 60 ) {
                status = AVERROR(EINVAL);
              }
              else {
                forced_key_frames_interval = s;
              }
            }

            if ( status ) {
              PDBG("[%s] Bad force_key_frames interval specified: %s", objname(enc), e->value);
              break;
            }

            enc->force_key_frames = forced_key_frames_interval * FFMPEG_TIME_SCALE;
          }



          // fixme: profile preset




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
          if ( (status = ffmpeg_filter_codec_opts(opts, codec, osidx, AV_OPT_FLAG_ENCODING_PARAM, &codec_opts)) ) {
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
          os->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
          os->codec->sample_aspect_ratio = is->codecpar->sample_aspect_ratio;
          if ( qscale >= 0 ) {
            os->codec->flags |= CODEC_FLAG_QSCALE;
            os->codec->global_quality = FF_QP2LAMBDA * qscale;
          }


          dump_ffopts("VIDEO CODEC OPTS", codec_opts);

          if ( (status = avcodec_open2(os->codec, codec, &codec_opts)) ) {
            PDBG("[%s] avcodec_open2('%s') fails: %s", objname(enc), codec->name, av_err2str(status));
            break;
          }

          if ( !(os->base.codecpar = avcodec_parameters_alloc()) ) {
            PDBG("[%s] avcodec_parameters_alloc() fails: %s", objname(enc), av_err2str(status));
            status = AVERROR(ENOMEM);
            break;
          }

          if ( (status = avcodec_parameters_from_context(os->base.codecpar, os->codec)) < 0 ) {
            PDBG("[%s] avcodec_parameters_from_context('%s') fails: %s", objname(enc), codec->name, av_err2str(status));
            break;
          }

          if ( (status = ffstream_copy(&os->base, is, false, true)) ) {
            PDBG("[%s] ffstream_copy() fails: %s", objname(enc), av_err2str(status));
            break;
          }

          os->base.time_base = os->codec->time_base;
          os->ppts = AV_NOPTS_VALUE;
        }
        break;


      case AVMEDIA_TYPE_AUDIO : {

          int input_channels, output_channels = 0;
          uint64_t input_channel_layout, output_channel_layout = 0;
          int input_sample_rate, output_sample_rate;
          int input_sample_format, output_sample_format;

          ///
          if ( (e = getffopt(opts, "c", 'a', osidx) ) ) {
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
          if ( !(e = getffopt(opts, "ac", 'a', osidx)) ) {
            output_channels = input_channels;
          }
          else if ( sscanf(e->value, "%d", &output_channels) == 1 && output_channels > 0 ) {
            if ( output_channels == 1 ) {
              output_channel_layout = AV_CH_LAYOUT_MONO;
            }
            else if ( output_channels == 2 ) {
              output_channel_layout = AV_CH_LAYOUT_STEREO;
            }
          }
          else {
            PDBG("[%s] Bad output audio channels specified: %s", objname(enc), e->value);
            status = AVERROR(EINVAL);
            break;
          }

          ///

          input_channel_layout = is->codecpar->channel_layout;

          if ( !(e = getffopt(opts, "channel_layout", 'a', osidx)) ) {
            if ( !output_channel_layout ) {
              output_channel_layout = ffmpeg_select_best_channel_layout(codec, output_channels, input_channel_layout);
            }
          }
          else if ( !(output_channel_layout = av_get_channel_layout(e->value)) ) {
            PDBG("[%s] Bad output audio channel layout specified: %s", objname(enc), e->value);
            status = AVERROR(EINVAL);
            break;
          }
          else if ( !ffmpeg_is_channel_layout_supported(codec, output_channel_layout) ) {
            PDBG("[%s] Specified output audio channel layout %s is not supported by %s", objname(enc), e->value,
                codec->name);
            status = AVERROR(EINVAL);
            break;
          }

          ///

          input_sample_rate = is->codecpar->sample_rate;

          if ( !(e = getffopt(opts, "ar", 'a', osidx)) ) {
            output_sample_rate = ffmpeg_select_samplerate(codec, NULL, input_sample_rate);
          }
          else if ( sscanf(e->value, "%d", &output_sample_rate) != 1 || output_sample_rate < 1 ) {
            PDBG("[%s] Bad output audio sample rate specified: %s", objname(enc), e->value);
            status = AVERROR(EINVAL);
            break;
          }
          else if ( !ffmpeg_is_samplerate_supported(codec, NULL, output_sample_rate) ) {
            PDBG("[%s] Specified output audio sample rate %s is not supported by %s", objname(enc), e->value,
                codec->name);
            status = AVERROR(EINVAL);
            break;
          }

          ///

          input_sample_format = is->codecpar->format;

          if ( !(e = getffopt(opts, "sample_fmt", 'a', osidx)) ) {
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

          if ( (status = ffmpeg_filter_codec_opts(opts, codec, osidx, AV_OPT_FLAG_ENCODING_PARAM, &codec_opts)) ) {
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

          dump_ffopts("AUDIO CODEC OPTS", codec_opts);
          if ( (status = avcodec_open2(os->codec, codec, &codec_opts)) ) {
            PDBG("[%s] avcodec_open2('%s') fails: %s", objname(enc), codec->name, av_err2str(status));
            break;
          }

          if ( !(os->base.codecpar = avcodec_parameters_alloc()) ) {
            PDBG("[%s] avcodec_parameters_alloc() fails: %s", objname(enc), av_err2str(status));
            status = AVERROR(ENOMEM);
            break;
          }

          if ( (status = avcodec_parameters_from_context(os->base.codecpar, os->codec)) < 0 ) {
            PDBG("[%s] avcodec_parameters_from_context('%s') fails: %s", objname(enc), codec->name, av_err2str(status));
            break;
          }

          if ( (status = ffstream_copy(&os->base, is, false, true)) ) {
            PDBG("[%s] ffstream_copy() fails: %s", objname(enc), av_err2str(status));
            break;
          }

          os->base.time_base = os->codec->time_base;
          os->ppts = AV_NOPTS_VALUE;
        }
        break;

        default :
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
      break;
    }

  }

  if ( status ) {
    goto end;
  }

  enc->source = source;
  ffgop_set_streams(&enc->gop, (const ffstream **) enc->oss, enc->nb_output_streams);

end:

  if ( opts ) {
    av_dict_free(&opts);
  }

  if ( status ) {
    free_encoded_streams(enc);
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


  switch ( os->codec->codec_type ) {
    case AVMEDIA_TYPE_AUDIO :
      status = avcodec_encode_audio2(os->codec, &pkt, frame, &gotpkt);
    break;

    case AVMEDIA_TYPE_VIDEO :

      if ( enc->force_key_frames <= 0 || (t0 = ffmpeg_gettime_us()) < enc->t0 + enc->force_key_frames ) {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
      }
      else {
        frame->key_frame = 1;
        frame->pict_type = AV_PICTURE_TYPE_I;
        enc->t0 = t0;
        // PDBG("[%s] FORCE KEY", objname(enc));
      }

      if ( os->codec->flags & CODEC_FLAG_QSCALE ) {
        frame->quality = os->codec->global_quality;
      }

      //    {
      //      int64_t upts = av_rescale_ts(frame->pts, os->codec->time_base, (AVRational){1, 1000});
      //      PDBG("[%s] IFRM [st=%2d] %s key=%d pic=%d pts=%8s upts=%8s dpts=%8s ctb=%s", objname(enc), stidx, av_get_media_type_string(os->codec->codec_type),
      //        frame->key_frame, frame->pict_type, av_ts2str(frame->pts), av_ts2str(upts), av_ts2str(upts-os->opts),av_tb2str(os->codec->time_base));
      //      os->opts = upts;
      //    }

      if ( (status = avcodec_encode_video2(os->codec, &pkt, frame, &gotpkt)) < 0 ) {
        PDBG("[%s] [st=%d] avcodec_encode_video2() fails: frame->pts=%s frame->ppts=%s %s", objname(enc), stidx,
            av_ts2str(frame->pts), av_ts2str(os->ppts), av_err2str(status));
      }

      if ( (enc->force_key_frames > 0) && (pkt.flags & AV_PKT_FLAG_KEY) ) {
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


static int encode_video(struct ffenc * enc, AVFrame * in_frame, int isidx, int osidx)
{
  AVFrame * out_frame;

  const struct ffstream * is;
  struct ostream * os;

  int64_t pts, frame_pts;
  int status;

  // TODO: See if (s->oformat->flags & AVFMT_RAWPICTURE && at ffmpeg.c:1082

  is = enc->iss[isidx];
  os = enc->oss[osidx];

  pts = av_rescale_ts(in_frame->pts, is->time_base, os->codec->time_base);

//  PDBG("[%s] IFRM isidx=%d osidx=%d in_frame->pts=%s pts=%s itb=%s ctb=%s", objname(enc), isidx, osidx, av_ts2str(in_frame->pts),
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

  frame_pts = out_frame->pts;
  out_frame->pts = pts;

  status = encode_and_send(enc, osidx, out_frame);

  os->ppts = out_frame->pts;
  out_frame->pts = frame_pts;


  return status;
}


// warning:
//  To work this function correctly, the codec time_base MUST be set to 1/sample_rate
static int encode_audio(struct ffenc * enc, AVFrame * in_frame, int isidx, int osidx)
{
  const struct ffstream * is;
  struct ostream * os;

  AVFrame * out_frame;
  int nb_samples_consumed;
  int nb_samples = 0;
  int64_t delay = 0;
  int64_t pts, frame_pts;

  // int64_t frame_size_ts;


  int status = 0;

  is = enc->iss[isidx];
  os = enc->oss[osidx];


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

  frame_pts = out_frame->pts;
  out_frame->pts = pts;

  if ( out_frame->pts <= os->ppts ) {
    PDBG("[%s] out of order pts=%s <= ppts=%s", objname(enc), av_ts2str(out_frame->pts), av_ts2str(os->ppts));
  }


  if ( !os->codec->frame_size ) {
    status = encode_and_send(enc, osidx, out_frame);
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

  //   PDBG("[%s] AUDIO: pts=%s opts=%s exp=%s frame_size=%d nb_samples=%d", objname(enc), av_ts2str(out_frame->pts),
  //	 av_ts2str(os->audio.tmp_frame->pts),  av_ts2str(os->audio.tmp_frame->pts + os->codec->frame_size),
  //	 os->codec->frame_size, out_frame->nb_samples);

  if ( out_frame->pts > os->audio.tmp_frame->pts + os->codec->frame_size ) {
    // drop lost fragment
    // PDBG("[%s] DROP: pts=%s opts=%s exp=%s frame_size=%d nb_samples=%d", objname(enc), av_ts2str(out_frame->pts),
    //	 av_ts2str(os->audio.tmp_frame->pts),  av_ts2str(os->audio.tmp_frame->pts + os->codec->frame_size),
    //	 os->codec->frame_size, out_frame->nb_samples);
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

      if ( (status = encode_and_send(enc, osidx, os->audio.tmp_frame)) < 0 ) {
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
  out_frame->pts = frame_pts;

  return status;
}

static void encoder_thread(void * arg)
{
  struct ffenc * enc = arg;

  AVFrame * frame;
  const struct ffstream * is;
  //struct ostream * os;
  int isidx, osidx, osidxs[enc->nb_output_streams], nbos;

  struct ffgop * igop = NULL;
  struct ffgoplistener * gl = NULL;

  int status = 0;

  PDBG("[%s] ENTER", objname(enc));

  if ( !(frame = av_frame_alloc()) ) {
    PDBG("[%s] av_frame_alloc() fails", objname(enc));
    status = AVERROR(ENOMEM);
    goto end;
  }

  PDBG("[%s] QUERY GOP", objname(enc));

  if ( !(igop = get_gop(enc->source)) ) {
    status = AVERROR(EFAULT);
    goto end;
  }

  if ( (status = ffgop_create_listener(igop, &gl, NULL)) ) {
    goto end;
  }

  // igop->debug = true;

  PDBG("[%s] START MAIN LOOP", objname(enc));

  while ( status >= 0 && enc->base.refs > 1 ) {

    if ( (status = ffgop_get_frm(gl, frame)) ) {
      PDBG("[%s] ffgop_get_frm() fails: %s", objname(enc), av_err2str(status));
      break;
    }


    isidx = (int) (ssize_t) (frame->opaque);
    if ( isidx < 0 || isidx > (int) enc->nb_input_streams ) {
      PDBG("[%s] invalid stream_index=%d", objname(enc), isidx);
      status = AVERROR_INVALIDDATA;
      av_frame_unref(frame);
      break;
    }

    is = enc->iss[isidx];

//    PDBG("[%s] IFRM [st=%d] %s pts=%s itb=%s", objname(enc), isidx, av_get_media_type_string(is->codecpar->codec_type),
//        av_ts2str(frame->pts), av_tb2str(is->time_base));


    nbos = ffmpeg_map_input_stream(enc->smap, enc->nb_output_streams, 0, isidx, osidxs);
    for ( int i = 0; i < nbos; ++i ) {

      osidx = osidxs[i];


      switch ( is->codecpar->codec_type ) {

        case AVMEDIA_TYPE_VIDEO :
          if ( frame->pts == AV_NOPTS_VALUE ) {
            PDBG("[%s] DROP: frame->pts==AV_NOPTS_VALUE", objname(enc));
          }
          else if ( (status = encode_video(enc, frame, isidx, osidx)) ) {
            PDBG("[%s] encode_video() fails: isidx=%d osidx=%d status=%d %s", objname(enc), isidx, osidx, status,
                av_err2str(status));
          }
        break;

        case AVMEDIA_TYPE_AUDIO :
          if ( (status = encode_audio(enc, frame, isidx, osidx)) ) {
            PDBG("[%s] encode_audio() fails: isidx=%d osidx=%d status=%d %s", objname(enc), isidx, osidx, status,
                av_err2str(status));
          }
        break;

        default :
          break;
      }

//      PDBG("[%s] ENC isidx=%d osidx=%d %s %s", objname(enc), isidx, osidx, av_get_media_type_string(is->codecpar->codec_type),
//          av_err2str(status));
    }

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
  static const struct ffobject_iface iface = {
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


  if ( !(enc = create_object(sizeof(struct ffenc), ffobjtype_encoder, args->name, &iface)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( (status = start_encoding(enc, args->source, args->params)) ) {
    PDBG("start_encoding() fails: %s", av_err2str(status));
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
    PDBG("C release_object()");
    release_object(&enc->base);
    PDBG("R release_object()");
    enc = NULL;
  }

  *obj = (void*)enc;

  return status;
}
