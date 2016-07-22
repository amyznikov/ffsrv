/*
 * ffinput.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffinput_h__
#define __ffinput_h__

#include "ffobject.h"


#ifdef __cplusplus
extern "C" {
#endif


struct ff_create_input_args {
  const char * name;
  const struct ffinput_params * params;
  void * cookie;
  int (*recv_pkt)(void * cookie, uint8_t *buf, int buf_size);
};

int ff_create_input(struct ffobject ** obj, const struct ff_create_input_args * args);
int ff_run_input_stream(struct ffinput * input);
const char * ff_input_decopts(const struct ffinput * input);


#ifdef __cplusplus
}
#endif

#endif /* __ffinput_h__ */
