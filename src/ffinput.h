/*
 * ffinputsource.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffms_ffinputsource_h__
#define __ffms_ffinputsource_h__

#include "ffobj.h"


#ifdef __cplusplus
extern "C" {
#endif



struct ff_input_params {
  char * source;
  char * opts;
  int re;
  int genpts;
};

struct ff_create_input_args {
  const char * name;
  const struct ff_input_params * params;
};

int ff_create_input(struct ffobject ** obj, const struct ff_create_input_args * args);


#ifdef __cplusplus
}
#endif

#endif /* __ffms_ffinputsource_h__ */
