/*
 * ffsrv.h
 *
 *  Created on: Mar 15, 2016
 *      Author: amyznikov
 */


#pragma once

#ifndef __ffsrv_h__
#define __ffsrv_h__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include "ccarray.h"

#ifdef __cplusplus
extern "C" {
#endif



bool ffsrv_start(void);
void ffsrv_finish(void);


#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_h__ */
