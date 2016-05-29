/*
 * ipaddrs.c
 *
 *  Created on: Oct 7, 2015
 *      Author: amyznikov
 */

#include "ipaddrs.h"

#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>



const char * saddr2str(const struct sockaddr_in * sin, char str[64])
{
  char addrbfr[INET_ADDRSTRLEN] = "";
  if ( str == NULL ) {
    static __thread char static_bfr[64];
    str = static_bfr;
  }
  sprintf(str, "%s:%u", inet_ntop(AF_INET, &sin->sin_addr.s_addr, addrbfr, sizeof(addrbfr)), ntohs(sin->sin_port));
  return str;
}

const char * ip4addr2str(uint32_t address, char str[INET_ADDRSTRLEN])
{
  const struct in_addr in = {
      htonl(address)
  };

  if ( str == NULL ) {
    static __thread char static_bfr[INET_ADDRSTRLEN];
    str = static_bfr;
  }

  return inet_ntop(AF_INET, &in, str, INET_ADDRSTRLEN);
}


int ip4_addrs_match(uint32_t addrs, const ipv4_addrs_range range)
{
  const uint8_t a[4] = { INET_BYTE(0, addrs), INET_BYTE(1, addrs), INET_BYTE(2, addrs), INET_BYTE(3, addrs) };
  int i = 0;

  while ( i < 4 && a[i] >= range[i].lo && a[i] <= range[i].hi ) {
    ++i;
  }
  return ( i == 4 );
}


int parse_ip4_addrs(const char * addrs, uint32_t * address, uint16_t * port)
{
  uint8_t a1, a2, a3, a4;
  if ( sscanf(addrs, "%hhu.%hhu.%hhu.%hhu:%hu", &a1, &a2, &a3, &a4, port) >= 4 ) {
    *address = INET_ADDR(a1, a2, a3, a4);
    return 1;
  }
  return 0;
}

int parse_ip4_addrs_range(const char * addrs, ipv4_addrs_range * range)
{
  char rngs[256][4];
  char chk = 0;
  int i;
  int fOk = 0;

  memset(rngs, 0, sizeof(rngs));

  if ( sscanf(addrs, "%255[^.].%255[^.].%255[^.].%255s", rngs[0], rngs[1], rngs[2], rngs[3]) != 4 ) {
    goto end;
  }

  for ( i = 0; i < 4; ++i ) {
    if ( strcmp(rngs[i], "*") == 0 ) {
      (*range)[3 - i].lo = 0;
      (*range)[3 - i].hi = 255;
    }
    else if ( sscanf(rngs[i], "[%hhu-%hhu]%c", &(*range)[3 - i].lo, &(*range)[3 - i].hi, &chk) == 2 ) {
      if ( (*range)[3 - i].lo > (*range)[3 - i].hi ) {
        goto end;
      }
    }
    else if ( sscanf(rngs[i], "%hhu%c", &(*range)[3 - i].lo, &chk) == 1 ) {
      (*range)[3 - i].hi = (*range)[3 - i].lo;
    }
    else {
      goto end;
    }
  }

  fOk = 1;

end: ;

  return fOk;
}
