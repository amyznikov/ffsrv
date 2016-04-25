/*
 * ffencoder.c
 *
 *  Created on: Apr 25, 2016
 *      Author: amyznikov
 */

#include "ffencoder.h"
#include "ffmpeg.h"




int ff_create_encoder(struct ffobject ** obj, const struct ff_create_encoder_args * args)
{
  (void)(args);
  * obj = NULL;
  return AVERROR(ENOSYS);
}
