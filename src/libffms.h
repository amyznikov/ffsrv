/*
 * libffms.h
 *
 *  Created on: Apr 2, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __libffms_h__
#define __libffms_h__

#include <stddef.h>
#include <sys/types.h>
#include <stdint.h>
#include <stdbool.h>
#include "coscheduler.h"
#include "ffinput.h"
#include "cclist.h"
#include "ccarray.h"

#ifdef __cplusplus
extern "C" {
#endif


bool ffms_schedule_io(int so, int (*callback)(void *, uint32_t), void * cookie, uint32_t events, size_t stack_size);
bool ffms_start_cothread(void (*thread)(void*), void * cookie, size_t stack_size);

#ifdef __cplusplus
}
#endif

#endif /* __libffms_h__ */
