/*
 * ffsegments.c
 *
 *  Created on: Jul 23, 2016
 *      Author: amyznikov
 */

#define _GNU_SOURCE

#include "ffsegments.h"
#include "ffcfg.h"
#include "ffgop.h"
#include "strfuncs.h"
#include "hashfuncs.h"
#include "pathfuncs.h"
#include "debug.h"
#include <search.h>


#define SEGMENTS_THREAD_STACK_SIZE  (ffsrv.mem.ffsegments)

#define objname(obj) \
    (obj)->base.name


enum ofmt {
  ofmt_unknown = -1,
  ofmt_dash,
  ofmt_hls
};

enum {
  ffsegments_state_idle,
  ffsegments_state_starting,
  ffsegments_state_streaming,
  ffsegments_state_stopping,
};

struct ffsegments {
  struct ffobject base;
  struct ffobject * source;

  // absolute path to this object
  char * basepath;

  // absolute path to playlist directory
  char * playlistpath;

  // prefix to each playlist or segment file name
  char * prefix;

  // main playlist (or dash manifest) absolute pathname
  char * playlist;
  const char * mime_type;

  // sub playlists absolute pathnames
  char ** subplaylists; // [nb_outputs]

  // playlistpath access hook ticket
  void * ticket;

  uint nb_input_streams;
  const struct ffstream * const * iss;    // [nb_input_streams]

  uint nb_outputs;
  AVFormatContext ** oc;    // [nb_outputs]
  int64_t * bitrates; // [nb_outputs]
  int * ocsmap; // [nb_input_streams][nb_outputs]

  enum ofmt ofmt;
  int itmo, rtmo;
  int stream_state;

  int64_t idletimeout;
};



static void on_destroy_segments_stream(void * ffobject)
{
  struct ffsegments * obj = ffobject;

  rmaccesshook(obj->ticket);

  if ( obj->playlist ) {
    free(obj->playlist);
    obj->playlist = NULL;
  }

  if ( obj->nb_outputs > 1 && obj->subplaylists ) {
    for ( uint i = 0; i < obj->nb_outputs; ++i ) {
      free(obj->subplaylists[i]);
    }
    free(obj->subplaylists);
  }

  if ( obj->oc ) {
    for ( uint i = 0; i < obj->nb_outputs; ++i ) {
      avformat_free_context(obj->oc[i]);
    }
    free(obj->oc);
    obj->oc = NULL;
  }

  if ( obj->bitrates ) {
    free(obj->bitrates);
    obj->bitrates = NULL;
  }

  if ( obj->ocsmap ) {
    free(obj->ocsmap);
    obj->ocsmap = NULL;
  }

  release_object(obj->source);
  obj->source = NULL;
}



static void on_segments_release_ref(void * ffobject)
{
  struct ffsegments * seg = ffobject;
  if ( seg->base.refs == 2 ) {
    seg->idletimeout = ffmpeg_gettime_us() + seg->itmo * FFMPEG_TIME_SCALE;
  }
}

static void on_reset_idle_timer(void * arg)
{
  struct ffsegments * seg = arg;
  seg->idletimeout = ffmpeg_gettime_us() + seg->itmo * FFMPEG_TIME_SCALE;
}


static char * genprefix(const struct ffsegments * seg)
{
  uint32_t hash;
  time_t t;
  char * p;
  t = time(NULL);
  hash = djb2_finalize(djb2_update(djb2_update_s(djb2_begin(), seg->base.name), &t, sizeof(t)));
  asprintf(&p, "%.X-", hash);
  return p;
}


static char * getobjpath(const char * objname)
{
  char * dname, * objpath = NULL;
  if ( (dname = strdirname(objname) ) ) {
    objpath = strmkpath("%s/%s", ffsrv.db.root, dname);
  }
  free(dname);
  return objpath;
}

static const char * get_mime_type(const char * formatname)
{
  if ( strcmp(formatname, "dash") == 0 ) {
    return "application/dash+xml";
  }
  if ( strcmp(formatname, "hls") == 0 ) {
    return "application/x-mpeg";
  }
  return "application/x-octet-stream";
}

static char * getplaylistpath(const char * basepath, const char * playlist)
{
  char * path = NULL, * playlistpath = NULL;

  if ( (path = strdirname(playlist)) ) {
    playlistpath = strmkpath("%s/%s", basepath, path);
    free(path);
  }
  return playlistpath;
}

// basepath/playlistpath/${prefix}playlistname-stream_index.m3u8
static char * genplaylistfilename(const char * basepath, const char * playlist, const char * prefix, int stream_index)
{
  char * path = NULL, * name = NULL, * basename = NULL, * playlistfilename = NULL;
  const char * ext;

  if ( !(name = strfilename(playlist)) ) {
    goto end;
  }

  if ( !*name ) {
    errno = EINVAL;
    goto end;
  }

  if ( !(path = strdirname(playlist)) ) {
    goto end;
  }

  if ( stream_index < 0 ) {
    playlistfilename = strmkpath("%s/%s/%s%s", basepath, path, prefix, name);
  }
  else {

    if ( !(ext = strrchr(name, '.')) ) {
      ext = "";
    }

    if ( !(basename = strbasename(name)) ) {
      goto end;
    }

    playlistfilename = strmkpath("%s/%s/%s%.2d-%s%s", basepath, path, prefix, stream_index, basename, ext);
  }

end:

  free(path);
  free(name);
  free(basename);

  return playlistfilename;
}

static char * gensegmenttemplate(const struct ffsegments * seg, const char * template, int stream_index)
{
  char * path = NULL, * name = NULL;
  char * segments_template = NULL;

  if ( !template || !*template ) {
    if ( stream_index < 0 ) {
      segments_template = strmkpath("%s/%ssegment%%03d.ts", seg->playlistpath, seg->prefix);
    }
    else {
      segments_template = strmkpath("%s/%s%.02d-segment%%03d.ts", seg->playlistpath, seg->prefix, stream_index);
    }
  }
  else if ( (path = strdirname(template)) && (name = strfilename(template)) ) {
    if ( stream_index < 0 ) {
      segments_template = strmkpath("%s/%s/%s%s", seg->playlistpath, path, seg->prefix, name);
    }
    else {
      segments_template = strmkpath("%s/%s/%s%.02d-%s", seg->playlistpath, path, seg->prefix, stream_index, name);
      PDBG("seg->prefix=%s segments_template=%s", seg->prefix, segments_template);
    }
  }

  free(path);
  free(name);

  return segments_template;
}


static int write_hls_playlist(struct ffsegments * seg)
{
  FILE * fp;
  const char * p;
  size_t rlen;

  int status = 0;

  if ( !(fp = fopen(seg->playlist,"w")) ) {
    status = AVERROR(errno);
    PDBG("[%s] can not write '%s': %s", objname(seg), seg->playlist, av_err2str(status));
    goto end;
  }

  rlen = strlen(seg->basepath) + 1;

  fprintf(fp, "#EXTM3U\n");
  for ( uint i = 0; i < seg->nb_outputs; ++i ) {
    p = seg->subplaylists[i] + rlen;
    PDBG("seg->submanifests[%u]=%s p=%s", i, seg->subplaylists[i], p );
    fprintf(fp, "#EXT-X-STREAM-INF:PROGRAM-ID=%u,BANDWIDTH=%"PRId64"\n%s\n", i, seg->bitrates[i], p);
  }

end:

  if ( fp ) {
    fclose(fp);
  }

  return status;
}

static void cleanup_segment_files(struct ffsegments * seg)
{
  if ( seg->prefix && *seg->prefix && seg->playlistpath ) {
    char mask[128];
    sprintf(mask,"%s*", seg->prefix);
    unlink_files(seg->playlistpath, mask);
  }
}


static void segments_thread(void * arg)
{
  struct ffsegments * seg = arg;
  struct ffgop * gop = NULL;
  struct ffgoplistener * gl = NULL;

  AVPacket pkt;

  int64_t firstdts = AV_NOPTS_VALUE;
  int64_t * tsoffset = NULL;


  int status = 0;


  av_init_packet(&pkt);
  pkt.data = NULL, pkt.size = 0;


  if ( !(gop = get_gop(seg->source)) || ffgop_get_type(gop) != ffgop_pkt ) {
    PDBG("[%s] get_gop() fails", objname(seg));
    status = AVERROR(EINVAL);
    goto end;
  }

  status = ffgop_create_listener(gop, &gl, &(struct ffgop_create_listener_args ) {
        .getoutspc = NULL,
        .cookie = NULL
      });

  if ( status ) {
    PDBG("[%s] ffgop_create_listener() fails: %s", objname(seg), av_err2str(status));
    goto end;
  }

  tsoffset = alloca(seg->nb_input_streams * sizeof(*tsoffset));
  for ( uint i = 0; i < seg->nb_input_streams; ++i ) {
    tsoffset[i] = AV_NOPTS_VALUE;
  }

  if ( seg->ofmt == ofmt_hls && seg->nb_outputs > 1 ) {
    if ( (status = write_hls_playlist(seg)) ) {
      PDBG("[%s] write_hls_playlist() fails: %s", objname(seg), av_err2str(status));
      goto end;
    }
  }


  //
  // write header(s)
  //
  for ( uint i = 0; i < seg->nb_outputs; ++i ) {
    if ( (status = avformat_write_header(seg->oc[i], NULL)) ) {
      PDBG("[%s] avformat_write_header() fails: %s", objname(seg), av_err2str(status));
      goto end;
    }
  }

  seg->stream_state = ffsegments_state_streaming;

  //
  // Main loop
  //

  while ( status >= 0 ) {

    int64_t pkt_pts, pkt_dts, pkt_duration;
    AVFormatContext * oc;
    const struct ffstream * is;
    AVStream * os;
    int isidx, osidx;


    if ( seg->base.refs == 1 && ffmpeg_gettime_us() > seg->idletimeout ) {
      PDBG("[%s] EXIT BY IDLE TIMEOUT %d sec: refs = %d", objname(seg), seg->itmo, seg->base.refs);
      status = AVERROR(ECHILD);
      break;
    }

    if ( (status = ffgop_get_pkt(gl, &pkt)) ) {
      PDBG("[%s] ffgop_get_pkt() fails: %s", objname(seg), av_err2str(status));
      break;
    }

    is = seg->iss[isidx = pkt.stream_index];
    pkt_pts = pkt.pts;
    pkt_dts = pkt.dts;
    pkt_duration = pkt.duration;

    if ( firstdts == AV_NOPTS_VALUE && pkt.dts != AV_NOPTS_VALUE ) {
      const AVRational utb = (AVRational ) {1, 1000000 };
      firstdts = av_rescale_ts(pkt.dts, is->time_base, utb);
      for ( uint i = 0; i < seg->nb_input_streams; ++i ) {
        tsoffset[i] = av_rescale_ts(firstdts, utb, seg->iss[i]->time_base);
      }
    }


    // map input stream index -> output stream index

    for ( uint i = 0; i < seg->nb_outputs; ++i ) {

      if ( !seg->ocsmap ) {
        osidx = isidx;
      }
      else if ( (osidx = seg->ocsmap[isidx * seg->nb_outputs + i]) < 0 ) {
        continue;
      }

      oc = seg->oc[i];
      os = oc->streams[osidx];

      pkt.pts = pkt_pts;
      pkt.dts = pkt_dts;
      pkt.duration = pkt_duration;
      pkt.stream_index = osidx;

      if ( is->time_base.num != os->time_base.num || is->time_base.den != os->time_base.den ) {
        av_packet_rescale_ts(&pkt, is->time_base, os->time_base);
      }

      if ( oc->nb_streams > 1 ) {
        if ( (status = av_interleaved_write_frame(oc, &pkt)) < 0 ) {
          PDBG("[%s] av_interleaved_write_frame() fails: %s", objname(seg), av_err2str(status));
        }
      }
      else if ( (status = av_write_frame(oc, &pkt)) < 0 ) {
        PDBG("[%s] av_write_frame() fails: %s", objname(seg), av_err2str(status));
      }
    }

    av_packet_unref(&pkt);
    co_yield();
  }

end:

  seg->stream_state = ffsegments_state_stopping;

  for ( uint i = 0; i < seg->nb_outputs; ++i ) {
    av_write_trailer(seg->oc[i]);
  }

  seg->stream_state = ffsegments_state_idle;

  ffgop_delete_listener(&gl);

  cleanup_segment_files(seg);

  PDBG("[%s] FRINISHED", objname(seg));
  release_object(&seg->base);

}

int ff_get_segments_playlist_filename(struct ffsegments * seg, const char ** playlist, const char ** mimetype)
{
  uint i;

  while ( seg->stream_state == ffsegments_state_starting ) {
    co_sleep(100 * 1000);
  }

  if ( seg->nb_outputs > 1 ) {
    while ( seg->stream_state == ffsegments_state_streaming ) {

      for ( i = 0; i < seg->nb_outputs; ++i ) {
        if ( access(seg->subplaylists[i], F_OK) != 0 ) {
          break;
        }
      }
      if ( i == seg->nb_outputs ) {
        break;
      }
      co_sleep(100 * 1000);
    }
  }

  * playlist = seg->playlist;
  * mimetype = seg->mime_type;
  return 0;
}


static int start_segments(struct ffsegments * seg, const struct ff_create_segments_args * args)
{
  AVDictionary * opts = NULL;
  AVDictionaryEntry * e = NULL;
  AVOutputFormat * ofmt = NULL;
  char * segment_template_opt = NULL;
  AVStream * os;
  int osidx;

  int nb_input_video_streams = 0;


  int status = 0;

  //
  //  Parse options line
  //
  seg->ofmt = ofmt_unknown;

  if ( (status = ffmpeg_parse_options(args->params->opts, true, &opts)) ) {
    PDBG("[%s] ffmpeg_parse_options('%s') fails: %s", objname(seg), args->params->opts, av_err2str(status));
    goto end;
  }



  //
  // Get output format
  //
  if ( !(e = av_dict_get(opts, "f", NULL, 0)) || !e->value ) {
    status = AVERROR_MUXER_NOT_FOUND;
    PDBG("[%s] Output format not specified: %s", objname(seg), av_err2str(status));
    goto end;
  }

  if ( !(ofmt = av_guess_format(e->value, NULL, NULL)) ) {
    status = AVERROR_MUXER_NOT_FOUND;
    PDBG("[%s] Not supported output format '%s': %s", objname(seg), e->value, av_err2str(status));
    goto end;
  }

  av_dict_set(&opts, "f", NULL, 0); // remove the -f option




  if ( strcmp(ofmt->name, "dash") == 0 ) {
    seg->ofmt = ofmt_dash;
  }
  else if ( strcmp(ofmt->name, "hls") == 0 ) {
    seg->ofmt = ofmt_hls;
    if ( (e = av_dict_get(opts, "hls_segment_filename", NULL, 0)) ) {
      segment_template_opt = strdup(e->value);
      av_dict_set(&opts, "hls_segment_filename", NULL, 0);
    }
  }





  if ( !(seg->prefix = genprefix(seg))) {
    status = AVERROR(errno);
    PDBG("[%s] genprefix() fails: %s", objname(seg), av_err2str(status));
    goto end;
  }

  if ( !(seg->basepath = getobjpath(seg->base.name)) ) {
    status = AVERROR(errno);
    PDBG("[%s] getobjpath(seg->base.name=%s) fails: %s", objname(seg), seg->base.name, av_err2str(status));
    goto end;
  }

  if ( !(seg->playlistpath = getplaylistpath(seg->basepath, args->params->manifest)) ) {
    status = AVERROR(errno);
    PDBG("[%s] getplaylistpath(%s) fails: %s", objname(seg), args->params->manifest, av_err2str(status));
    goto end;
  }


  if ( !(seg->playlist = genplaylistfilename(seg->basepath, args->params->manifest, seg->prefix, -1)) ) {
    status = AVERROR(errno);
    PDBG("[%s] genplaylistfilename() fails: %s", objname(seg), av_err2str(status));
    goto end;
  }

  //
  // Get input streams
  //
  if ( (status = get_streams(args->source, &seg->iss, &seg->nb_input_streams)) ) {
    PDBG("[%s] get_streams(source=%p) fails: %s", objname(seg), args->source, av_err2str(status));
    goto end;
  }

  if ( seg->nb_input_streams < 1 ) {
    status = AVERROR_STREAM_NOT_FOUND;
    PDBG("[%s] No input streams found", objname(seg));
    goto end;
  }

  for ( uint i = 0; i < seg->nb_input_streams; ++i ) {
    if ( seg->iss[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ) {
      ++nb_input_video_streams;
    }
  }



  // for hls we will send each video stream to separate output
  if ( seg->ofmt != ofmt_hls || nb_input_video_streams < 2 ) {

    seg->nb_outputs = 1;

    if ( seg->ofmt == ofmt_hls ) {
      char * segment_template = gensegmenttemplate(seg, segment_template_opt, -1);
      if ( segment_template ) {
        av_dict_set(&opts, "hls_segment_filename", segment_template, AV_DICT_DONT_STRDUP_VAL);
      }
      else {
        status = AVERROR(errno);
        PDBG("[%s] gensegmenttemplate() fails: %s", objname(seg), av_err2str(status));
        goto end;
      }
    }


    if ( !(seg->oc = calloc(1, sizeof(*seg->oc))) ) {
      status = AVERROR(errno);
      PDBG("[%s] calloc(seg->oc) fails: %s", objname(seg), av_err2str(status));
      goto end;
    }

    if ( (status = avformat_alloc_output_context2(&seg->oc[0], ofmt, NULL, seg->playlist)) ) {
      PDBG("avformat_alloc_output_context2('%s': '%s') fails: %s", ofmt->name, seg->playlist, av_err2str(status));
      goto end;
    }

    if ( (status = ffstreams_to_context(seg->iss, seg->nb_input_streams, seg->oc[0])) ) {
      PDBG("ffstreams_to_context() fails: %s", av_err2str(status));
      goto end;
    }

    if ( (status = ffmpeg_apply_context_opts(seg->oc[0], opts)) ) {
      PDBG("[%s] ffmpeg_apply_context_opts() fails: %s", objname(seg), av_err2str(status));
      goto end;
    }

  }
  else {

    seg->nb_outputs = nb_input_video_streams;

    if ( !(seg->ocsmap = calloc(seg->nb_input_streams * seg->nb_outputs, sizeof(seg->ocsmap[0]))) ) {
      status = AVERROR(errno);
      PDBG("[%s] calloc(seg->ocsmap) fails: %s", objname(seg), av_err2str(status));
      goto end;
    }

    if ( !(seg->oc = calloc(seg->nb_outputs, sizeof(*seg->oc))) ) {
      status = AVERROR(errno);
      PDBG("[%s] calloc(seg->oc) fails: %s", objname(seg), av_err2str(status));
      goto end;
    }

    if ( !(seg->subplaylists = calloc(seg->nb_outputs, sizeof(seg->subplaylists[0])))) {
      status = AVERROR(errno);
      PDBG("[%s] calloc(seg->oc) fails: %s", objname(seg), av_err2str(status));
      goto end;
    }

    // IN:
    //  0 A1
    //  1 A2
    //  2 V1
    //  3 A3
    //  4 V2
    //  5 D1

            // OUT[0]:   0UT[1]:
            //  0 V1      0 V2
            //  1 A1      1 A1
            //  2 A2      2 A2
            //  3 A3      3 A3
            //  4 D1      4 D1

    // map:
    //    0  [  1  1 ]
    //    1  [  2  2 ]
    //    2  [  0 -1 ]
    //    3  [  3  3 ]
    //    4  [ -1  0 ]
    //    5  [  4  4 ]

    for ( uint i = 0, v = 0, a = 1; i < seg->nb_input_streams; ++i ) {
      if ( seg->iss[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ) {
        for ( uint k = 0; k < seg->nb_outputs; ++k ) {
          seg->ocsmap[i * seg->nb_outputs + k] = k == v ? 0 : -1;
        }
        ++v;
      }
      else {
        for ( uint k = 0; k < seg->nb_outputs; ++k ) {
          seg->ocsmap[i * seg->nb_outputs + k] = a;
        }
        ++a;
      }
    }

    for ( uint i = 0; i < seg->nb_outputs; ++i ) {

      if ( !(seg->subplaylists[i] = genplaylistfilename(seg->basepath, args->params->manifest, seg->prefix, i)) ) {
        status = AVERROR(errno);
        PDBG("[%s] genplaylistfilename() fails: %s", objname(seg), av_err2str(status));
        goto end;
      }

      if ( (status = avformat_alloc_output_context2(&seg->oc[i], ofmt, NULL, seg->subplaylists[i])) ) {
        PDBG("[%s] avformat_alloc_output_context2('%s': '%s') fails: %s", objname(seg), ofmt->name, seg->subplaylists[i], av_err2str(status));
        goto end;
      }

      if ( seg->ofmt == ofmt_hls ) {
        char * segment_template = gensegmenttemplate(seg, segment_template_opt, i);
        if ( segment_template ) {
          av_dict_set(&opts, "hls_segment_filename", segment_template, AV_DICT_DONT_STRDUP_VAL);
        }
        else {
          status = AVERROR(errno);
          PDBG("[%s] gensegmenttemplate() fails: %s", objname(seg), av_err2str(status));
          goto end;
        }
      }

      if ( (status = ffmpeg_apply_context_opts(seg->oc[i], opts)) ) {
        PDBG("[%s] ffmpeg_apply_context_opts() fails: %s", objname(seg), av_err2str(status));
        goto end;
      }

      for ( uint j = 0; j < seg->nb_input_streams - nb_input_video_streams + 1; ++j ) {
        if ( !(os = avformat_new_stream(seg->oc[i], NULL)) ) {
          status = AVERROR(ENOMEM);
          PDBG("[%s] avformat_new_stream() fails: %s", objname(seg), av_err2str(status));
          goto end;
        }
      }
    }

    for ( uint j = 0; j < seg->nb_input_streams; ++j ) {
      for ( uint i = 0; i < seg->nb_outputs; ++i ) {
        if ( (osidx = seg->ocsmap[j * seg->nb_outputs + i]) >= 0 ) {
          os = seg->oc[i]->streams[osidx];
          if ( (status = ffstream_to_context(seg->iss[j], os)) ) {
            PDBG("[%s] ffstream_to_context() fails: %s", objname(seg), av_err2str(status));
            goto end;
          }
        }
      }
    }
  }


  ///

  e = NULL;

  while ( (e = av_dict_get(opts, "b", e, AV_DICT_IGNORE_SUFFIX)) ) {

    char opt_name[64] = "";
    char m = 0;
    int isidx = -1, osidx = -1;

    int bitrate;
    char * tail = NULL;

    ffmpeg_parse_option_name(e->key, opt_name, &m, &isidx);

    if ( strcmp(opt_name, "b") == 0 && isidx >= 0 && isidx < (int) seg->nb_input_streams ) {

      if ( m != 0 && ffmpeg_get_media_type(m) != seg->iss[isidx]->codecpar->codec_type ) {
        PDBG("[%s] Media type not match for stream %d: actual stream type is '%c'", objname(seg), isidx,
            ffmpeg_get_media_type_specifier(seg->iss[isidx]->codecpar->codec_type));
        status = AVERROR(EINVAL);
        goto end;
      }

      if ( *e->value == '?' && seg->iss[isidx]->codecpar->bit_rate >= 1000 ) {
        continue;
      }

      if ( (bitrate = av_strtod(*e->value == '?' ? e->value + 1 : e->value, &tail)) < 1000 || *tail ) {
        PDBG("[%s] Invalid bitrate specified : %s = %s", objname(seg), e->key, e->value);
        status = AVERROR(EINVAL);
        goto end;
      }

      for ( uint i = 0; i < seg->nb_outputs; ++i ) {
        osidx = seg->ocsmap == NULL ? isidx : seg->ocsmap[isidx * seg->nb_outputs + i];
        if ( osidx >= 0 ) {
          seg->oc[i]->streams[osidx]->codecpar->bit_rate = bitrate;
        }
      }
    }
  }

  if ( !(seg->bitrates = calloc(seg->nb_outputs, sizeof(seg->bitrates[0]))) ) {
    status = AVERROR(ENOMEM);
    PDBG("[%s] calloc(seg->bitrates) fails: %s", objname(seg), av_err2str(status));
    goto end;
  }

  for ( uint i = 0; i < seg->nb_outputs; ++i ) {
    for ( uint j = 0; j < seg->oc[i]->nb_streams; ++j ) {
      if ( seg->oc[i]->streams[j]->codecpar->bit_rate > 0 ) {
        seg->bitrates[i] += seg->oc[i]->streams[j]->codecpar->bit_rate;
      }
    }
  }


  seg->mime_type = get_mime_type(ofmt->name);
  seg->source = args->source;
  seg->itmo = args->params->itmo > 0 ? args->params->itmo : 20;
  seg->rtmo = args->params->rtmo > 0 ? args->params->rtmo : 20;
  seg->idletimeout = ffmpeg_gettime_us() + (seg->itmo + 4) * FFMPEG_TIME_SCALE;
  seg->ticket = addaccesshook(seg->playlistpath, on_reset_idle_timer, seg);

end:

  free(segment_template_opt);

  if ( opts ) {
    av_dict_free(&opts);
  }

  return status;
}


int ff_create_segments_stream(struct ffobject ** obj, const struct ff_create_segments_args * args)
{
  static const struct ffobject_iface iface = {
    .on_release_ref = on_segments_release_ref,
    .on_destroy = on_destroy_segments_stream,
    .get_streams = NULL,
    .get_gop = NULL,
  };

  struct ffsegments * seg = NULL;

  int status = 0;

  if ( !args || !args->name || !args->source || !args->params ) {
    PDBG("Invalid args");
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( !args->params->manifest || !*args->params->manifest  ) {
    PDBG("[%s] manifest not specified", args->name);
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( !args->params->opts || !*args->params->opts ) {
    PDBG("[%s] Options not specified", args->name);
    status = AVERROR(EINVAL);
    goto end;
  }

  if ( !(seg = create_object(sizeof(struct ffsegments), ffobjtype_segments, args->name, &iface)) ) {
    status = AVERROR(ENOMEM);
    PDBG("[%s] C create_object(ffsegments) fails: %s", args->name, av_err2str(status));
    goto end;
  }

  seg->stream_state = ffsegments_state_starting;

  if ( (status = start_segments(seg, args)) ) {
    PDBG("[%s] start_segments() fails: %s", objname(seg), av_err2str(status));
    goto end;
  }

  add_object_ref(&seg->base);
  if ( !co_schedule(segments_thread, seg, SEGMENTS_THREAD_STACK_SIZE) ) {
    status = AVERROR(errno);
    PDBG("[%s] co_schedule(segments_thread) fails: %s", objname(seg), strerror(errno));
    release_object(&seg->base);
    goto end;
  }

end:

  if ( status && seg ) {
    seg->stream_state = ffsegments_state_idle;
    release_object(&seg->base);
    seg = NULL;
  }

  *obj = (void*)seg;

  PDBG("[%s] leave", args->name);
  return status;
}


