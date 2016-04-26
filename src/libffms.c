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
#include "libffms.h"
#include "ffinput.h"
#include "ffmixer.h"
#include "ffdecoder.h"
#include "ffencoder.h"
#include "ffoutput.h"
#include "coscheduler.h"
#include "sockopt.h"
#include "cclist.h"
#include "ccarray.h"
#include "debug.h"





bool ffms_init(int ncpu)
{
  bool fok;

  ff_object_init();

  av_register_all();
  avdevice_register_all();
  avformat_network_init();
  //av_log_set_level(AV_LOG_TRACE);

  extern int (*ff_poll)(struct pollfd *__fds, nfds_t __nfds, int __timeout);
  ff_poll = co_poll;

  fok = co_scheduler_init(ncpu);

  return fok;
}



void ffms_shutdown(void)
{
  /*fixme: ffms_shutdown() */
  return;
}


