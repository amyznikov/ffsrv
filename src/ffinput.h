/*
 * ffio.h
 *
 *  Created on: Mar 21, 2016
 *      Author: amyznikov
 */


#pragma once

#ifndef __ffinput_h__
#define __ffinput_h__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "ffms.h"
#include <libavutil/common.h>
#include <libavutil/error.h>

#ifdef __cplusplus
extern "C" {
#endif


struct ffinput;
struct ffoutput;

struct ff_create_input_args {
  struct ffms_input_params p;
};


struct ff_start_input_args {
  void * cookie;
  int (*onrecvpkt)(void * cookie, uint8_t *buf, int buf_size);
  void (*onfinish)(void * cookie);
};

struct ff_start_output_args {
  const char * format;
  void * cookie;
  int (*onsendpkt)(void * cookie, int stream_index, uint8_t * buf, int buf_size);
};


enum ff_input_state {
  ff_input_state_idle = 0,
  ff_input_state_connecting = 1,
  ff_input_state_established = 2,
  ff_input_state_disconnecting = 3,
};


bool ffinput_initialize(void);

struct ffinput * ff_create_input(struct ff_create_input_args args);
void ff_destroy_input(struct ffinput * ffinput);



int ff_start_input_stream(struct ffinput ** pp,  const char * name, struct ff_start_input_args * args);
void ff_close_input_stream(struct ffinput * input);


int ff_create_output_stream(struct ffoutput ** pp, const char * name, const struct ff_start_output_args * args);
void ff_delete_output_stream(struct ffoutput * output);
int ff_run_output_stream(struct ffoutput * output);
const char * ff_get_output_mime_type(struct ffoutput * output);
int ff_get_output_sdp(struct ffoutput * output, char * sdp, int sdpmax);
int ff_get_output_nb_streams(struct ffoutput * output, uint * nb_streams);










#ifdef __cplusplus
}
#endif

#endif /* __ffinput_h__ */
