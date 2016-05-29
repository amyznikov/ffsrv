/*
 * ffsrv.c
 *
 *  Created on: Apr 2, 2016
 *      Author: amyznikov
 */

#include "ffsrv.h"
#include "ffcfg.h"
#include "ffobject.h"
#include "http-port.h"
#include "rtsp-port.h"
#include "ipaddrs.h"
#include "debug.h"

#include <poll.h>
#include <libavutil/log.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>

#include "../cc/coscheduler/co-scheduler.h"





bool ffsrv_start(void)
{
  av_log_set_level(ffsrv.avloglevel);

  av_register_all();
  avdevice_register_all();
  avformat_network_init();

  extern int (*ff_poll)(struct pollfd *__fds, nfds_t __nfds, int __timeout);
  ff_poll = co_poll;

  if ( !co_scheduler_init(ffsrv.ncpu) ) {
    PDBG("co_scheduler_init() fails: %s", strerror(errno));
    return false;
  }

  if ( !ffobject_init() ) {
    PDBG("ff_object_init() fails: %s", strerror(errno));
    return false;
  }

  if ( !ffdb_init() ) {
    PDBG("ffdb_setup() fails: %s", strerror(errno));
    return false;
  }

  for ( size_t i = 0, n = ccarray_size(&ffsrv.http.faces); i < n; ++i ) {
    if ( !ffsrv_add_http_port(ccarray_peek(&ffsrv.http.faces, i)) ) {
      PDBG("ffsrv_add_https_port(%s) fails: %s", sa2str(ccarray_peek(&ffsrv.http.faces, i)), strerror(errno));
      return false;
    }
  }


  for ( size_t i = 0, n = ccarray_size(&ffsrv.https.faces); i < n; ++i ) {
    if ( !ffsrv_add_https_port(ccarray_peek(&ffsrv.https.faces, i)) ) {
      PDBG("ffsrv_add_https_port(%s) fails: %s", sa2str(ccarray_peek(&ffsrv.https.faces, i)), strerror(errno));
      return false;
    }
  }

  return true;
}

void ffsrv_finish(void)
{
  /*fixme: ffsrv_finish() */
  return;
}

