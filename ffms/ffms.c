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
#include <limits.h>
#include <sys/syscall.h>

#include "../src/ffms.h"
#include "ccarray.h"
#include "pthread_wait.h"
#include "sockopt.h"
#include "daemon.h"
#include "debug.h"


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////


int main(int argc, char * argv[])
{
  int ncpu = 1;
  const char * avll = NULL;

  bool daemon_mode = true;
  pid_t pid;

  char logfilename[PATH_MAX] = "";

  for ( int i = 1; i < argc; ++i ) {

    if ( strncmp(argv[i], "ncpu=", 5) == 0 ) {
      if ( sscanf(argv[i] + 5, "%d", &ncpu) != 1 ) {
        fprintf(stderr, "invalid value: %s\n", argv[i]);
        return EXIT_FAILURE;
      }
    }
    else if ( strncmp(argv[i], "loglevel=", 9) == 0 ) {
      avll = argv[i] + 9;
    }
    else if ( strcmp(argv[i], "-v") == 0 ) {
      if ( ++i >= argc ) {
        fprintf(stderr, "missing parameter value after -v\n");
        return EXIT_FAILURE;
      }
      avll = argv[i];
    }
    else if ( strcmp(argv[i], "--no-daemon") == 0 ) {
      daemon_mode = false;
    }
    else if ( strncmp(argv[i], "--logfile=", 10) == 0 ) {
      strncpy(logfilename, argv[i] + 10, sizeof(logfilename) - 1);
    }
    else {
      fprintf(stderr, "invalid argument: %s\n", argv[i]);
      return EXIT_FAILURE;
    }
  }


  /* Become daemon if requested
   *  Do it before all, because of next steps will open sockets and files
   * */
  if ( daemon_mode ) {
    if ( (pid = become_daemon()) == -1 ) {
      PDBG("become_daemon() fails: %s", strerror(errno));
      return EXIT_FAILURE;
    }
    if ( pid != 0 ) {
      return EXIT_SUCCESS; /* finish parrent process */
    }
  }


  /* Setup log file name
   * */
  if ( !*logfilename ) {
    if ( daemon_mode ) {
      strncpy(logfilename, "ffms.log", sizeof(logfilename)-1);
    }
    else {
      strncpy(logfilename, "stderr", sizeof(logfilename)-1);
    }
  }

  set_logfilename(logfilename);




  /* Setup singal handler
   * */
  if ( !setup_signal_handler() ) {
    PDBG("setup_signal_handler() fails: %s", strerror(errno));
    return EXIT_FAILURE;
  }


  /* Start actual server
   * */

  if ( !ffms_init(ncpu, avll) ) {
    PDBG("ffms_init() fails: %s", strerror(errno));
    return EXIT_FAILURE;
  }

  if ( !ffms_add_http_port(0, 8082) ) {
    PDBG("ffms_add_http_port() fails: %s", strerror(errno));
    return EXIT_FAILURE;
  }


/*
  if ( !ffms_add_rtsp_port(0, 554) ) {
    fprintf(stderr, "ffms_add_rtsp_port() fails: %s\n", strerror(errno));
    return EXIT_FAILURE;
  }
*/



  /* Go sleep */
  while ( -1 ) {
    sleep(1);
  }


  ffms_shutdown();

  return EXIT_SUCCESS;
}
