/*
 * http-port.h
 *
 *  Created on: May 29, 2016
 *      Author: amyznikov
 */

// #pragma once

#ifndef __ffsrv_http_port_h__
#define __ffsrv_http_port_h__

#include <stddef.h>
#include <stdbool.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif


bool ffsrv_add_rtsp_port(uint32_t addrs, uint16_t port);


#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_http_port_h__ */
