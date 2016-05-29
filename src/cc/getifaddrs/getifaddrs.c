/*
 * getifaddrs.c
 *
 *  Created on: Nov 30, 2011
 *      Author: amyznikov
 */

#include "getifaddrs.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <ifaddrs.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define INET_ADDR(a,b,c,d) \
  (uint32_t)((((uint32_t)(a))<<24)|(((uint32_t)(b))<<16)|(((uint32_t)(c))<<8)|(d))


static char * strappend(char * string, char * substring)
{
  while ( *substring ) {
    *string++ = *substring++;
  }
  return string;
}



/**
 * enumerate_ifaces()
 * @see man getifaddrs
 */
ssize_t enumerate_ifaces(struct ifaceinfo ifaces[], size_t maxifaces)
{
  struct ifaddrs *ifaddr, *ifa;
  ssize_t n = 0;

  if ( getifaddrs(&ifaddr) == -1 ) {
    return -1;
  }

  /* Walk through linked list, maintaining head pointer so we can free list later */
  for ( ifa = ifaddr; n < (ssize_t) maxifaces && ifa != 0; ifa = ifa->ifa_next ) {
    if ( ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_INET ) {
      strncpy(ifaces[n].ifname, ifa->ifa_name, sizeof(ifaces[n].ifname) - 1)[sizeof(ifaces[n].ifname) - 1] = 0;
      ifaces[n].ifa_flags = ifa->ifa_flags;
      ifaces[n].ifaddress = ntohl(((struct sockaddr_in *) ifa->ifa_addr)->sin_addr.s_addr);
      ++n;
    }
  }

  freeifaddrs(ifaddr);

  return n;
}

/**
 * format the output string with interface state flags for comfortable reading by human
 */
char * fmtifaflags(uint32_t ifa_flags, char string[])
{
  char * beg = string;
#define CHECKIFF( flag ) \
  if ( flag & ifa_flags ) { string = strappend(string, #flag), *string ++= ' '; }

  CHECKIFF(IFF_UP);
  CHECKIFF(IFF_BROADCAST);
  CHECKIFF(IFF_DEBUG);
  CHECKIFF(IFF_LOOPBACK);
  CHECKIFF(IFF_POINTOPOINT);
  CHECKIFF(IFF_NOTRAILERS);
  CHECKIFF(IFF_RUNNING);
  CHECKIFF(IFF_NOARP);
  CHECKIFF(IFF_PROMISC);
  CHECKIFF(IFF_ALLMULTI);
  CHECKIFF(IFF_MASTER);
  CHECKIFF(IFF_SLAVE);
  CHECKIFF(IFF_MULTICAST);
  CHECKIFF(IFF_PORTSEL);
  CHECKIFF(IFF_AUTOMEDIA);
  CHECKIFF(IFF_DYNAMIC);
  *string = 0;

#undef CHECKIFF
  return beg;
}


/**
 * Parse string with address:port pair.
 *  The address may be present as interface name
 */
int getifaddr(const char * string, uint32_t * address, uint16_t * port)
{
  char * tmp;
  char sdup[strlen(string) + 1];
  int use_iface_name;

  strcpy(sdup, string);

  if ( isdigit(*sdup) ) {
    use_iface_name = 0;
  }
  else if ( isalpha(*sdup) ) {
    use_iface_name = 1;
  }
  else {
    errno = EINVAL;
    return -1;
  }

  if ( port != NULL && (tmp = strchr(sdup, ':')) != 0) {
    if ( sscanf(tmp + 1, "%hu", port) != 1 ) {
      errno = EINVAL;
      return -1;
    }
    *tmp = 0; /* Fix C string with zero byte */
  }

  if ( !use_iface_name ) {
    uint8_t b1, b2, b3, b4;
    if ( sscanf(sdup, "%hhu.%hhu.%hhu.%hhu", &b1, &b2, &b3, &b4) != 4 ) {
      errno = EINVAL;
      return -1;
    }
    *address = INET_ADDR(b1, b2, b3, b4);
  }
  else {
    /* Search the interface name requested and query it's ip address */
    struct ifaceinfo ifaces[64];
    size_t ifaces_count;
    int iface_found = 0;
    size_t i;

    if ( (ifaces_count = enumerate_ifaces(ifaces, sizeof(ifaces) / sizeof(ifaces[0]))) == (size_t) -1 ) {
      return -1;
    }

    if ( ifaces_count == 0 ) {
      errno = ENODEV;
      return -1;
    }

    for ( i = 0; i < ifaces_count; ++i ) {
      if ( strcmp(ifaces[i].ifname, sdup) == 0 ) {
        *address = ifaces[i].ifaddress;
        iface_found = 1;
        break;
      }
    }

    if ( !iface_found ) {
      errno = ENXIO;
      return -1;
    }
  }

  return 0;
}
