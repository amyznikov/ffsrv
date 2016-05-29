/*
 * main.c
 *
 *  ffmpeg-based live streamer
 *
 *  Created on: Mar 17, 2016
 *      Author: amyznikov
 *
 *      https://ffmpeg.org/ffmpeg-protocols.htm
 */

#include "ffsrv.h"
#include "ffcfg.h"
#include "daemon.h"
#include "debug.h"
#include <stdlib.h>


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////



int main(int argc, char * argv[])
{
  const char * config_file_name = NULL;
  bool daemon_mode = true;
  pid_t pid;


  /* Search command line for config file name
   * */
  for ( int i = 1; i < argc; ++i ) {
    if ( strncmp(argv[i], "--config=", 9) == 0 ) {
      config_file_name = argv[i] + 9;
    }
    else if ( strcmp(argv[i], "--no-daemon") == 0 ) {
      daemon_mode = 0;
    }
  }


  /* Read config file
   * */
  if ( !config_file_name ) {
    config_file_name = ffsrv_find_config_file();
  }

  if ( config_file_name && !ffsrv_read_config_file(config_file_name) ) {
    return EXIT_FAILURE;
  }



  /* Walk over command line again, overriding config file settings
   * */
  for ( int i = 1; i < argc; ++i ) {
    char keyname[256] = "", keyvalue[256] = "";
    sscanf(argv[i], "%255[^=]=%255s", keyname, keyvalue);
    if ( !ffsrv_parse_option(keyname, keyvalue) ) {
      return EXIT_FAILURE;
    }
  }


  /* Become daemon if requested
   * */
  if ( daemon_mode ) {
    if ( (pid = become_daemon()) == -1 ) {
      fprintf(stderr, "become_daemon() fails: %s", strerror(errno));
      return EXIT_FAILURE;
    }
    if ( pid != 0 ) {
      return EXIT_SUCCESS; /* finish parrent process */
    }
  }


  /* Setup log file name
   * */
  if ( !ffsrv.logfilename || !*ffsrv.logfilename ) {
    free(ffsrv.logfilename);
    if ( daemon_mode ) {
      ffsrv.logfilename = strdup("ffsrv.log");
    }
    else {
      ffsrv.logfilename = strdup("stderr");
    }
  }
  set_logfilename(ffsrv.logfilename);








  /* Setup singal handler
   * */
  if ( !setup_signal_handler() ) {
    PDBG("setup_signal_handler() fails: %s", strerror(errno));
    return EXIT_FAILURE;
  }


  /* Start actual server
   * */

  if ( !ffsrv_start() ) {
    PDBG("ffsrv_start() fails: %s", strerror(errno));
    return EXIT_FAILURE;
  }


  /* Sleep this thread */
  while ( 42 ) {
    sleep(1);
  }


  ffsrv_finish();

  return EXIT_SUCCESS;
}
