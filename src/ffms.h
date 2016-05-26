/*
 * libffsrv.h
 *
 *  Created on: Mar 15, 2016
 *      Author: amyznikov
 */


// #pragma once

#ifndef __ffms_h__
#define __ffms_h__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/epoll.h>
#include "ffcfg.h"

#ifdef __cplusplus
extern "C" {
#endif


bool ffms_init(void);
bool ffms_add_http_port(uint32_t addrs, uint16_t port);
bool ffms_add_https_port(uint32_t addrs, uint16_t port);
bool ffms_add_rtsp_port(uint32_t addrs, uint16_t port);
void ffms_shutdown(void);





#ifdef __cplusplus
}
#endif

#endif /* __ffms_h__ */
