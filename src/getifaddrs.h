/*
 * getifaddrs.h
 *
 *  Created on: Nov 30, 2011
 *      Author: amyznikov
 */

#ifndef __getifaddrs_h__
#define __getifaddrs_h__

#include <sys/types.h>
#include <netdb.h>
#include <net/if.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef
struct ifaceinfo {
  char ifname[256];
  uint32_t ifaddress;
  uint32_t ifa_flags; /*!< IFF_* from net/if.h */
} ifaceinfo;

ssize_t enumerate_ifaces(struct ifaceinfo ifaces[], size_t maxifaces);
char * fmtifaflags(uint32_t ifa_flags, char string[]);
int getifaddr(const char * string, uint32_t * address, uint16_t * port);

#ifdef __cplusplus
}
#endif

#endif /* __getifaddrs_h__ */
