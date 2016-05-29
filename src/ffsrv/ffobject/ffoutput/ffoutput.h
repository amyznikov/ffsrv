/*
 * ffoutput.h
 *
 *  Created on: Apr 23, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffoutput_h__
#define __ffoutput_h__

#include "ffobject.h"

#ifdef __cplusplus
extern "C" {
#endif


struct ffoutput;

struct ff_create_output_args {
  struct ffobject * source;
  const char * format;
  void * cookie;
  int (*sendpkt)(void * cookie, int stream_index, uint8_t * buf, int buf_size);
};

int ff_create_output(struct ffoutput ** output, const struct ff_create_output_args * args);
void ff_delete_output(struct ffoutput ** pps);

const char * ff_get_output_mime_type(struct ffoutput * output);
int ff_get_output_nb_streams(struct ffoutput * output, uint * nb_streams);
int ff_get_output_sdp(struct ffoutput * output, char * sdp, int sdpmax);

int ff_run_output_stream(struct ffoutput * output);


#ifdef __cplusplus
}
#endif

#endif /* __ffoutput_h__ */
