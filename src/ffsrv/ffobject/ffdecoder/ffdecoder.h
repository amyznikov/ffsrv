/*
 * ffdecoder.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffdecoder_h__
#define __ffdecoder_h__

#include "ffobject.h"

#ifdef __cplusplus
extern "C" {
#endif



struct ff_create_decoder_args {
  const char * name;
  const char * opts;
  struct ffobject * source;
};

int ff_create_decoder(struct ffobject ** obj, const struct ff_create_decoder_args * args);

#ifdef __cplusplus
}
#endif

#endif /* __ffdecoder_h__ */
