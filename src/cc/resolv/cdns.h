/*
 * cdns.h
 *
 *  Created on: Jul 9, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __cc_dns_cdns_h__
#define __cc_dns_cdns_h__

#include <netdb.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cdns_query;

int cdns_query_submit(struct cdns_query ** pq, const char * name, const struct addrinfo * hints);
int cdns_query_pollfd(const struct cdns_query * q);
int cdns_query_fetch(const struct cdns_query * q, struct addrinfo ** ent);
void cdns_query_destroy(struct cdns_query ** q);
const char * cdns_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif /* __cc_dns_cdns_h__ */
