/*
 * ffmpeg.c
 *
 *  Created on: Jul 25, 2014
 *      Author: amyznikov
 *
 *  References:
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
#include <time.h>
#include <pthread.h>
#include "debug.h"

#define PICB       PDBG
#define PCRITICAL  PDBG
#define PERROR     PDBG
#define PDEBUG     PDBG
#define PINFO      PDBG

int64_t ffmpeg_gettime(void)
{
  struct timespec tm;
  clock_gettime(CLOCK_MONOTONIC, &tm);
  return ((int64_t)tm.tv_sec * 1000000 + (int64_t)tm.tv_nsec / 1000);
}


void ffmpeg_usleep( int64_t usec )
{
  struct timespec rqtp;
  rqtp.tv_sec = usec / 1000000;
  rqtp.tv_nsec = (usec - (int64_t)rqtp.tv_sec * 1000000) * 1000;
  clock_nanosleep(CLOCK_MONOTONIC, 0, &rqtp, NULL);
}


int ffmpeg_timeout_interrupt_callback(void * arg)
{
  struct timeout_interrupt_callback_s * cb = arg;
  int interrupt;

  if ( (interrupt = check_interrupt(&cb->origcb)) ) {
    PERROR("parrent callback interrupted");
  }
  else if ( ffmpeg_gettime() >= cb->end_time ) {
    PERROR("timeout expired");
    interrupt = 1;
  }

  PICB("interrupt=%d", interrupt);
  return interrupt;
}




////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int ffmpeg_apply_opts(const char * options, void * obj, int ignore_not_found)
{
  static const char delims[] = " \t\n\r";

  char opts[strlen(options) + 1];
  const char * opt, * val;

  int status = 0;

  opt = strtok(strcpy(opts, options), delims);
  while ( opt ) {

    if ( *opt != '-' ) {
      PCRITICAL("Option must start with '-' symbol. Got:%s", opt);
      goto next;
    }

    if ( !*(++opt) ) {
      goto next;
    }

    if ( !(val = strtok(NULL, delims)) ) {
      PERROR("missing argument for option '%s': option ignored",  opt);
      goto next;
    }

    if ( (status = av_opt_set(obj, opt, val, AV_OPT_SEARCH_CHILDREN)) != 0 ) {
      PERROR("av_opt_set(%s=%s) FAILS: %s", opt, val, av_err2str(status));
      if ( ignore_not_found ) {
        status = 0;
      }
      goto next;
    }

next:
    opt = strtok(NULL, delims);
  }

  return status;
}


int ffmpeg_parse_opts( const char * options, AVDictionary ** dict)
{
  static const char delims[] = " \t\n\r";

  char opts[strlen(options) + 1];
  const char * opt, * val;

  int status = 0;

  opt = strtok(strcpy(opts, options), delims);
  while ( opt ) {

    if ( *opt != '-' ) {
      PERROR("Option must start with '-' symbol. Got:%s", opt);
      goto next;
    }

    if ( !*(++opt) ) {
      goto next;
    }

    if ( !(val = strtok(NULL, delims)) ) {
      PERROR("missing argument for option '%s': option ignored", opt);
      goto next;
    }

    av_dict_set(dict, opt, val, 0);

next:
    opt = strtok(NULL, delims);
  }

  return status;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int ffmpeg_probe_input(AVFormatContext * ic, int fast)
{
  int status = 0;

  if ( fast ) {
    ffmpeg_apply_opts("-fpsprobesize 0", ic, 1);
  }

  PDEBUG("avformat_find_stream_info()");
  if ( (status = avformat_find_stream_info(ic, NULL)) ) {
    PERROR("avformat_find_stream_info() fails: %s", av_err2str(status));
  }

  return status;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static void check_input_name(const char ** filename, const char ** format)
{
  if ( *filename ) {

    static const struct {
      const char * fname;
      const char * prefix;
    } cc[] = {
      { "v4l2", "v4l2://" },
      { "video4linux2", "video4linux2://" }
    };

    uint i, n;

    for ( i = 0; i < sizeof(cc) / sizeof(cc[0]); ++i ) {
      if ( strncmp(*filename, cc[i].prefix, n = strlen(cc[i].prefix)) == 0 ) {
        *filename += n;
        if ( !*format ) {
          *format = cc[i].fname;
        }
        break;
      }
    }
  }
}

int ffmpeg_alloc_input_context(AVFormatContext **ic, const char * filename, const char * format, AVIOContext * pb,
    AVIOInterruptCB * icb, const char * options, AVDictionary ** dicout)
{
  AVInputFormat * fmt = NULL;
  AVDictionary * dict = NULL;
  char opts[strlen(options) + 1];

  int status = 0;

  check_input_name(&filename, &format);

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

    static const char delims[] = " \t\n\r";
    const char * opt;
    const char * val;
    int s;

    strcpy(opts, options);

    opt = strtok(opts, delims);
    while ( opt ) {

      if ( *opt == '-' && !*(++opt) ) {
        opt = strtok(NULL, delims);
        continue;
      }

      if ( !(val = strtok(NULL, delims)) ) {
        PERROR("[%s] missing argument for option '%s': option ignored", filename, opt);
        break;
      }

      if ( (s = av_opt_set(*ic, opt, val, AV_OPT_SEARCH_CHILDREN)) == 0 ) {
        //PDEBUG("[%s] av_opt_set(%s=%s) OK", filename, opt, val);
      }
      else if ( s == AVERROR_OPTION_NOT_FOUND ) {
        av_dict_set(&dict, opt, val, 0);
        //PDEBUG("[%s] av_dict_set(%s=%s)", filename, opt, val);
      }
      else {
        PERROR("[%s] av_opt_set(%s=%s) FAILS: %s", filename, opt, val, av_err2str(s));
      }

      if ( !format && strcmp(opt, "f") == 0 ) {
        format = val;
      }

      opt = strtok(NULL, delims);
    }
  }

  if ( format && !(fmt = av_find_input_format(format)) ) {
    PERROR("[%s] av_find_input_format(%s) fails", filename, format);
    status = AVERROR_DEMUXER_NOT_FOUND;
    goto end;
  }

  if ( fmt ) {
    (*ic)->iformat = fmt;
  }

end:

  if ( status ) {
    avformat_close_input(ic);
  }
  else if ( dicout && dict ) {
    av_dict_copy(dicout, dict, 0);
  }

  av_dict_free(&dict);

  return status;
}



int ffmpeg_open_input(AVFormatContext **ic, const char * filename, const char * format, AVIOContext * pb,
    AVIOInterruptCB * icb, const char * options )
{
  AVDictionary * dict = NULL;

  int status;

  check_input_name(&filename, &format);

  if ( (status = ffmpeg_alloc_input_context(ic, filename, format, pb, icb, options, &dict)) ) {
    PCRITICAL("[%s] ffmpeg_alloc_format_context(format=%s) fails: %s", filename, format, av_err2str(status));
    goto end;
  }

  (*ic)->flags |= AVFMT_FLAG_DISCARD_CORRUPT; // AVFMT_FLAG_NONBLOCK |

  PINFO("[%s] avformat_open_input()", filename);
  if ( (status = avformat_open_input(ic, filename, NULL, &dict)) < 0 ) {
    if ( check_interrupt(icb) ) {
      status = AVERROR_EXIT;
    }
    PCRITICAL("[%s] avformat_open_input() fails: %s", filename, av_err2str(status));
    goto end;
  }

end:

  if ( status ) {
    avformat_close_input(ic);
  }

  if ( dict ) {
    av_dict_free(&dict);
  }

  PINFO("[%s]: %s", filename, av_err2str(status));
  return status;
}


void ffmpeg_close_input(AVFormatContext ** ic)
{
  if ( ic && *ic ) {
    unsigned int i;
    for ( i = 0; i < (*ic)->nb_streams; ++i ) {
      PDEBUG("avcodec_close(i=%u)",i);
      avcodec_close((*ic)->streams[i]->codec);
    }
    PDEBUG("avformat_close_input()");
    avformat_close_input(ic);
  }
}



//static void free_packet_buffer(AVPacketList **pkt_buf, AVPacketList **pkt_buf_end)
//{
//  while ( *pkt_buf ) {
//    AVPacketList *pktl = *pkt_buf;
//    *pkt_buf = pktl->next;
//    av_free_packet(&pktl->pkt);
//    av_freep(&pktl);
//  }
//  *pkt_buf_end = NULL;
//}

void ffmpeg_close_output(AVFormatContext ** oc)
{
  unsigned int i;

  if ( oc && *oc ) {

    for ( i = 0; i < (*oc)->nb_streams; ++i ) {
      avcodec_close((*oc)->streams[i]->codec);
    }

//    if ( (*oc)->packet_buffer ) {
//      free_packet_buffer(&(*oc)->packet_buffer, &(*oc)->packet_buffer_end);
//    }


    PDEBUG("*oc->pb=%p", (*oc)->pb);

    avformat_free_context(*oc);
    *oc = NULL;
  }
}

int ffmpeg_copy_stream(const AVStream * is, AVStream * os, const struct AVOutputFormat * oformat)
{
  int pts_wrap_bits = 33;
  uint pts_num = 1;
  uint pts_den = 90000;

  int status;

  os->id = is->id;
  os->avg_frame_rate = is->avg_frame_rate;
  os->sample_aspect_ratio = is->sample_aspect_ratio;
  os->disposition = is->disposition;
  os->time_base = is->time_base;

  if ( (status = avcodec_copy_context(os->codec, is->codec)) == 0 ) {

    os->codec->codec_tag = 0;
    os->codec->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    os->codec->sample_fmt = is->codec->sample_fmt;

    if ( oformat ) {

      static const struct {
        const char * format_name;
        int pts_wrap_bits ;
        uint pts_num;
        uint pts_den;
      } ptsdefs[] = {
          {"matroska",64, 1, 1000},
          {"webm",64, 1, 1000},
          {"flv",32, 1, 1000},
          {"asf",32, 1, 1000},
          {"ffm",64, 1, 1000000},
          {"mpeg", 64, 1, 90000},
          {"vcd", 64, 1, 90000},
          {"svcd", 64, 1, 90000},
          {"dvd", 64, 1, 90000},
          {"msm", 64, 1, 1000},
          {"mp4", 64, 1, 10000},
      };

      for ( uint i = 0; i < sizeof(ptsdefs) / sizeof(ptsdefs[0]); ++i ) {
        if ( strcmp(oformat->name, ptsdefs[i].format_name) == 0 ) {
          pts_wrap_bits = ptsdefs[i].pts_wrap_bits;
          pts_num = ptsdefs[i].pts_num;
          pts_den = ptsdefs[i].pts_den;
        }
      }

      if ( oformat->flags & AVFMT_GLOBALHEADER ) {
        os->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
      }
      else {
        os->codec->flags &= ~CODEC_FLAG_GLOBAL_HEADER;
      }
    }

    /** Warning: internal ffmpeg function */
    extern void avpriv_set_pts_info(AVStream * s,
        int pts_wrap_bits,
        unsigned int pts_num,
        unsigned int pts_den );

    avpriv_set_pts_info(os, pts_wrap_bits, pts_num, pts_den);
  }

  return status;
}

int ffmpeg_copy_streams(const AVFormatContext * ic, AVFormatContext * oc)
{
  int status = 0;

  for ( uint i = 0, status = 0; i < ic->nb_streams; ++i ) {
    if ( !avformat_new_stream(oc, NULL) ) {
      status = AVERROR(ENOMEM);
      break;
    }
    if ( (status = ffmpeg_copy_stream(ic->streams[i], oc->streams[i], oc->oformat)) ) {
      break;
    }
  }

  return status;
}


//int ffmpeg_copy_streams(const AVFormatContext * ic, AVFormatContext * oc)
//{
//  int pts_wrap_bits = 33;
//  unsigned int pts_num = 1;
//  unsigned int pts_den = 90000;
//  unsigned int i;
//
//  int status;
//
//  if ( oc->oformat ) {
//
//    static const struct {
//      const char * format_name;
//      int pts_wrap_bits ;
//      unsigned int pts_num;
//      unsigned int pts_den;
//    } ptsdefs[] = {
//        {"matroska",64, 1, 1000},
//        {"webm",64, 1, 1000},
//        {"flv",32, 1, 1000},
//        {"asf",32, 1, 1000},
//        {"ffm",64, 1, 1000000},
//        {"mpeg", 64, 1, 90000},
//        {"vcd", 64, 1, 90000},
//        {"svcd", 64, 1, 90000},
//        {"dvd", 64, 1, 90000},
//        {"msm", 64, 1, 1000},
//        {"mp4", 64, 1, 10000},
//    };
//
//    for ( i = 0; i < sizeof(ptsdefs) / sizeof(ptsdefs[0]); ++i ) {
//      if ( strcmp(oc->oformat->name, ptsdefs[i].format_name) == 0 ) {
//        pts_wrap_bits = ptsdefs[i].pts_wrap_bits;
//        pts_num = ptsdefs[i].pts_num;
//        pts_den = ptsdefs[i].pts_den;
//      }
//    }
//  }
//
//  for ( i = 0, status = 0; i < ic->nb_streams; ++i ) {
//
//    if ( !avformat_new_stream(oc, NULL) ) {
//      status = AVERROR(ENOMEM);
//      break;
//    }
//
//    oc->streams[i]->id = ic->streams[i]->id;
//    oc->streams[i]->avg_frame_rate = ic->streams[i]->avg_frame_rate;
//    oc->streams[i]->sample_aspect_ratio = ic->streams[i]->sample_aspect_ratio;
//    oc->streams[i]->disposition = ic->streams[i]->disposition;
//    oc->streams[i]->time_base = ic->streams[i]->time_base;
//
//    if ( (status = avcodec_copy_context(oc->streams[i]->codec, ic->streams[i]->codec)) ) {
//      break;
//    }
//
//
//    oc->streams[i]->codec->codec_tag = 0;
//    oc->streams[i]->codec->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
//
//    if ( oc->oformat ) {
//      if ( oc->oformat->flags & AVFMT_GLOBALHEADER ) {
//        oc->streams[i]->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
//      }
//      else {
//        oc->streams[i]->codec->flags &= ~CODEC_FLAG_GLOBAL_HEADER;
//      }
//    }
//
//    /** Warning: internal ffmpeg function */
//    extern void avpriv_set_pts_info(AVStream * s,
//        int pts_wrap_bits,
//        unsigned int pts_num,
//        unsigned int pts_den );
//
//    avpriv_set_pts_info(oc->streams[i], pts_wrap_bits, pts_num, pts_den);
//  }
//
//  return status;
//}


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













AVFrame * ffmpeg_video_frame_create(enum AVPixelFormat fmt, int cx, int cy)
{
  AVFrame * frame;
//  const AVPixFmtDescriptor * desc;
  //int i;

  int status;

  if ( !(frame = av_frame_alloc()) ) {
    PCRITICAL("av_frame_alloc() fails");
    goto end;
  }

  frame->format = fmt;
  frame->width = cx;
  frame->height = cy;

  if ( (status = av_frame_get_buffer(frame, 64)) < 0 ) {
    PCRITICAL("av_frame_get_buffer() fails: %s", av_err2str(status));
    av_frame_free(&frame);
    frame = NULL;
    goto end;
  }

//  if ( (desc = av_pix_fmt_desc_get(frame->format)) ) {
//    for ( i = 0; i < 4 && frame->linesize[i]; i++ ) {
//      int h = FFALIGN(frame->height, 64);
//      if ( i == 1 || i == 2 ) {
//        h = FF_CEIL_RSHIFT(h, desc->log2_chroma_h);
//      }
//      memset(frame->data[i], 0, frame->linesize[i] * h + 16 + 16/*STRIDE_ALIGN*/- 1);
//    }
//  }

end:

  return frame;
}


AVFrame * ffmpeg_audio_frame_create(enum AVSampleFormat fmt, int sample_rate, int nb_samples, int channels,
    uint64_t channel_layout)
{
  AVFrame * frame;
  int status;

  if ( !(frame = av_frame_alloc()) ) {
    PCRITICAL("av_frame_alloc() fails");
    goto end;
  }

  frame->format = fmt;
  frame->nb_samples = nb_samples;
  frame->channel_layout = channel_layout;
  frame->channels = channels;
  frame->sample_rate = sample_rate;

  if ( (status = av_frame_get_buffer(frame, 64)) < 0 ) {
    PCRITICAL("av_frame_get_buffer() fails: %s", av_err2str(status));
    av_frame_free(&frame);
    frame = NULL;
    goto end;
  }

end:

  return frame;
}


int ffmpeg_copy_frame( AVFrame * dst, const AVFrame * src )
{
  int status;

  if ( dst->format != src->format || dst->format < 0 ) {
    PCRITICAL("dst->format=%d src->format=%d", dst->format, src->format);
    status = AVERROR(EINVAL);
  }
  else if ( dst->nb_samples != src->nb_samples || dst->channels != src->channels
      || dst->channel_layout != src->channel_layout ) {

    PCRITICAL("dst->nb_samples=%d  src->nb_samples=%d", dst->nb_samples, src->nb_samples);
    PCRITICAL("dst->channels=%d src->channels=%d\n", dst->channels, src->channels);
    PCRITICAL("dst->channel_layout=%"PRIu64" src->channel_layout=%"PRIu64"\n", dst->channel_layout, src->channel_layout);

    status = AVERROR(EINVAL);
  }
  else if ( (status = av_frame_copy(dst, src)) < 0 ) {
    PCRITICAL("av_frame_copy() fails: %s", av_err2str(status));
  }
  else if ( (status = av_frame_copy_props(dst, src)) ) {
    PCRITICAL("av_frame_copy_props() fails: %s", av_err2str(status));
  }

  return status;
}



int ffmpeg_decode_frame(AVCodecContext * codec, AVPacket * pkt, AVFrame * frm, int * gotframe)
{
  int status = 0;

  switch ( codec->codec_type )
  {
  case AVMEDIA_TYPE_VIDEO:
    status = avcodec_decode_video2(codec, frm, gotframe, pkt);
    break;

  case AVMEDIA_TYPE_AUDIO:
    status = avcodec_decode_audio4(codec, frm, gotframe, pkt);
    break;

  default:
    status = AVERROR(ENOTSUP);
    break;
  }

  return status;
}



/////////////

const int * ffmpeg_get_supported_samplerates(const AVCodec * enc, const AVOutputFormat * ofmt)
{
  const int * supported_samplerates = enc->supported_samplerates;

  if ( !supported_samplerates ) {
    if ( strcmp(ofmt->name, "flv") == 0 ) {
      static const int flv_samplerates[] = { 44100, 22050, 11025, 0 };
      supported_samplerates = flv_samplerates;
    }
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

int ffmpeg_is_samplerate_supported(const AVCodec * enc, const AVOutputFormat * ofmt, int sample_rate)
{
  const int * supported_samplerates;
  int i;

  if ( !(supported_samplerates = ffmpeg_get_supported_samplerates(enc, ofmt)) ) {
    return 1; /* assume yes if unknown */
  }

  for ( i = 0; supported_samplerates[i] != 0; ++i ) {
    if ( supported_samplerates[i] == sample_rate ) {
      return 1;
    }
  }

  return 0;
}


/** Select best pixel/sample format for encoder */
int ffmpeg_select_best_format(const int fmts[], int fmt)
{
  int enc_fmt = -1;

  if ( fmts ) {
    int j = 0;

    while ( fmts[j] != -1 && fmts[j] != fmt ) {
      ++j;
    }
    if ( fmts[j] == fmt ) {
      enc_fmt = fmt;
    }
    else {
      enc_fmt = fmts[0];
    }
  }

  return enc_fmt;
}




int64_t ffmpeg_get_media_file_duration(const char * source)
{
  AVFormatContext * ic = NULL;
  int64_t duration = -1;
  int status;

  status = avformat_open_input(&ic, source, NULL, NULL);
  if ( status == 0 ) {
    duration = ic->duration;
  }

  avformat_free_context(ic);

  return duration;
}




/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


struct ffmpeg_http_input_context {
  int (*recv)(void * cookie, int so, void * buf, int buf_size);
  void * cookie;
  int so;
  int chunksize;
  int bytesreceived;
};


static ssize_t ffmpeg_http_getline(struct ffmpeg_http_input_context * ctx, char msg[], size_t size)
{
  size_t cb = 0;
  ssize_t status = 0;

  while ( cb < size && (status = ctx->recv(ctx->cookie, ctx->so, &msg[cb], 1)) == 1 ) {
    ++cb;
    if ( msg[cb - 1] == 0 ) {
      break;
    }

    if ( msg[cb - 1] == '\n' ) {
      if ( cb < 2 || msg[cb - 2] != '\r' ) {
        msg[cb - 1] = 0;
      }
      else {
        msg[cb - 2] = 0;
        --cb;
      }

      break;
    }
  }

  return status < 0 ? status : (ssize_t) (cb);
}


static int ffmpeg_http_read_chunksize(struct ffmpeg_http_input_context * ctx)
{
  char line[256] = "";
  int size = -1;

  while ( (size = ffmpeg_http_getline(ctx, line, sizeof(line))) == 1 ) {
  }

  if ( size < 0 || size >= (int) sizeof(line) ) {
    size = -1;
  }
  else if ( size && sscanf(line, "%X", &size) != 1 ) {
    size = -1;
  }

  return size;
}

static int ffmpeg_http_read_packet(void * opaque, uint8_t * buf, int buf_size)
{
  struct ffmpeg_http_input_context * ctx;

  int size, bytes_to_read, rest;

  ctx = opaque;

  if ( ctx->chunksize < 0 ) {
    size = ctx->recv(ctx->cookie, ctx->so, buf, buf_size);
  }
  else {

    if ( (rest = ctx->chunksize - ctx->bytesreceived) <= 0 ) {
      ctx->bytesreceived = 0;
      if ( (ctx->chunksize = ffmpeg_http_read_chunksize(ctx)) < 0 ) {
        return -1;
      }
      if ( ctx->chunksize == 0 ) {
        return 0;
      }
      rest = ctx->chunksize;
    }

    bytes_to_read = rest < buf_size ? rest : buf_size;
    if ( (size = ctx->recv(ctx->cookie, ctx->so, buf, bytes_to_read)) > 0 ) {
      ctx->bytesreceived += size;
    }
  }

  return size;
}


int ffmpeg_create_http_input_context(AVIOContext ** pb,
    int (*recv)(void * cookie, int so, void * buf, int buf_size), void * cookie, int so,
    bool chunked_encoding)
{
  struct ffmpeg_http_input_context * ctx = NULL;

  uint8_t * iobuf = NULL;
  uint32_t iobufsz = 256 * 1024;

  int status = 0;

  if ( !(ctx = av_mallocz(sizeof(struct ffmpeg_http_input_context))) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(iobuf = av_mallocz(iobufsz)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  if ( !(*pb = avio_alloc_context(iobuf, iobufsz, 0, ctx, ffmpeg_http_read_packet, NULL, NULL)) ) {
    status = AVERROR(ENOMEM);
    goto end;
  }

  ctx->so = so;
  ctx->recv = recv;
  ctx->cookie = cookie;
  ctx->chunksize = chunked_encoding ? 0 : -1;

  (*pb)->max_packet_size = iobufsz;

end:

  if ( status ) {
    av_free(*pb);
    av_free(iobuf);
    av_free(ctx);
    *pb = NULL;
  }

  return status;
}


int ffmpeg_http_close_input_context(AVIOContext **pb)
{
  if ( pb && *pb ) {

    struct ffmpeg_http_input_context * ctx = (*pb)->opaque;
    if ( ctx && ctx->so != -1 ) {
      close(ctx->so);
    }

    av_free((*pb)->buffer);
    av_free(ctx);
    av_free(*pb);
    *pb = NULL;
  }
  return 0;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////





//int64_t ffmpeg_get_byte_position_for_time(const char * source, int64_t timestamp)
//{
//  AVFormatContext * ic = NULL;
//  int status;
//  int64_t pos = 0;
//
//  status = avformat_open_input(&ic, source, NULL, NULL);
//  if ( status == 0 ) {
//
//    status = av_seek_frame(ic, -1, timestamp, 0);
//    if ( status < 0 ) {
//      PCRITICAL("av_seek_frame() fails: %s", av_err2str(status));
//    }
//    else {
//
//
//      pos = avio_tell(ic->pb);
//    }
//  }
//
//  avformat_close_input(&ic);
//
//  return pos;
//}


void ffmpeg_dump_codec_context(AVCodecContext * ctx)
{
  FILE * fp;

  fp = fopen("codec.dat", "w");
  if ( !fp ) {
    PCRITICAL("fopen(codec.dat) fails: %s", strerror(errno));
    return;
  }

#define PRNPARAM(fmt,p) \
  fprintf(fp,"" #p "=" fmt "\n",ctx->p)

  fprintf(fp, "codec_tag=%s\n",fcc2str(ctx->codec_tag));
  // fprintf(fp, "stream_codec_tag=%s\n",fcc2str(ctx->stream_codec_tag));

  PRNPARAM("%p",av_class);
  PRNPARAM("%d",log_level_offset);
  PRNPARAM("%d",codec_type); /* see AVMEDIA_TYPE_xxx */
  PRNPARAM("%p",codec);
  PRNPARAM("%d",codec_id); /* see AV_CODEC_ID_xxx */
  PRNPARAM("%p",priv_data);
  PRNPARAM("%p",internal);
  PRNPARAM("%p",opaque);
  PRNPARAM("%"PRId64"",bit_rate);
  PRNPARAM("%d",bit_rate_tolerance);
  PRNPARAM("%d",global_quality);
  PRNPARAM("%d",compression_level);
  PRNPARAM("%d",flags);
  PRNPARAM("%d",flags2);
  PRNPARAM("%p",extradata);
  PRNPARAM("%d",extradata_size);
  PRNPARAM("%d",time_base.num);
  PRNPARAM("%d",time_base.den);
  PRNPARAM("%d",ticks_per_frame);
  PRNPARAM("%d",delay);
  PRNPARAM("%d",width);
  PRNPARAM("%d",height);
  PRNPARAM("%d",coded_width);
  PRNPARAM("%d",coded_height);
  PRNPARAM("%d",gop_size);
  PRNPARAM("%d",pix_fmt);
//  PRNPARAM("%d",me_method);
  PRNPARAM("%d",max_b_frames);
  PRNPARAM("%g",b_quant_factor);
//  PRNPARAM("%d",rc_strategy);
//  PRNPARAM("%d",b_frame_strategy);
  PRNPARAM("%g",b_quant_offset);
  PRNPARAM("%d",has_b_frames);
//  PRNPARAM("%d",mpeg_quant);
  PRNPARAM("%g",i_quant_factor);
  PRNPARAM("%g",i_quant_offset);
  PRNPARAM("%g",lumi_masking);
  PRNPARAM("%g",temporal_cplx_masking);
  PRNPARAM("%g",spatial_cplx_masking);
  PRNPARAM("%g",p_masking);
  PRNPARAM("%g",dark_masking);
  PRNPARAM("%d",slice_count);
//  PRNPARAM("%d",prediction_method);
  PRNPARAM("%d",sample_aspect_ratio.num);
  PRNPARAM("%d",sample_aspect_ratio.den);
  PRNPARAM("%d",me_cmp);
  PRNPARAM("%d",me_sub_cmp);
  PRNPARAM("%d",mb_cmp);
  PRNPARAM("%d",ildct_cmp);
  PRNPARAM("%d",dia_size);
  PRNPARAM("%d",last_predictor_count);
//  PRNPARAM("%d",pre_me);
  PRNPARAM("%d",me_pre_cmp);
  PRNPARAM("%d",pre_dia_size);
  PRNPARAM("%d",me_subpel_quality);
  PRNPARAM("%d",me_range);
//  PRNPARAM("%d",intra_quant_bias);
//  PRNPARAM("%d",inter_quant_bias);
  PRNPARAM("%d",slice_flags);
  PRNPARAM("%d",mb_decision);
  PRNPARAM("%p",intra_matrix);
  PRNPARAM("%p",inter_matrix);
//  PRNPARAM("%d",scenechange_threshold);
//  PRNPARAM("%d",noise_reduction);
  //PRNPARAM("%d",me_threshold);
  //PRNPARAM("%d",mb_threshold);
  PRNPARAM("%d",intra_dc_precision);
  PRNPARAM("%d",skip_top);
  PRNPARAM("%d",skip_bottom);
  //PRNPARAM("%g",border_masking);
  PRNPARAM("%d",mb_lmin);
  PRNPARAM("%d",mb_lmax);
//  PRNPARAM("%d",me_penalty_compensation);
  PRNPARAM("%d",bidir_refine);
//  PRNPARAM("%d",brd_scale);
  PRNPARAM("%d",keyint_min);
  PRNPARAM("%d",refs);
//  PRNPARAM("%d",chromaoffset);
  PRNPARAM("%d",mv0_threshold);
//  PRNPARAM("%d",b_sensitivity);
  PRNPARAM("%d",color_primaries);
  PRNPARAM("%d",color_trc);
  PRNPARAM("%d",colorspace);
  PRNPARAM("%d",color_range);
  PRNPARAM("%d",chroma_sample_location);
  PRNPARAM("%d",slices);
  PRNPARAM("%d",field_order);
  PRNPARAM("%d",sample_rate); ///< samples per second
  PRNPARAM("%d",channels);    ///< number of audio channels
  PRNPARAM("%d",sample_fmt);  ///< sample format
  PRNPARAM("%d",frame_size);
  PRNPARAM("%d",frame_number);
  PRNPARAM("%d",block_align);
  PRNPARAM("%d",cutoff);
  PRNPARAM("%"PRId64,channel_layout);
  PRNPARAM("%d",audio_service_type);
  PRNPARAM("%d",request_sample_fmt);
  PRNPARAM("%d",refcounted_frames);
  PRNPARAM("%g",qcompress);
  PRNPARAM("%g",qblur);
  PRNPARAM("%d",qmin);
  PRNPARAM("%d",qmax);
  PRNPARAM("%d",max_qdiff);
//  PRNPARAM("%g",rc_qsquish);
//  PRNPARAM("%g",rc_qmod_amp);
//  PRNPARAM("%d",rc_qmod_freq);
  PRNPARAM("%d",rc_buffer_size);
  PRNPARAM("%d",rc_override_count);
  PRNPARAM("%p",rc_override);
//  PRNPARAM("%s",rc_eq);
  PRNPARAM("%"PRId64"",rc_max_rate);
  PRNPARAM("%"PRId64"",rc_min_rate);
//  PRNPARAM("%g",rc_buffer_aggressivity);
//  PRNPARAM("%g",rc_initial_cplx);
  PRNPARAM("%g",rc_max_available_vbv_use);
  PRNPARAM("%g",rc_min_vbv_overflow_use);
  PRNPARAM("%d",rc_initial_buffer_occupancy);
//  PRNPARAM("%d",coder_type);
//  PRNPARAM("%d",context_model);
//  PRNPARAM("%d",lmin);
//  PRNPARAM("%d",lmax);
//  PRNPARAM("%d",frame_skip_threshold);
//  PRNPARAM("%d",frame_skip_factor);
//  PRNPARAM("%d",frame_skip_exp);
//  PRNPARAM("%d",frame_skip_cmp);
  PRNPARAM("%d",trellis);
//  PRNPARAM("%d",min_prediction_order);
//  PRNPARAM("%d",max_prediction_order);
//  PRNPARAM("%"PRId64,timecode_frame_start);
//  PRNPARAM("%d",rtp_payload_size);   /* The size of the RTP payload: the coder will  */
//  PRNPARAM("%d",mv_bits);
//  PRNPARAM("%d",header_bits);
//  PRNPARAM("%d",i_tex_bits);
//  PRNPARAM("%d",p_tex_bits);
//  PRNPARAM("%d",i_count);
//  PRNPARAM("%d",p_count);
//  PRNPARAM("%d",skip_count);
//  PRNPARAM("%d",misc_bits);
//  PRNPARAM("%d",frame_bits);
  PRNPARAM("%s",stats_out);
  PRNPARAM("%s",stats_in);
  PRNPARAM("%d",workaround_bugs);
  PRNPARAM("%d",strict_std_compliance);
  PRNPARAM("%d",error_concealment);
  PRNPARAM("%d",debug);
  PRNPARAM("%d",err_recognition);
  PRNPARAM("%"PRId64,reordered_opaque);
  PRNPARAM("%p",hwaccel);
  PRNPARAM("%p",hwaccel_context);
  PRNPARAM("%d",dct_algo);
  PRNPARAM("%d",idct_algo);
  PRNPARAM("%d",bits_per_coded_sample);
  PRNPARAM("%d",bits_per_raw_sample);
//  PRNPARAM("%p",coded_frame);
  PRNPARAM("%d",thread_count);
  PRNPARAM("%d",thread_type);
  PRNPARAM("%d",active_thread_type);
  PRNPARAM("%d",thread_safe_callbacks);
  PRNPARAM("%d",nsse_weight);
  PRNPARAM("%d",profile);
  PRNPARAM("%d",level);
  PRNPARAM("%d",skip_loop_filter);
  PRNPARAM("%d",skip_idct);
  PRNPARAM("%d",skip_frame);
  PRNPARAM("%p",subtitle_header);
  PRNPARAM("%d",subtitle_header_size);
//  PRNPARAM("%"PRIu64,vbv_delay);
//  PRNPARAM("%d",side_data_only_packets);
  PRNPARAM("%d",pkt_timebase.num);
  PRNPARAM("%d",pkt_timebase.den);
  PRNPARAM("%p",codec_descriptor);
  PRNPARAM("%d",lowres);
  PRNPARAM("%"PRId64,pts_correction_num_faulty_pts);
  PRNPARAM("%"PRId64,pts_correction_num_faulty_dts);
  PRNPARAM("%"PRId64,pts_correction_last_pts);
  PRNPARAM("%"PRId64,pts_correction_last_dts);
  PRNPARAM("%s",sub_charenc);
  PRNPARAM("%d",sub_charenc_mode);
  PRNPARAM("%d",skip_alpha);
  PRNPARAM("%d",seek_preroll);
  PRNPARAM("%d",debug_mv);
  PRNPARAM("%p",chroma_intra_matrix);

#undef PRNPARAM

  fclose(fp);
}
