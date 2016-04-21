/*
 * ffmpeg.h
 *
 *  Created on: Jul 12, 2014
 *      Author: amyznikov
 */


#ifndef __ffmpeg_h__
#define __ffmpeg_h__


#include <stddef.h>
#include <inttypes.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libavutil/intreadwrite.h>


#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_STREAMING_FORMAT "matroska"
//#define DEFAULT_STREAMING_FORMAT "asf"
//#define DEFAULT_STREAMING_FORMAT "mpegts"
//#define DEFAULT_STREAMING_FORMAT "rtp_mpegts"
//#define DEFAULT_STREAMING_FORMAT "m4v"
//#define DEFAULT_STREAMING_FORMAT "vob"


int64_t ffmpeg_gettime(void);
void ffmpeg_usleep(int64_t usec);


int ffmpeg_parse_opts(const char * options, AVDictionary ** dict);

int ffmpeg_apply_opts(const char * options, void * obj, int ignore_not_found);

int ffmpeg_alloc_input_context(AVFormatContext **ic, const char * filename, const char * format, AVIOContext * pb,
    AVIOInterruptCB * icb, const char * options, AVDictionary ** dicout);

int ffmpeg_open_input( AVFormatContext **ic, const char *filename, const char * format, AVIOContext * pb,
    AVIOInterruptCB * icb, const char * fflags );

void ffmpeg_close_input(AVFormatContext **ic);


int ffmpeg_probe_input_fast(AVFormatContext * ic, int maxframes, int maxtime_sec );
int ffmpeg_probe_input(AVFormatContext * ic, int fast);

void ffmpeg_close_output(AVFormatContext **oc);
int64_t ffmpeg_get_media_file_duration(const char * source);

int ffmpeg_copy_stream(const AVStream * is, AVStream * os, const struct AVOutputFormat * oformat);
int ffmpeg_copy_streams(const AVFormatContext * ic, AVFormatContext * oc);
int ffmpeg_decode_frame(AVCodecContext * codec, AVPacket * pkt, AVFrame * frm, int * gotframe);

void ffmpeg_rescale_timestamps( AVPacket * pkt, const AVRational from, const AVRational to );

AVFrame * ffmpeg_video_frame_create(enum AVPixelFormat fmt, int cx, int cy);
AVFrame * ffmpeg_audio_frame_create(enum AVSampleFormat fmt, int sample_rate, int nb_samples, int channels,
    uint64_t channel_layout);
int ffmpeg_copy_frame( AVFrame * dst, const AVFrame * src );




const int * ffmpeg_get_supported_samplerates(const AVCodec * enc, const AVOutputFormat * ofmt);
int ffmpeg_select_samplerate(const AVCodec * enc, const AVOutputFormat * ofmt, int dec_sample_rate);
int ffmpeg_is_samplerate_supported(const AVCodec * enc, const AVOutputFormat * ofmt, int sample_rate);
int ffmpeg_select_best_format(const int fmts[], int fmt);



struct timeout_interrupt_callback_s {
  AVIOInterruptCB origcb;
  int64_t end_time;
};

int ffmpeg_timeout_interrupt_callback(void * arg);

static inline int check_interrupt(const AVIOInterruptCB * icb) {
  return icb && icb->callback && icb->callback(icb->opaque);
}

static inline int is_interrupted(AVFormatContext ** c) {
  return c && *c && check_interrupt(&(*c)->interrupt_callback);
}




/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


int ffmpeg_create_http_input_context(AVIOContext ** pb,
    int (*recv)(void * cookie, int so, void * buf, int buf_size),
    void * cookie,
    int so,
    bool chunked_encoding);

int ffmpeg_http_close_input_context(AVIOContext ** pb);

/* debug */

static inline char * fcc2str(uint32_t fcc)
{
  static __thread char bfr[8];
  bfr[0] = fcc & 0xFF;
  bfr[1] = (fcc >> 8) & 0xFF;
  bfr[2] = (fcc >> 16) & 0xFF;
  bfr[3] = (fcc >> 24) & 0xFF;
  return bfr;
}


void ffmpeg_dump_codec_context(AVCodecContext * ctx);

#define DUMP_CODEC_CONTEXT(info, ctx) \
  fprintf(stderr, "%s() %d: %s\n", __func__, __LINE__, info); ffmpeg_dump_codec_context(ctx)



#ifdef __cplusplus
}
#endif

#endif /* __ffmpeg_h__ */
