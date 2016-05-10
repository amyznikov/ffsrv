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



//AV_LOG_DEBUG
static int str2avll(const char * str)
{
  int ll = AV_LOG_WARNING;

  static const struct {
    const char * s;
    int ll;
  } avlls[] = {
    {"quiet", AV_LOG_QUIET},
    {"panic", AV_LOG_PANIC},
    {"fatal", AV_LOG_FATAL},
    {"error", AV_LOG_ERROR},
    {"warning", AV_LOG_WARNING},
    {"info", AV_LOG_INFO},
    {"verbose", AV_LOG_VERBOSE},
    {"debug", AV_LOG_DEBUG},
    {"trace", AV_LOG_TRACE},
  };

  if ( str ) {
    for ( uint i = 0; i < sizeof(avlls)/sizeof(avlls[0]); ++i ) {
      if ( strcasecmp(avlls[i].s, str) == 0 ) {
        ll = avlls[i].ll;
        break;
      }
    }
  }

  return ll;
}


bool ffms_init(int ncpu, const char * avll)
{
  bool fok;

  av_log_set_level(str2avll(avll));
  av_register_all();
  avdevice_register_all();
  avformat_network_init();

  extern int (*ff_poll)(struct pollfd *__fds, nfds_t __nfds, int __timeout);
  ff_poll = co_poll;

  if ( (fok = co_scheduler_init(ncpu)) ) {
    fok = ff_object_init();
  }

  return fok;
}



void ffms_shutdown(void)
{
  /*fixme: ffms_shutdown() */
  return;
}


