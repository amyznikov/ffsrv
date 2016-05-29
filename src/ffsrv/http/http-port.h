/*
 * http-port.h
 *
 *  Created on: May 29, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffsrv_http_port_h__
#define __ffsrv_http_port_h__

#include <stddef.h>
#include <stdbool.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif


bool ffsrv_add_http_port(const struct sockaddr_in * addrs);
bool ffsrv_add_https_port(const struct sockaddr_in * addrs);

#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_http_port_h__ */
