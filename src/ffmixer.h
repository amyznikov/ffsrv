/*
 * ffio.h
 *
 *  Created on: Mar 21, 2016
 *      Author: amyznikov
 */


// #pragma once

#ifndef __ffinput_h__
#define __ffinput_h__

#include "ffobj.h"

#ifdef __cplusplus
extern "C" {
#endif


struct ffmixer;

struct ff_create_mixer_args {
  const char * name;
  struct ff_mixer_params * params;
};

int ff_create_mixer(struct ffmixer ** pp, const struct ff_create_mixer_args * args);
void ff_destroy_mixer(struct ffmixer * input);
const char * ff_mixer_get_name(struct ffmixer * input);
struct ffinputctx ff_mixer_get_input_context(struct ffmixer * input);



#ifdef __cplusplus
}
#endif

#endif /* __ffinput_h__ */
