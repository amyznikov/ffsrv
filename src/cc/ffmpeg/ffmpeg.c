/*
 * ffmpeg.c
 *
 *  Created on: Jul 25, 2014
 *      Author: amyznikov
 *
 *  References:
 *
 *   ffmpeg docs:
 *     http://ffmpeg.org/ffmpeg-filters.html
 *     https://ffmpeg.org/ffmpeg-protocols.html
 *
 *   FFmpeg and H.264 Encoding Guide - H264 Rate control modes
 *     https://trac.ffmpeg.org/wiki/Encode/H.264
 *
 *   H.264 encoding guide
 *     http://www.avidemux.org/admWiki/doku.php?id=tutorial:h.264
 *
 *   X264 Settings Guide
 *     http://mewiki.project357.com/wiki/X264_Settings
 *     http://www.chaneru.com/Roku/HLS/X264_Settings.htm
 *
 *
 *   VBV Encoding
 *     http://mewiki.project357.com/wiki/X264_Encoding_Suggestions#VBV_Encoding
 *
 *
 *   CRF Guide
 *     http://slhck.info/articles/crf
 *
 *   Explanation of x264 tune
 *     http://superuser.com/questions/564402/explanation-of-x264-tune
 *
 *   Forcing keyframes with an ffmpeg expression
 *     http://paulherron.com/blog/forcing_keyframes_with_ffmpeg%20copy
 *
 *   Limit Bitrate
 *     http://veetle.com/index.php/article/view/bitrateLimit
 */


#include "ffmpeg.h"
#include "debug.h"
#include <time.h>
#include <ctype.h>


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int64_t ffmpeg_gettime_us(void)
{
  struct timespec tm;
  clock_gettime(CLOCK_MONOTONIC, &tm);
  return ((int64_t) tm.tv_sec * 1000000 + (int64_t) tm.tv_nsec / 1000 );
}


void ffmpeg_usleep( int64_t usec )
{
  struct timespec rqtp;
  rqtp.tv_sec = usec / 1000000;
  rqtp.tv_nsec = (usec - (int64_t)rqtp.tv_sec * 1000000) * 1000;
  clock_nanosleep(CLOCK_MONOTONIC, 0, &rqtp, NULL);
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void * ffmpeg_alloc_ptr_array(uint n, size_t item_size)
{
  void ** p;

  if ( (p = av_mallocz(n * sizeof(*p))) ) {

    for ( uint i = 0; i < n; ++i ) {

      if ( !(p[i] = av_mallocz(item_size)) ) {
        for ( uint j = 0; j < i; ++j ) {
          av_free(p[j]);
        }

        av_free(p);
        p = NULL;
        break;
      }
    }
  }
  return p;
}

void ffmpeg_free_ptr_array(void * a, uint n)
{
  void *** p = a;
  if ( p && *p ) {
    for ( uint i = 0; i < n; ++i ) {
      av_free((*p)[i]);
    }
    av_free(*p), *p = NULL;
  }
}




/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




int ffmpeg_parse_options(const char * options, bool remove_prefix, AVDictionary ** rv)
{
  AVDictionary * tmp = NULL;
  AVDictionaryEntry * e = NULL;

  int status = 0;

  if ( !options || !*options  ) {
    goto end;
  }

  if ( (status = av_dict_parse_string(&tmp, options, " \t", " \t", AV_DICT_MULTIKEY)) || !tmp ) {
    PDBG("av_dict_parse_string('%s') fails: %s", options, av_err2str(status));
    goto end;
  }

  while ( (e = av_dict_get(tmp, "", e, AV_DICT_IGNORE_SUFFIX )) ) {

    const char * key = e->key;
    const char * value = e->value;
    const int flags = strcmp(key, "-map") == 0 ? AV_DICT_MULTIKEY : 0;

//    PDBG("OPT: '%s' = '%s'", key, value);

    if ( remove_prefix ) {
      if ( *e->key != '-' ) {
        PDBG("Option '%s' not starting with '-'", e->key);
        status = AVERROR(EINVAL);
        break;
      }
      ++key;
    }


    if ( (status = av_dict_set(rv, key, value, flags)) < 0 ) {
      PDBG("av_dict_set('%s'='%s') fails: %s", key, value, av_err2str(status));
      break;
    }

    status = 0;
  }

end:

  av_dict_free(&tmp);

  return status;
}


static inline bool stoi(const char * s, int * x)
{
  char * ep;
  ulong y = strtol(s, &ep, 10);
  if ( ep > s && !*ep ) {
    *x = y;
    return true;
  }
  return false;
}


// opt[:m][:st]
bool ffmpeg_parse_option_name(char * key, char opt[64], char * m, int * st)
{
  char * p1, * p2;
  size_t n;
  bool fok = true;

  if ( !(p1 = strchr(key,':')) ) {
    strncpy(opt,key, 63)[63] = 0;
    return true;
  }

  n = p1 - key > 63 ? 63 : p1 - key;
  strncpy(opt,key, n)[n] = 0;

  if ( (p2 = strchr(p1 + 1,':')) ) {
    if ( (fok = (p2 - p1 == 2 && stoi(p2 + 1, st))) ) {
      *m = *(p1 + 1);
    }
  }
  else if ( isalpha(*(p1+1) )) {
    if ( (fok = !*(p1 + 2) ) ) {
      *m = *(p1 + 1);
    }
  }
  else {
    fok = stoi(p1 + 1, st);
  }

  return fok;
}

// see avformat_match_stream_specifier()
char ffmpeg_get_media_type_specifier(enum AVMediaType type)
{
  char spec;

  switch ( type ) {
    case AVMEDIA_TYPE_VIDEO :
      spec = 'v';
    break;
    case AVMEDIA_TYPE_AUDIO :
      spec = 'a';
    break;
    case AVMEDIA_TYPE_SUBTITLE :
      spec = 's';
    break;
    case AVMEDIA_TYPE_DATA:
      spec = 'd';
      break;
    case AVMEDIA_TYPE_ATTACHMENT:
      spec = 't';
      break;
    default :
      spec = 0;
      break;
  }
  return spec;
}

// see avformat_match_stream_specifier()
enum AVMediaType ffmpeg_get_media_type(char spec)
{
  enum AVMediaType type;

  switch ( spec ) {
    case 'v' :
      type = AVMEDIA_TYPE_VIDEO;
    break;
    case 'a' :
      type = AVMEDIA_TYPE_AUDIO;
    break;
    case 's' :
      type = AVMEDIA_TYPE_SUBTITLE;
    break;
    case 'd' :
      type = AVMEDIA_TYPE_DATA;
    break;
    case 't' :
      type = AVMEDIA_TYPE_ATTACHMENT;
    break;
      //case 'V': type = AVMEDIA_TYPE_VIDEO; nopic = 1; break;
    default :
      type = AVMEDIA_TYPE_UNKNOWN;
    break;
  }
  return type;

}


// See ffmpeg/cmdutils.c filter_codec_opts()
// flags is AV_OPT_FLAG_ENCODING_PARAM or AV_OPT_FLAG_DECODING_PARAM
int ffmpeg_filter_codec_opts(AVDictionary * opts, const AVCodec * codec, uint sidx, int flags, AVDictionary ** rv)
{
  const AVClass * cc = NULL;
  AVDictionaryEntry * e = NULL;
  char spec = 0;
  int status = 0;


  if ( !(spec = ffmpeg_get_media_type_specifier(codec->type)) ) {
    return AVERROR(EINVAL);
  }

  cc = avcodec_get_class();


  // opt[:spec]:sidx
  while ( (e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)) ) {

    char opt[64] = "", m = 0;
    int s = -1;

    if ( !ffmpeg_parse_option_name(e->key, opt, &m, &s) ) {
      PDBG("Invalid option syntax: '%s'", e->key);
      return AVERROR(EINVAL);
    }

    if ( s == (int)sidx ) {
      if ( m && m != spec ) {
        PDBG("Invalid media specificator in '%s'", e->key);
        return AVERROR(EINVAL);
      }
      av_dict_set(rv, opt, e->value, 0);
    }
  }


  e = NULL;
  while ( (e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)) ) {

    char opt[64] = "", m = 0;
    int s = -1;
    bool found = false;

    ffmpeg_parse_option_name(e->key, opt, &m, &s);

    if ( s >= 0 || (m && m != spec) ) {
      continue;
    }

    found = av_opt_find(&cc, opt, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ)
        || (codec->priv_class && av_opt_find((void*) &codec->priv_class, opt, NULL, flags, AV_OPT_SEARCH_FAKE_OBJ));

    if ( found ) {
      av_dict_set(rv, opt, e->value, AV_DICT_DONT_OVERWRITE);
    }
  }

  return status;
}

int ffmpeg_apply_context_opts(AVFormatContext * c, AVDictionary * opts)
{
  AVDictionaryEntry * e = NULL;
  int status = 0;

  while ( (e = av_dict_get(opts, "", e, AV_DICT_IGNORE_SUFFIX)) ) {
    if ( (status = av_opt_set(&c->av_class, e->key, e->value, AV_OPT_SEARCH_CHILDREN)) ) {
      if ( status == AVERROR_OPTION_NOT_FOUND ) {    // ignore this error
        status = 0;
      }
      else {
        PDBG("av_opt_set(%s = %s) fails: %s", e->key, e->value, av_err2str(status));
        break;
      }
    }
  }

  return status;
}


const char * ffmpeg_get_default_file_suffix(const char * format_name, char suffix[64])
{
  const AVOutputFormat * ofmt;
  char * c;

  if ( !(ofmt = av_guess_format(format_name, NULL, NULL)) || !ofmt->extensions ) {
    *suffix = 0;
  }
  else if ( !(c = strpbrk(ofmt->extensions, ",;")) ) {
    *suffix = '.';
    strncpy(suffix + 1, ofmt->extensions, 62);
  }
  else {
    *suffix = '.';
    strncpy(suffix + 1, ofmt->extensions, FFMIN(c - ofmt->extensions, 62));
  }

  return suffix;
}

const char * ff_guess_file_mime_type(const char * filename)
{
  AVOutputFormat * fmt = av_guess_format(NULL, filename, NULL);
  const char * mime_type = NULL;

  if ( fmt && fmt->mime_type && *fmt->mime_type ) {
    mime_type = fmt->mime_type;
  }
  else {
    mime_type = "application/x-octet-stream";
  }
  return mime_type;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


int ffmpeg_alloc_input_context(AVFormatContext **ic, AVIOContext * pb, AVIOInterruptCB * icb, AVDictionary ** options)
{
  AVInputFormat * fmt = NULL;
  AVDictionaryEntry * e = NULL;

  int status = 0;

  if ( !(*ic = avformat_alloc_context()) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( icb ) {
    (*ic)->interrupt_callback = *icb;
  }

  if ( pb ) {
    (*ic)->pb = pb;
    (*ic)->flags |= AVFMT_FLAG_CUSTOM_IO;
  }

  if ( options && *options ) {

    if ( (e = av_dict_get(*options, "f", e, 0)) ) {
      if ( (fmt = av_find_input_format(e->value)) ) {
        (*ic)->iformat = fmt;
      }
      else {
        status = AVERROR_DEMUXER_NOT_FOUND;
        goto end;
      }
    }

    while ( (e = av_dict_get(*options, "", e, AV_DICT_IGNORE_SUFFIX)) ) {
      if ( strcmp(e->key, "f") != 0 ) {
        av_opt_set(*ic, e->key, e->value, AV_OPT_SEARCH_CHILDREN);
      }
    }
  }

end:

  if ( status ) {
    avformat_close_input(ic);
  }

  return status;
}



int ffmpeg_open_input(AVFormatContext **ic, const char *filename, AVIOContext * pb, AVIOInterruptCB * icb,
    AVDictionary ** options)
{
  int status;

  if ( (status = ffmpeg_alloc_input_context(ic, pb, icb, options)) ) {
    PDBG("[%s] ffmpeg_alloc_format_context() fails: %s", filename, av_err2str(status));
    goto end;
  }

  (*ic)->flags |= AVFMT_FLAG_DISCARD_CORRUPT; // AVFMT_FLAG_NONBLOCK |

  if ( (status = avformat_open_input(ic, filename, NULL, options)) < 0 ) {
    if ( icb && icb->callback && icb->callback(icb->opaque) ) {
      status = AVERROR_EXIT;
    }
    goto end;
  }

end:

  if ( status ) {
    avformat_close_input(ic);
  }

  return status;
}


int ffmpeg_probe_input(AVFormatContext * ic, bool fast)
{
  int status = 0;

  if ( fast ) {
    av_opt_set(&ic->av_class, "fpsprobesize", "0", AV_OPT_SEARCH_CHILDREN);
  }

  if ( (status = avformat_find_stream_info(ic, NULL)) ) {
    PDBG("avformat_find_stream_info() fails: %s", av_err2str(status));
  }

  return status;
}


void ffmpeg_close_input(AVFormatContext ** ic)
{
  avformat_close_input(ic);
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void ffmpeg_rescale_timestamps(AVPacket * pkt, const AVRational from, const AVRational to)
{
  if ( pkt->pts != AV_NOPTS_VALUE ) {
    pkt->pts = av_rescale_q_rnd(pkt->pts, from, to, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
  }
  if ( pkt->dts != AV_NOPTS_VALUE ) {
    pkt->dts = av_rescale_q_rnd(pkt->dts, from, to, AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX);
  }
  if ( pkt->duration != 0 ) {
    pkt->duration = av_rescale_q(pkt->duration, from, to);
  }
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


int ffmpeg_decode_packet(AVCodecContext * codec, AVPacket * pkt, AVFrame * outfrm, int * gotframe)
{
  int status = 0;

  switch ( codec->codec_type )
  {
  case AVMEDIA_TYPE_VIDEO:
    status = avcodec_decode_video2(codec, outfrm, gotframe, pkt);
    break;

  case AVMEDIA_TYPE_AUDIO:
    status = avcodec_decode_audio4(codec, outfrm, gotframe, pkt);
    break;

  default:
    status = AVERROR(ENOTSUP);
    break;
  }

  return status;
}



////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int ffmpeg_create_video_frame(AVFrame ** out, enum AVPixelFormat fmt, int cx, int cy)
{
  AVFrame * frame;
  int status;

  if ( !(frame = av_frame_alloc()) ) {
    status = AVERROR(ENOMEM);
  }
  else {

    frame->format = fmt;
    frame->width = cx;
    frame->height = cy;

    if ( (status = av_frame_get_buffer(frame, 64)) ) {
      av_frame_free(&frame);
    }
  }

  *out = frame;

  return status;
}


int ffmpeg_create_audio_frame(AVFrame ** out, enum AVSampleFormat fmt, int sample_rate, int nb_samples, int channels,
    uint64_t channel_layout)
{
  AVFrame * frame;
  int status;

  if ( !(frame = av_frame_alloc()) ) {
    status = AVERROR(ENOMEM);
  }
  else {

    frame->format = fmt;
    frame->nb_samples = nb_samples;
    frame->channel_layout = channel_layout;
    frame->channels = channels;
    frame->sample_rate = sample_rate;

    if ( (status = av_frame_get_buffer(frame, 64)) ) {
      av_frame_free(&frame);
    }
  }

  *out = frame;

  return status;
}


int ffmpeg_copy_frame(AVFrame * dst, const AVFrame * src)
{
  int status;

  if ( dst->format != src->format || dst->format < 0 ) {
    status = AVERROR(EINVAL);
  }
  else if ( dst->nb_samples != src->nb_samples ) {
    status = AVERROR(EINVAL);
  }
  else if ( dst->channels != src->channels || dst->channel_layout != src->channel_layout ) {
    status = AVERROR(EINVAL);
  }
  else if ( (status = av_frame_copy(dst, src)) >= 0 && (status = av_frame_copy_props(dst, src)) > 0 ) {
    status = 0;
  }

  return status;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

/** Select best pixel/sample format for encoder */
int ffmpeg_select_best_format(const int fmts[], int default_fmt)
{
  int fmt = -1;

  if ( fmts ) {
    int j = 0;

    while ( fmts[j] != -1 && fmts[j] != default_fmt ) {
      ++j;
    }
    if ( fmts[j] == default_fmt ) {
      fmt = default_fmt;
    }
    else {
      fmt = fmts[0];
    }
  }

  return fmt;
}


bool ffmpeg_is_format_supported(const int fmts[], int fmt)
{
  if ( !fmts ) {
    return true;    // assume yes if not specified
  }

  for ( uint i = 0; fmts[i] != -1; ++i ) {
    if ( fmts[i] == fmt ) {
      return true;
    }
  }

  return false;
}



bool ffmpeg_is_channel_layout_supported(const AVCodec * codec, uint64_t channel_layout)
{
  if ( !codec->channel_layouts ) {
    return true; // assume yes if not specified
  }

  for ( uint i = 0; codec->channel_layouts[i]; ++i ) {
    if ( codec->channel_layouts[i] == channel_layout ) {
      return true;
    }
  }

  return false;
}


uint64_t ffmpeg_select_best_channel_layout(const AVCodec * codec, int nb_channels, uint64_t input_channel_layout)
{
  uint64_t best_layout = 0;

  if ( !codec->channel_layouts ) {
    return input_channel_layout;
  }

  if ( nb_channels > 0 ) {

    for ( uint i = 0; codec->channel_layouts[i]; ++i ) {
      if ( av_get_channel_layout_nb_channels(codec->channel_layouts[i]) == nb_channels ) {
        if ( codec->channel_layouts[i] == input_channel_layout ) {
          best_layout = codec->channel_layouts[i];
          break;
        }
        if ( !best_layout ) {
          best_layout = codec->channel_layouts[i];
        }
      }
    }

    if ( best_layout ) {
      return best_layout;
    }
  }


  for ( uint i = 0; codec->channel_layouts[i]; ++i ) {
    if ( codec->channel_layouts[i] == input_channel_layout ) {
      return input_channel_layout;
    }
  }

  return codec->channel_layouts[0];
}


const int * ffmpeg_get_supported_samplerates(const AVCodec * enc, const AVOutputFormat * ofmt)
{
  const int * supported_samplerates = enc->supported_samplerates;

  if ( !supported_samplerates && ofmt && ofmt->name && strcmp(ofmt->name, "flv") == 0 ) {
    static const int flv_samplerates[] = { 44100, 22050, 11025, 0 };
    supported_samplerates = flv_samplerates;
  }

  return supported_samplerates;
}


int ffmpeg_select_samplerate(const AVCodec * enc, const AVOutputFormat * ofmt, int dec_sample_rate)
{
  int enc_sample_rate = dec_sample_rate;
  int min_diff = INT_MAX;

  const int * supported_samplerates = ffmpeg_get_supported_samplerates(enc, ofmt);

  if ( supported_samplerates ) {
    int i;
    for ( i = 0; supported_samplerates[i] != 0; ++i ) {
      if ( abs(supported_samplerates[i] - dec_sample_rate) < min_diff ) {
        min_diff = abs(supported_samplerates[i] - dec_sample_rate);
        enc_sample_rate = supported_samplerates[i];
      }
    }
  }

  return enc_sample_rate;
}


bool ffmpeg_is_samplerate_supported(const AVCodec * enc, const AVOutputFormat * ofmt, int sample_rate)
{
  const int * supported_samplerates;
  int i;

  if ( !(supported_samplerates = ffmpeg_get_supported_samplerates(enc, ofmt)) ) {
    return true; /* assume yes if unknown */
  }

  for ( i = 0; supported_samplerates[i] != 0; ++i ) {
    if ( supported_samplerates[i] == sample_rate ) {
      return true;
    }
  }

  return false;
}




////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int ffstream_init(struct ffstream * dst, const AVStream * src)
{
  int status = 0;

  memset(dst, 0, sizeof(*dst));

  if ( src && (status = ffstream_copy_context(dst, src, true, true)) ) {
    ffstream_cleanup(dst);
  }

  return status;
}

void ffstream_cleanup(struct ffstream * dst)
{
  av_dict_free(&dst->metadata);
  avcodec_parameters_free(&dst->codecpar);
}


int ffstream_copy(struct ffstream * dst, const struct ffstream * src, bool copy_codecpar, bool copy_metadata )
{
  int status;

  dst->start_time = src->start_time;
  dst->duration = src->duration;
  dst->time_base = src->time_base;
  dst->sample_aspect_ratio = src->sample_aspect_ratio;
  dst->display_aspect_ratio = src->display_aspect_ratio;
  dst->discard = src->discard;
  dst->disposition = src->disposition;

  if ( copy_metadata && src->metadata && (status = av_dict_copy(&dst->metadata, src->metadata, 0)) ) {
    goto end;
  }

  if ( copy_codecpar && src->codecpar ) {

    if ( !(dst->codecpar = avcodec_parameters_alloc()) ) {
      status = AVERROR(ENOMEM);
      goto end;
    }

    if ( (status = avcodec_parameters_copy(dst->codecpar, src->codecpar)) < 0 ) {
      goto end;
    }
  }

  status = 0;

end:

  if ( status ) {
    ffstream_cleanup(dst);
  }

  return status;
}

int ffstream_copy_context(ffstream * dst, const AVStream * src, bool copy_codecpar, bool copy_metadata)
{
  int status;

  dst->start_time = src->start_time;
  dst->duration = src->duration;
  dst->time_base = src->time_base;
  dst->sample_aspect_ratio = src->sample_aspect_ratio;
  dst->display_aspect_ratio = src->display_aspect_ratio;
  dst->discard = src->discard;
  dst->disposition = src->disposition;

  if ( copy_metadata && src->metadata && (status = av_dict_copy(&dst->metadata, src->metadata, 0)) ) {
    goto end;
  }

  if ( copy_codecpar && src->codecpar ) {

    if ( !dst->codecpar && !(dst->codecpar = avcodec_parameters_alloc())) {
      status = AVERROR(ENOMEM);
      goto end;
    }

    if ( (status = avcodec_parameters_copy(dst->codecpar, src->codecpar)) < 0 ) {
      goto end;
    }
  }

  status = 0;

end:

  return status;
}



int ffstream_to_context(const ffstream * src, AVStream * os)
{
  int status;

  os->start_time = src->start_time;
  os->duration = src->duration;
  os->time_base = src->time_base;
  os->sample_aspect_ratio = src->sample_aspect_ratio;
  os->display_aspect_ratio = src->display_aspect_ratio;
  os->discard = src->discard;
  os->disposition = src->disposition;

  if ( src->metadata && (status = av_dict_copy(&os->metadata, src->metadata, 0)) ) {
    goto end;
  }

  if ( !os->codecpar && !(os->codecpar = avcodec_parameters_alloc())) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( (status = avcodec_parameters_copy(os->codecpar, src->codecpar)) < 0 ) {
    goto end;
  }

  // hack ? ffmpeg magic ?
  os->codecpar->block_align = 0;
  os->codecpar->codec_tag = 0;

  status = 0;

end:

  return status;
}




int ffstreams_to_context(const ffstream * const * streams, uint nb_streams, AVFormatContext * oc)
{
  int status = 0;

  for ( uint i = 0, status = 0; i < nb_streams; ++i ) {
    if ( !avformat_new_stream(oc, NULL) ) {
      status = AVERROR(ENOMEM);
      break;
    }
    if ( (status = ffstream_to_context(streams[i], oc->streams[i])) ) {
      break;
    }
  }

  return status;
}


int ffmpeg_create_output_context(AVFormatContext ** outctx, const char * format, const struct ffstream * const * iss,
    uint nb_streams, const char * filename)
{
  AVFormatContext * oc = NULL;
  AVOutputFormat * ofmt = NULL;
  int status;

  if ( !format || !(ofmt = av_guess_format(format, NULL, filename)) ) {
    PDBG("av_guess_format(%s) fails", format);
    status = AVERROR_MUXER_NOT_FOUND;
    goto end;
  }

  if ( (status = avformat_alloc_output_context2(&oc, ofmt, NULL, filename)) ) {
    PDBG("avformat_alloc_output_context2('%s') fails: %s", ofmt->name, av_err2str(status));
    goto end;
  }

  if ( (status = ffstreams_to_context(iss, nb_streams, oc)) ) {
    PDBG("ffstreams_to_context() fails: %s", av_err2str(status));
    goto end;
  }

end: ;

  if ( status ) {
    avformat_free_context(oc);
    oc = NULL;
  }

  *outctx = oc;
  return status;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int ffmpeg_parse_stream_mapping(const AVDictionary * opts, const uint nb_input_streams[], uint nb_inputs, struct ffstmap ** map)
{
  AVDictionaryEntry * e = NULL;
  uint nb_output_streams = 0;
  int status = 0;

  *map = NULL;


  if ( !opts || !(e = av_dict_get(opts, "map", e, 0)) ) {

    for ( uint i = 0; i < nb_inputs; ++i ) {
      nb_output_streams += nb_input_streams[i];
    }

    if ( nb_output_streams > 0 && !(*map = malloc(nb_output_streams * sizeof(struct ffstmap))) ) {
      status = AVERROR(ENOMEM);
    }
    else {
      for ( uint i = 0, k = 0; i < nb_inputs; ++i ) {
        for ( uint j = 0; j < nb_input_streams[i]; ++j ) {
          (*map)[k].iidx = i;
          (*map)[k].isidx = j;
          ++k;
        }
      }
    }
  }
  else {

    do {

      int iidx = -1, isidx = -1, np;
      struct ffstmap * temp;

      PDBG("nb_output_streams:%d e->value=%s", nb_output_streams, e->value);

      np = sscanf(e->value, "%d:%d", &iidx, &isidx);
      if ( np < 1 || iidx < 0 ) {
        PDBG("Invalid stream maping '%s' specified. np=%d", e->value, np);
        status = AVERROR(EINVAL);
        break;
      }

      if ( iidx >= (int)nb_inputs ) {
        PDBG("Not existing input referenced in stream mapping '%s'", e->value);
        status = AVERROR_STREAM_NOT_FOUND;
        break;
      }

      if ( np == 1 ) {

        if ( !(temp = realloc(*map, (nb_output_streams + nb_input_streams[iidx]) * sizeof(struct ffstmap))) ) {
          status = AVERROR(ENOMEM);
          break;
        }

        *map = temp;

        for ( uint j = 0; j < nb_input_streams[iidx]; ++j ) {
          (*map)[nb_output_streams].iidx = iidx;
          (*map)[nb_output_streams].isidx = j;
          ++nb_output_streams;
        }

      }
      else {

        if ( isidx >= (int)nb_input_streams[iidx] ) {
          PDBG("Not existing stream referenced in stream mapping '%s'", e->value);
          status = AVERROR_STREAM_NOT_FOUND;
          break;
        }

        if ( !(temp = realloc(*map, (nb_output_streams + 1) * sizeof(struct ffstmap))) ) {
          status = AVERROR(ENOMEM);
          break;
        }

        *map = temp;

        (*map)[nb_output_streams].iidx = iidx;
        (*map)[nb_output_streams].isidx = isidx;
        ++nb_output_streams;
      }


    } while ( (e = av_dict_get(opts, "map", e, 0)) );

  }

  if ( status == 0 ) {
    status = (int) (nb_output_streams);
  }
  else if ( *map ) {
    free(*map), *map = NULL;
  }

  return status;
}

int ffmpeg_map_input_stream(const struct ffstmap map[], uint msize, int iidx, int isidx, int ostidx[])
{
  int n = 0;
  for ( uint i = 0; i < msize; ++i ) {
    if ( map[i].iidx == iidx && map[i].isidx == isidx ) {
      ostidx[n++] = i;
    }
  }
  return n;
}

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int ffmpeg_timeout_interrupt_callback(void * arg)
{
  struct ff_timeout_interrupt_callback * cb = arg;
  return ffmpeg_gettime_us() >= cb->end_time;
}

void ffmpeg_init_timeout_interrupt_callback(struct ff_timeout_interrupt_callback * cb)
{
  cb->icb.callback = ffmpeg_timeout_interrupt_callback;
  cb->icb.opaque = cb;
  cb->end_time = INT64_MAX;
}

void ffmpeg_set_timeout_interrupt_callback(struct ff_timeout_interrupt_callback * cb, int64_t end_time)
{
  cb->end_time = end_time;
}


