/*
 * ffms.c
 *
 *  Created on: Mar 17, 2016
 *      Author: amyznikov
 *
 *      https://ffmpeg.org/ffmpeg-protocols.htm
 */

#define _GNU_SOURCE       /* for vasprintf() */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <errno.h>
#include <sys/syscall.h>

#include "../src/ffms.h"
#include "ccarray.h"
#include "pthread_wait.h"
#include "sockopt.h"
#include "debug.h"


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////


int main(int argc, char * argv[])
{
  int ncpu = 1;

  for ( int i = 1; i < argc; ++i ) {

    if ( strncmp(argv[i], "ncpu=", 5) == 0 ) {
      if ( sscanf(argv[i] + 5, "%d", &ncpu) != 1 ) {
        fprintf(stderr, "invalid value: %s\n", argv[i]);
        return EXIT_FAILURE;
      }
    }
    else {
      fprintf(stderr, "invalid argument: %s\n", argv[i]);
      return EXIT_FAILURE;
    }
  }

  if ( !ffms_init(ncpu) ) {
    fprintf(stderr, "ffms_init() fails: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }

  ffms_add_input((struct ffms_input_params) {
    .name = "cam4",
    .source = "rtsp://cam4.sis.lan:554/live1.sdp",
    .ctxopts = "-rtsp_transport tcp -rtsp_flags +prefer_tcp -fpsprobesize 0",
    .idle_timeout = 5,
    });

  ffms_add_input((struct ffms_input_params) {
    .name = "cam5",
    .source = "rtsp://cam5.sis.lan:554/Streaming/Channels/2",
    .ctxopts = "-rtsp_transport tcp -rtsp_flags +prefer_tcp -fpsprobesize 0",
    .idle_timeout = 5,
    });

  ffms_add_input((struct ffms_input_params) {
    .name = "america",
    .source = "mmsh://live.camstreams.com/cschristys",
    .ctxopts = "-fpsprobesize 0",
    .idle_timeout = 5,
    });

  ffms_add_input((struct ffms_input_params) {
    .name = "hessdalen03",
    .source = "rtsp://freja.hiof.no:1935/rtplive/_definst_/hessdalen03.stream",
    .ctxopts = "-fpsprobesize 0",
    .idle_timeout = 5,
    });



  ffms_add_input((struct ffms_input_params) {
    .name = "video0",
    .source = "v4l2:///dev/video0",
    .ctxopts = "-fflags +nobuffer -fpsprobesize 0 "
        "-pixel_format yuyv420 -input_format yuyv422 -framerate 30 -video_size 640x480 "
        "-use_libv4l2 true",
    .idle_timeout = 5,
    });

  ffms_add_input((struct ffms_input_params) {
    .name = "webcam",
    .source = "popen://ffmpeg -fpsprobesize 0 -fflags +nobuffer -avioflags direct -f v4l2 -use_libv4l2 true -re -i /dev/video0 -r 10 -c:v mjpeg -f mjpeg -blocksize 65536 pipe:1 2>/dev/null",
    .ctxopts = "-fpsprobesize 0 -probesize 32 -fflags +nobuffer -f mjpeg",
    .idle_timeout = 5,
    });

  ffms_add_input((struct ffms_input_params) {
    .name = "xwebcam",
    .source = "popen://"
        "ffmpeg -fpsprobesize 0 -probesize 32 -fflags +nobuffer -avioflags direct -f v4l2 -use_libv4l2 true -i /dev/video0 -c:v libx264 -crf 35 -profile Baseline -level 41 -rc-lookahead 2 -g 5  -fflags +nobuffer+genpts -f ffm pipe:1 2>/dev/null",
    .ctxopts = "-fpsprobesize 0 -probesize 32 -fflags +nobuffer+genpts -f ffm",
    .idle_timeout = 5,
    });


  ffms_add_input((struct ffms_input_params) {
    .name = "test",
    .source = NULL,
    .ctxopts = "-fpsprobesize 0 -fflags +genpts+sortdts",
    .idle_timeout = -1,
    .re = 0,
    });


  ffms_add_input((struct ffms_input_params) {
    .name = "koriolan",
    .source = "file:////home/videos/Koriolan.2011.XviD.HDRip.kinobomond.ru.avi",
    .ctxopts = "-fpsprobesize 0 -fflags +genpts+sortdts",
    .idle_timeout = 20,
    .re=2
    });

  ffms_add_input((struct ffms_input_params) {
    .name = "K",
    .source = "file:///mnt/sdb1/temp/koriolan.mkv",
    .ctxopts = "-fflags +genpts",
    .idle_timeout = 20,
    .re=2
    });


  if ( !ffms_add_http_port(0, 8082) ) {
    fprintf(stderr, "ffms_add_http_port() fails: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }


  if ( !ffms_add_rtsp_port(0, 554) ) {
    fprintf(stderr, "ffms_add_rtsp_port() fails: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }




  for ( int i = 0; i < 2000; ++i ) {
    sleep(1);
  }

  ffms_shutdown();

  return EXIT_SUCCESS;
}
