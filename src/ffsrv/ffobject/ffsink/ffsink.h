/*
 * ffsink.h
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffsink_ffsink_h__
#define __ffsink_ffsink_h__

#include "ffobject.h"


#ifdef __cplusplus
extern "C" {
#endif

struct ff_create_sink_args {
  const char * name;
  const char * path;
  const char * format;
  struct ffobject * source;
};

int ff_create_sink(struct ffobject ** obj, const struct ff_create_sink_args * args);


#ifdef __cplusplus
}
#endif

#endif /* __ffsink_ffsink_h__ */
