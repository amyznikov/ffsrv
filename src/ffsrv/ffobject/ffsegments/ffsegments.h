/*
 * ffsegments.h
 *
 *  Created on: Jul 23, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffsegments_h__
#define __ffsegments_h__

#include "ffobject.h"

#ifdef __cplusplus
extern "C" {
#endif

struct ffsegments;
struct ff_create_segments_args {
  const char * name;
  struct ffobject * source;
  const struct ffsegments_params * params;
};


int ff_create_segments_stream(struct ffobject ** obj, const struct ff_create_segments_args * args);
int ff_get_segments_playlist_filename(struct ffsegments * seg, const char ** manifestname, const char ** mimetype);
//int ff_get_segments_stream_state(struct ffsegments * seg);

#ifdef __cplusplus
}
#endif

#endif /* __ffsegments_h__ */
