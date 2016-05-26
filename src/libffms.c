/*
 * libffms.c
 *
 *  Created on: Apr 2, 2016
 *      Author: amyznikov
 */

#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include "ffms.h"
#include "ffinput.h"
#include "ffmixer.h"
#include "ffdecoder.h"
#include "ffencoder.h"
#include "ffoutput.h"
#include "sockopt.h"
#include "cclist.h"
#include "ccarray.h"
#include "debug.h"





bool ffms_init(void)
{
  av_log_set_level(ffms.avloglevel);

  av_register_all();
  avdevice_register_all();
  avformat_network_init();

  extern int (*ff_poll)(struct pollfd *__fds, nfds_t __nfds, int __timeout);
  ff_poll = co_poll;

  if ( !co_scheduler_init(ffms.ncpu) ) {
    PDBG("co_scheduler_init() fails: %s", strerror(errno));
    return false;
  }

  if ( !ff_object_init() ) {
    PDBG("ff_object_init() fails: %s", strerror(errno));
    return false;
  }

  if ( !ffms_setup_db() ) {
    PDBG("ffdb_setup() fails: %s", strerror(errno));
    return false;
  }

  if ( ffms.http.port && !ffms_add_http_port(ffms.http.address, ffms.http.port) ) {
    PDBG("ffms_add_http_port() fails: %s", strerror(errno));
    return false;
  }



  return true;
}

void ffms_shutdown(void)
{
  /*fixme: ffms_shutdown() */
  return;
}

