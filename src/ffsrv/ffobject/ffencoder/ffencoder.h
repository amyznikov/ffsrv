/*
 * ffencoder.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffencoder_h__
#define __ffencoder_h__

#include "ffobject.h"

#ifdef __cplusplus
extern "C" {
#endif


struct ff_create_encoder_args {
  const char * name;
  struct ffobject * source;
  const struct ffencoder_params * params;
};

int ff_create_encoder(struct ffobject ** obj, const struct ff_create_encoder_args * args);






#ifdef __cplusplus
}
#endif

#endif /* __ffencoder_h__ */
