/*
 * ffio.h
 *
 *  Created on: Mar 21, 2016
 *      Author: amyznikov
 */


// #pragma once

#ifndef __ffinput_h__
#define __ffinput_h__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "ffmpeg.h"
#include "ffobj.h"

#ifdef __cplusplus
extern "C" {
#endif


struct ffmixer;

struct ff_mixer_params {
  char ** sources;
  size_t nb_sources;
  char * smap;
};

struct ff_create_mixer_args {
  const char * name;
  struct ff_mixer_params * params;
};

int ff_create_mixer(struct ffmixer ** pp, const struct ff_create_mixer_args * args);
void ff_destroy_mixer(struct ffmixer * input);
const char * ff_mixer_get_name(struct ffmixer * input);
struct ffinputctx ff_mixer_get_input_context(struct ffmixer * input);



//
//struct ff_start_input_args {
//  void * cookie;
//  int (*onrecvpkt)(void * cookie, uint8_t *buf, int buf_size);
//  void (*onfinish)(void * cookie);
//};
//
//enum ff_input_state {
//  ff_input_state_idle = 0,
//  ff_input_state_connecting = 1,
//  ff_input_state_established = 2,
//  ff_input_state_disconnecting = 3,
//};
//
//
//bool ffinput_initialize(void);
//
//
//
//
//int ff_start_input_stream(struct ffinput ** pp,  const char * name, struct ff_start_input_args * args);
//void ff_close_input_stream(struct ffinput * input);
//
//






#ifdef __cplusplus
}
#endif

#endif /* __ffinput_h__ */
