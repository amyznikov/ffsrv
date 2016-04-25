/*
 * ffdecoder.c
 *
 *  Created on: Apr 25, 2016
 *      Author: amyznikov
 */

#include "ffdecoder.h"
#include "ffmpeg.h"


int ff_create_decoder(struct ffobject ** obj, const struct ff_create_decoder_args * args)
{
  (void)(args);
  *obj = NULL;
  return AVERROR(ENOSYS);
}
