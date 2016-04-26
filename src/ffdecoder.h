/*
 * ffdecoder.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

//#pragma once

#ifndef __ffms_ffdecoder_h__
#define __ffms_ffdecoder_h__

#include "ffobj.h"

#ifdef __cplusplus
extern "C" {
#endif


struct ff_create_decoder_args {
  const char * name;
  struct ffobject * source;
};

int ff_create_decoder(struct ffobject ** obj, const struct ff_create_decoder_args * args);

#ifdef __cplusplus
}
#endif

#endif /* __ffms_ffdecoder_h__ */
