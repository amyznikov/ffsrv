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
#include <pwd.h>

#include "../src/ffms.h"
#include "ccarray.h"
#include "pthread_wait.h"
#include "sockopt.h"
#include "../src/ffms-config.h"
#include "daemon.h"

#include "debug.h"


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static char config_file_name[PATH_MAX];

static void get_default_config_file_name(void)
{
  struct passwd * pw;
  uid_t uid;

  snprintf(config_file_name, sizeof(config_file_name) - 1, "./ffms.cfg");
  if ( access(config_file_name, F_OK) == 0 ) {
    return;
  }

  if ( (uid = geteuid()) != 0 && (pw = getpwuid(uid)) != NULL ) {
    snprintf(config_file_name, sizeof(config_file_name) - 1, "%s/.config/ffms/ffms.cfg", pw->pw_dir);
    if ( access(config_file_name, F_OK) == 0 ) {
      return;
    }
  }

  snprintf(config_file_name, sizeof(config_file_name) - 1, "/var/lib/ffms/ffms.cfg");
  if ( access(config_file_name, F_OK) == 0 ) {
    return;
  }

  snprintf(config_file_name, sizeof(config_file_name) - 1, "/usr/local/etc/ffms.cfg");
  if ( access(config_file_name, F_OK) == 0 ) {
    return;
  }

  snprintf(config_file_name, sizeof(config_file_name) - 1, "/etc/ffms.cfg");
  if ( access(config_file_name, F_OK) == 0 ) {
    return;
  }

  *config_file_name = 0;
}


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////


int main(int argc, char * argv[])
{
  bool daemon_mode = true;
  pid_t pid;


  /* Search command line for config file name
   * */
  for ( int i = 1; i < argc; ++i ) {
    if ( strncmp(argv[i], "--config=", 9) == 0 ) {
      strncpy(config_file_name, argv[i] + 9, sizeof(config_file_name) - 1);
    }
    else if ( strcmp(argv[i], "--no-daemon") == 0 ) {
      daemon_mode = 0;
    }
  }


  /* Read config file
   * */
  if ( !*config_file_name ) {
    get_default_config_file_name();
  }

  if ( *config_file_name && !ffms_read_config_file(config_file_name) ) {
    return EXIT_FAILURE;
  }



  /* Walk over command line again, overriding config file settings
   * */
  for ( int i = 1; i < argc; ++i ) {
    char keyname[256] = "", keyvalue[256] = "";
    sscanf(argv[i], "%255[^=]=%255s", keyname, keyvalue);
    if ( !ffms_parse_option(keyname, keyvalue) ) {
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
  if ( !ffms.logfilename || !*ffms.logfilename ) {
    free(ffms.logfilename);
    if ( daemon_mode ) {
      ffms.logfilename = strdup("ffms.log");
    }
    else {
      ffms.logfilename = strdup("stderr");
    }
  }
  set_logfilename(ffms.logfilename);








  /* Setup singal handler
   * */
  if ( !setup_signal_handler() ) {
    PDBG("setup_signal_handler() fails: %s", strerror(errno));
    return EXIT_FAILURE;
  }


  /* Start actual server
   * */

  if ( !ffms_init() ) {
    PDBG("ffms_init() fails: %s", strerror(errno));
    return EXIT_FAILURE;
  }


  /* Go sleep */
  while ( -1 ) {
    sleep(1);
  }


  ffms_shutdown();

  return EXIT_SUCCESS;
}
