/*
 * ffencoder.c
 *
 *  Created on: Apr 25, 2016
 *      Author: amyznikov
 */

#include "ffencoder.h"
#include "debug.h"


#define objname(obj) \
    (obj)->base.name


struct ffenc {
  struct ffobject base;

  struct ffobject * source;
  //struct ffgoplistener * gl;
  struct ffgop gop;

  AVFormatContext * oc;

  union {

    struct audio_ctx {
      struct SwrContext * swr;
      AVFrame * swr_frame, * tmp_frame;
    } audio;

    struct video_ctx {
      struct SwsContext * sws;
      AVFrame * sws_frame;
      int64_t ppts;
    } video;

  } * os; // [oc->nb_streams]

  int64_t t0, forced_key_frames_interval;
  int npkt;
};


static void on_release_encoder(void * ffobject)
{
  struct ffenc * enc = ffobject;

  //ffgop_delete_listener(&enc->gl);
  ffgop_cleanup(&enc->gop);
  release_object(enc->source);
}


static int get_encoded_format_context(void * ffobject, struct AVFormatContext ** cc)
{
  struct ffenc * enc = ffobject;
  *cc = enc->oc;
  return 0;
}

static struct ffgop * get_encoded_gop(void * ffobject)
{
  struct ffenc * enc = ffobject;
  return &enc->gop;
}


// See ffmpeg/cmdutils.c filter_codec_opts()
static int filter_encoder_opts(AVDictionary * opts, AVCodec * codec, AVDictionary ** rv)
{
  const AVClass * cc = NULL;
  AVDictionaryEntry * e = NULL;

  char prefix = 0;

  int flags = AV_OPT_FLAG_ENCODING_PARAM;

  int status = 0;

  switch ( codec->type ) {
    case AVMEDIA_TYPE_VIDEO :
      prefix = 'v';
      flags |= AV_OPT_FLAG_VIDEO_PARAM;
    break;
    case AVMEDIA_TYPE_AUDIO :
      prefix = 'a';
      flags |= AV_OPT_FLAG_AUDIO_PARAM;
    break;
    case AVMEDIA_TYPE_SUBTITLE :
      prefix = 's';
      flags |= AV_OPT_FLAG_SUBTITLE_PARAM;
    break;

    default:
      status = AVERROR(EINVAL);
      break;
  }

  if ( status ) {
    goto end;
  }

  cc = avcodec_get_class();

  PDBG("B getopts");
  while ( (e = av_dict_get(opts, "-", e, AV_DICT_IGNORE_SUFFIX)) ) {

    PDBG("e: '%s' = '%s", e->key, e->value);

    if ( av_opt_find(&cc, e->key + 1, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ) {
      av_dict_set(rv, e->key + 1, e->value, 0);
      PDBG("FOUND 1.");
    }
    else if ( (codec->priv_class && av_opt_find(&codec->priv_class, e->key + 1, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ)) ) {
      av_dict_set(rv, e->key + 1, e->value, 0);
      PDBG("FOUND 2.");
    }
    else if ( e->key[1] == prefix && av_opt_find(&cc, e->key + 2, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ) ) {
      av_dict_set(rv, e->key + 2, e->value, 0);
      PDBG("FOUND 3.");
    }
    else {
      PDBG("NOT FOUND");
    }
  }
  PDBG("E getopts");

end:

  return status;
}


static int start_encoding(struct ffenc * enc, struct ffobject * source, const struct ff_encoder_params * params)
{
  const char * opts;
  AVDictionary * codec_opts = NULL;
  AVDictionary * dict = NULL;
  AVDictionaryEntry * e = NULL;
  AVFormatContext * ic = NULL;
  AVFormatContext * oc = NULL;
  AVStream * os, * is;

  AVCodec * codec = NULL;
  const char * codec_name;

  int status = 0;

  if ( params->opts ) {
    opts = params->opts;
  }
  else {
    opts = "-c:v libx264 -c:a aac3";
  }

  if ( (status = av_dict_parse_string(&dict, opts, " \t", " \t", 0)) ) {
    goto end;
  }

  if ( (status = get_format_context(source, &ic)) ) {
    goto end;
  }

  if ( !(oc = avformat_alloc_context())) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(enc->os = calloc(ic->nb_streams, sizeof(enc->os[0]))) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  for ( uint i = 0; i < ic->nb_streams; ++i ) {

    codec_name = NULL;
    is = ic->streams[i];

    if ( !(os = avformat_new_stream(oc, NULL)) ) {
      status = AVERROR(ENOMEM);
      goto end;
    }

    os->id = is->id;
    os->avg_frame_rate = is->avg_frame_rate;
    os->sample_aspect_ratio = is->sample_aspect_ratio;
    os->disposition = is->disposition;
    os->time_base = is->time_base;
    os->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;

    switch ( is->codec->codec_type ) {

      case AVMEDIA_TYPE_VIDEO: {

        if ( (e = av_dict_get(dict, "-c:v", NULL, 0)) ) {
          codec_name = e->value;
        }
        else {
          codec_name = "libx264";
        }

        os->codec->pix_fmt = is->codec->pix_fmt; // AV_PIX_FMT_YUV420P;//
        os->codec->width = is->codec->width;
        os->codec->height = is->codec->height;
        os->codec->time_base = is->time_base;
        os->codec->bit_rate = 500000;
        os->codec->gop_size = 25;
        os->codec->me_range = 1;
        os->codec->qmin = 10;
        os->codec->qmax = 40;
        enc->os[i].video.ppts = AV_NOPTS_VALUE;
      }
      break;

      case AVMEDIA_TYPE_AUDIO: {
        if ( (e = av_dict_get(dict, "-c:a", NULL, 0)) ) {
          codec_name = e->value;
        }
        else {
          codec_name = "aac";
        }

        os->codec->time_base = is->codec->time_base;// (AVRational ) { 1, output_sample_rate };
        os->codec->sample_fmt = is->codec->sample_fmt;
        os->codec->sample_rate = is->codec->sample_rate;
        os->codec->channels = is->codec->channels;
        os->codec->channel_layout = is->codec->channel_layout;

      }
      break;

      default:
      break;
    }

    if ( codec_name ) {

      if ( !(codec = avcodec_find_encoder_by_name(codec_name)) ) {
        PDBG("[%s] avcodec_find_encoder_by_name(%s) fails", objname(enc), codec_name);
        status = AVERROR_ENCODER_NOT_FOUND;
        break;
      }

      if (( status = filter_encoder_opts(dict, codec, &codec_opts))) {
        PDBG("[%s] filter_encoder_opts(%s) fails", objname(enc), codec_name);
        break;
      }
    }

    if ( (status = avcodec_open2(os->codec, codec, &codec_opts)) ) {
      PDBG("[%s] avcodec_open2('%s') fails", objname(enc), codec_name);
      break;
    }

    if ( codec_opts ) {
      av_dict_free(&codec_opts);
    }

  }

  if ( status ) {
    goto end;
  }


  enc->source = source;
  enc->oc = oc;

end:

  if ( codec_opts ) {
    av_dict_free(&codec_opts);
  }

  if ( dict ) {
    av_dict_free(&dict);
  }

  if ( status ) {
    PDBG("C ffmpeg_close_output(&oc=%p)", oc);
    ffmpeg_close_output(&oc);
    PDBG("R ffmpeg_close_output(&oc=%p)", oc);
  }

  return status;
}


static int encode_and_send(struct ffenc * enc, int stream_index, AVFrame * frame)
{
  AVStream * st;

  AVPacket pkt;
  int64_t t0 = 0;
  int status, gotpkt;

  st = enc->oc->streams[stream_index];

  av_init_packet(&pkt);
  pkt.data = NULL;
  pkt.size = 0;

  switch ( st->codec->codec_type ) {
    case AVMEDIA_TYPE_AUDIO :
      status = avcodec_encode_audio2(st->codec, &pkt, frame, &gotpkt);
    break;

    case AVMEDIA_TYPE_VIDEO :

      frame->quality = st->codec->global_quality;

      if ( enc->forced_key_frames_interval <= 0 ) {
        t0 = ffmpeg_gettime();
      }
      else if ( (t0 = ffmpeg_gettime()) >= enc->t0 + enc->forced_key_frames_interval ) {
        frame->key_frame = 1;
        frame->pict_type = AV_PICTURE_TYPE_I;
        enc->t0 = t0;
      }

      if ( (status = avcodec_encode_video2(st->codec, &pkt, frame, &gotpkt)) < 0 ) {
        PDBG("avcodec_encode_video2() fails: %s frame->pts=%"PRId64" frame->ppts=%"PRId64"",
            av_err2str(status), frame->pts, enc->os[stream_index].video.ppts);
      }

      if ( !gotpkt ) {
        // PDBG("ENC: pic=%d %"PRId64" us", out_frame->pict_type, ffmpeg_gettime() - t0);
      }
      else {
//        PDBG("PKT: pic=%2d flags=0x%0X %"PRId64" us ctb_pts=%"PRId64" ctb_dts=%"PRId64" size=%d stb=%d/%d ctb=%d/%d",
//            out_frame->pict_type, pkt.flags, ffmpeg_gettime() - t0, pkt.pts, pkt.dts, pkt.size,
//            st->time_base.num, st->time_base.den,
//            st->codec->time_base.num, st->codec->time_base.den);
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

    pkt.stream_index = stream_index;

    if ( st->time_base.den != st->codec->time_base.den || st->time_base.num != st->codec->time_base.num ) {
      ffmpeg_rescale_timestamps(&pkt, st->codec->time_base, st->time_base);
    }

//    sctx->video.avg_size = (sctx->video.avg_size * sctx->video.npkts + pkt.size) / (sctx->video.npkts + 1);
//    ++sctx->video.npkts;
////
//    PENC("output_packet: pts=%"PRId64" dts=%"PRId64" size=%d flags=0x%X npkts=%d avg_size=%g", pkt.pts, pkt.dts,
//        pkt.size, pkt.flags, sctx->video.npkts, sctx->video.avg_size);

    ffgop_put_pkt(&enc->gop, &pkt, st->codec->codec_type);
  }

  ff_avpacket_unref(&pkt);

  return status;
}


static int encode_video(struct ffenc * enc, AVFrame * in_frame)
{
  AVFrame * out_frame;
  AVStream * st;
  struct video_ctx * video;
  int stream_index;
  int64_t pts;
  int status;

  // TODO: See if (s->oformat->flags & AVFMT_RAWPICTURE && at ffmpeg.c:1082

  stream_index = (int) (ssize_t) (in_frame->opaque);
  st = enc->oc->streams[stream_index];
  video = &enc->os[stream_index].video;

  //PDBG("1) [st=%d] pts=%"PRId64" st->time_base=%d/%d st->codec->time_base=%d/%d", stream_index, in_frame->pts, st->time_base.num, st->time_base.den, st->codec->time_base.num, st->codec->time_base.den);

  pts = av_rescale_q_rnd(in_frame->pts, st->time_base, st->codec->time_base, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

  //PDBG("2) [st=%d] pts=%"PRId64"", stream_index, pts);

  ++enc->npkt;


  if ( pts <= video->ppts ) {
    PDBG("[%s] invalid pts=%"PRId64"<= ppts=%"PRId64" npkt=%d", objname(enc), pts, video->ppts, enc->npkt);
    status = AVERROR(EPERM);
  }
  else {

    if ( !video->sws ) {
      out_frame = in_frame;
    }
    else {
      out_frame = video->sws_frame;
      out_frame->key_frame = in_frame->key_frame;
      out_frame->pict_type = in_frame->pict_type;
      sws_scale(video->sws, (const uint8_t * const *) in_frame->data, in_frame->linesize, 0, in_frame->height,
            out_frame->data, out_frame->linesize);
    }

    out_frame->pts = pts;
    status = encode_and_send(enc, stream_index, out_frame);
  }

  video->ppts = pts;

  return status;
}

static int encode_audio(struct ffenc * enc, AVFrame * in_frame )
{
  AVFrame * out_frame;
  AVStream * st;
  struct audio_ctx * audio;
  int stream_index;
  int nb_samples;
  int64_t delay = -1;
  int status = 0;

  stream_index = (int) (ssize_t) (in_frame->opaque);
  st = enc->oc->streams[stream_index];
  audio = &enc->os[stream_index].audio;

  if ( !audio->swr ) {
    out_frame = in_frame;
  }
  else {

    out_frame = audio->swr_frame;

    delay = swr_get_delay(audio->swr, in_frame->sample_rate);

    nb_samples = av_rescale_rnd(delay + in_frame->nb_samples, out_frame->sample_rate, in_frame->sample_rate,
        AV_ROUND_UP);

    out_frame->nb_samples = swr_convert(audio->swr, out_frame->extended_data, nb_samples,
        (const uint8_t **) in_frame->extended_data, in_frame->nb_samples);

  }

  out_frame->pts = av_rescale_q_rnd(in_frame->pts - delay, st->time_base, st->codec->time_base,
      AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);

  if ( !st->codec->frame_size ) {
    status = encode_and_send(enc, stream_index, out_frame);
  }
  else {

    int nb_samples_consumed;
    AVFrame * tmp_frame;

    if ( !audio->tmp_frame ) {
      audio->tmp_frame = ffmpeg_audio_frame_create(st->codec->sample_fmt, st->codec->sample_rate,
          st->codec->frame_size, st->codec->channels, st->codec->channel_layout);
      audio->tmp_frame->pts = out_frame->pts;
      audio->tmp_frame->nb_samples = 0;

      av_frame_make_writable(audio->tmp_frame);
    }

    tmp_frame = audio->tmp_frame;

    if ( out_frame->pts > tmp_frame->pts + st->codec->frame_size ) {
      // drop lost fragment
      PDBG("[%s] DROP", objname(enc));
      tmp_frame->pts = out_frame->pts;
      tmp_frame->nb_samples = 0;
    }

    nb_samples_consumed = 0;

    while ( out_frame->nb_samples > 0 ) {

      int tmp_space = st->codec->frame_size - tmp_frame->nb_samples;
      int N = out_frame->nb_samples >= tmp_space ? tmp_space : out_frame->nb_samples;

      av_samples_copy(tmp_frame->extended_data, out_frame->extended_data, tmp_frame->nb_samples, nb_samples_consumed,
          N, st->codec->channels, st->codec->sample_fmt);

      if ( (tmp_frame->nb_samples += N) == st->codec->frame_size ) {

        status = encode_and_send(enc, stream_index, tmp_frame);
        if ( status < 0 ) {
          PDBG("[%s] encode_and_send() fails: %s", objname(enc), av_err2str(status));
          break;
        }

        tmp_frame->pts += st->codec->frame_size;
        tmp_frame->nb_samples = 0;
      }

      out_frame->nb_samples -= N;
      nb_samples_consumed += N;
    }
  }

  return status;
}

static void encoder_thread(void * arg)
{
  struct ffenc * enc = arg;

  AVFrame * frame;
  AVStream * st;
  int stream_index;

  struct ffgop * igop = NULL;
  struct ffgoplistener * gl = NULL;

  int status = 0;

  PDBG("[%s] ENTER", objname(enc));

  if ( !(frame = av_frame_alloc()) ) {
    PDBG("[%s] av_frame_alloc() fails", objname(enc));
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(igop = ff_get_gop(enc->source)) ) {
    status = AVERROR(EFAULT);
    goto end;
  }

  if ( (status = ffgop_create_listener(igop, &gl)) ) {
    goto end;
  }

  while ( status >= 0 && enc->base.refs > 1 ) {

    if ( (status = ffgop_get_frm(gl, frame)) ) {
      PDBG("[%s] ffgop_get_frm() fails: %s", objname(enc), av_err2str(status));
      break;
    }

    stream_index = (int)(ssize_t)(frame->opaque);
    if ( stream_index < 0 || stream_index > (int)enc->oc->nb_streams ) {
      PDBG("[%s] invalid stream_index=%d", objname(enc), stream_index);
      exit(1);
    }

    st = enc->oc->streams[stream_index];

   // PDBG("[st=%d] pts=%"PRId64"", stream_index, frame->pts);

    switch ( st->codec->codec->type ) {

      case AVMEDIA_TYPE_VIDEO :
        // PDBG("[%s] video st=%d", objname(enc), stream_index);

        if ( frame->pts == AV_NOPTS_VALUE ) {
          PDBG("[%s] DROP: frame->pts==AV_NOPTS_VALUE", objname(enc));
        }
        else if ( (status = encode_video(enc, frame)) ) {
          PDBG("[%s] encode_video() fails: stream_index=%d status=%d %s", objname(enc), stream_index, status, av_err2str(status));
        }
      break;

      case AVMEDIA_TYPE_AUDIO :
        // PDBG("[%s] audio st=%d", objname(enc), stream_index);

        if ( (status = encode_audio(enc, frame)) ) {
          PDBG("[%s] encode_audio() fails: %s", objname(enc), av_err2str(status));
        }
      break;

      default :
      break;
    }

    ff_avframe_unref(frame);
  }


end:

  PDBG("[%s] C ffgop_delete_listener(gl=%p)", objname(enc), gl);
  ffgop_delete_listener(&gl);
  PDBG("[%s] R ffgop_delete_listener(gl=%p)", objname(enc), gl);

  PDBG("[%s] C ffgop_put_eof()", objname(enc));
  ffgop_put_eof(&enc->gop, status);
  PDBG("[%s] R ffgop_put_eof()", objname(enc));

  PDBG("[%s] C av_frame_free()", objname(enc));
  av_frame_free(&frame);
  PDBG("[%s] R av_frame_free()", objname(enc));

  PDBG("C free(enc->os=%p)", enc->os);
  free(enc->os);
  enc->os = NULL;
  PDBG("R free(enc->os=%p)", enc->os);

  PDBG("C ffmpeg_close_output(enc->oc=%p)", enc->oc);
  ffmpeg_close_output(&enc->oc);
  PDBG("R ffmpeg_close_output(enc->oc=%p)", enc->oc);

  release_object(&enc->base);
}


int ff_create_encoder(struct ffobject ** obj, const struct ff_create_encoder_args * args)
{
  static const struct ff_object_iface iface = {
    .on_add_ref = NULL,
    .on_release = on_release_encoder,
    .get_format_context = get_encoded_format_context,
    .get_gop = get_encoded_gop,
  };

  struct ffenc * enc = NULL;
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

  if ( !(enc = ff_create_object(sizeof(struct ffenc), object_type_encoder, args->name, &iface)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }


  if ( (status = ffgop_init(&enc->gop, 32, ffgop_pkt)) ) {
    goto end;
  }


//  if ( (status = ffgop_create_listener(igop, &enc->gl)) ) {
//    goto end;
//  }


  if ( (status = start_encoding(enc, args->source, args->params)) ) {
    goto end;
  }

  add_object_ref(&enc->base);

  if ( !co_schedule(encoder_thread, enc, 64 * 1024) ) {
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
