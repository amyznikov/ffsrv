/*
 * ffencoder.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffms_ffencoder_h__
#define __ffms_ffencoder_h__

#include "ffobj.h"

#ifdef __cplusplus
extern "C" {
#endif


struct ff_encoder_params {
  char * source;
  char * opts;
  char * format;
};


struct ff_create_encoder_args {
  const char * name;
  struct ffobject * decoder;
  const struct ff_encoder_params * params;
};

int ff_create_encoder(struct ffobject ** obj, const struct ff_create_encoder_args * args);






#ifdef __cplusplus
}
#endif

#endif /* __ffms_ffencoder_h__ */
