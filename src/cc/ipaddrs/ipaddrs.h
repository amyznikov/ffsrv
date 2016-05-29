/*
 * ipaddrs.h
 *
 *  Created on: Oct 7, 2015
 *      Author: amyznikov
 */


#ifndef __addrs_range_h__
#define __addrs_range_h__

#include <inttypes.h>
#include <netinet/in.h>


#define INET_ADDR(a,b,c,d)      (uint32_t)((((uint32_t)(a))<<24)|((b)<<16)|((c)<<8)|(d))
#define INET_BYTE(n,x)          ((uint8_t)((x >> (n*8) ) & 0x000000FF))
#define IP4_ADDRSTRLEN          32

#ifdef __cplusplus
extern "C" {
#endif


/** IPv4 address range
 * */
typedef
struct { uint8_t lo, hi; }
  ipv4_addrs_range[4];

int parse_ip4_addrs(const char * addrs, uint32_t * address, uint16_t * port);
int parse_ip4_addrs_range(const char * addrs, ipv4_addrs_range * range);
int ip4_addrs_match(uint32_t addrs, const ipv4_addrs_range range);

const char * ip4addr2str(uint32_t address, char str[INET_ADDRSTRLEN]);
const char * saddr2str(const struct sockaddr_in * sin, char str[IP4_ADDRSTRLEN]);


#define sa2str(addr) \
  saddr2str((addr),(char[64]){0})

#ifdef __cplusplus
}
#endif

#endif /* __addrs_range_h__ */
