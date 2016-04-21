/*
 * sockopt.c
 *
 *  Created on: Oct 6, 2015
 *      Author: amyznikov
 */

#define _GNU_SOURCE

#include "sockopt.h"
//#include "debug.h"
#include <stdio.h>
#include <alloca.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <unistd.h>
#include <malloc.h>
#include <stdarg.h>
#include <fcntl.h>

#define INET_ADDR(a,b,c,d)    \
  (uint32_t)((((uint32_t)(a))<<24)|((b)<<16)|((c)<<8)|(d))


static int gai2errno(int status)
{
  switch ( status ) {
#if __ANDROID__
    case EAI_ADDRFAMILY :
    case EAI_NODATA :
#endif
    case EAI_FAIL :
    case EAI_NONAME :
    case EAI_SERVICE :
    case EAI_SOCKTYPE :
      return ENXIO;

    case EAI_AGAIN :
      return EAGAIN;

    case EAI_BADFLAGS :
    case EAI_FAMILY :
      return EINVAL;

    case EAI_MEMORY :
      return ENOMEM;

    case EAI_SYSTEM :
      return errno;
  }

  return ENODATA;
}

int so_resolve(const char * servername, uint32_t * address, uint16_t * port)
{
  char * node;
  struct addrinfo hints;
  struct addrinfo * res = NULL;
  const struct sockaddr_in * addrs = NULL;

  int status;

  uint8_t a1, a2, a3, a4;

  errno = 0;

  if ( !servername ) {
    errno = EINVAL;
    return -1;
  }

  node = alloca(strlen(servername) + 1);

  if ( !port ) {
    strtok(strncpy(node, servername, sizeof(node) - 1), ":");
  }
  else if ( sscanf(servername, "%512[^:]:%hu", node, port) < 1 ) {
    errno = EINVAL;
    return -1;
  }

  if ( sscanf(node, "%hhu.%hhu.%hhu.%hhu", &a1, &a2, &a3, &a4) == 4 ) {
    *address = INET_ADDR(a1, a2, a3, a4);
    return 0;
  }


  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;

  if ( (status = getaddrinfo(node, NULL, &hints, &res)) ) {
    errno = gai2errno(status);
    return -1;
  }

  addrs = (const struct sockaddr_in *) res->ai_addr;

  *address = ntohl(addrs->sin_addr.s_addr);

  freeaddrinfo(res);

  return 0;
}


int so_geterror(int so)
{
  int optval = 0;
  socklen_t optlen = sizeof(optval);

  if ( getsockopt(so, SOL_SOCKET, SO_ERROR, &optval, &optlen) < 0 ) {
    optval = errno;
  }
  else {
    setsockopt(so, SOL_SOCKET, SO_ERROR, &optval, optlen);
  }

  return optval;
}


int so_set_sendbuf(int so, int size)
{
  return setsockopt(so, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
}

int so_set_recvbuf(int so, int size)
{
  return setsockopt(so, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
}

int so_get_sendbuf(int so, int * size)
{
  socklen_t optlen = sizeof(*size);
  return getsockopt(so, SOL_SOCKET, SO_SNDBUF, size, &optlen);
}

int so_get_recvbuf(int so, int * size)
{
  socklen_t optlen = sizeof(*size);
  return getsockopt(so, SOL_SOCKET, SO_RCVBUF, size, &optlen);
}

int so_set_send_timeout(int so, int sec)
{
  struct timeval timeout = { .tv_sec = sec, .tv_usec = 0 };
  return setsockopt(so, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

int so_set_recv_timeout(int so, int sec)
{
  struct timeval timeout = { .tv_sec = sec, .tv_usec = 0 };
  return setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

int so_get_send_timeout(int so, int * sec)
{
  struct timeval timeout = { .tv_sec = 0, .tv_usec = 0 };
  socklen_t optlen = sizeof(timeout);
  if ( getsockopt(so, SOL_SOCKET, SO_SNDTIMEO, &timeout, &optlen) != -1 ) {
    *sec = (int) (timeout.tv_sec);
    return 0;
  }
  return -1;
}

int so_get_recv_timeout(int so, int * sec)
{
  struct timeval timeout = { .tv_sec = 0, .tv_usec = 0 };
  socklen_t optlen = sizeof(timeout);
  if ( getsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &timeout, &optlen) != -1 ) {
    *sec = (int) (timeout.tv_sec);
    return 0;
  }
  return -1;
}

int so_set_nodelay(int so, int optval)
{
  return setsockopt(so, IPPROTO_TCP, TCP_NODELAY, &optval, sizeof(optval));
}

int so_get_nodelay(int so, int * optval)
{
  socklen_t optlen = sizeof(*optval);
  return getsockopt(so, IPPROTO_TCP, TCP_NODELAY, optval, &optlen);
}


int so_set_reuse_addrs(int so, int optval)
{
  return setsockopt(so, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
}

int so_get_reuse_addrs(int so, int * optval)
{
  socklen_t optlen = sizeof(*optval);
  return getsockopt(so, SOL_SOCKET, SO_REUSEADDR, optval, &optlen);
}

int so_is_listening(int so)
{
  int optval = 0;
  socklen_t optlen = sizeof(optval);
  getsockopt(so, SOL_SOCKET, SO_ACCEPTCONN, &optval, &optlen);
  return optval;
}

int so_set_noblock(int so, int optval)
{
  int flags;
  int status;

  if ( (flags = fcntl(so, F_GETFL, 0)) < 0 ) {
    status = -1;
  }
  else if ( optval ) {
    status = fcntl(so, F_SETFL, flags | O_NONBLOCK);
  }
  else {
    status = fcntl(so, F_SETFL, flags & ~O_NONBLOCK);
  }

  return status;
}


int so_set_keepalive(int so, int keepalive, int keepidle, int keepintvl, int keepcnt)
{
  if ( keepalive != -1 && setsockopt(so, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive)) == -1 ) {
    return -1;
  }

  if ( keepidle != -1 && setsockopt(so, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle)) == -1 ) {
    return -1;
  }

  if ( keepintvl != -1 && setsockopt(so, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl)) == -1 ) {
    return -1;
  }

  if ( keepcnt != -1 && setsockopt(so, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt)) == -1 ) {
    return -1;
  }

  return 0;
}


int so_get_keepalive(int so, int * keepalive, int * keepidle, int * keepintvl, int * keepcnt)
{
  if ( keepalive ) {
    socklen_t optlen = sizeof(*keepalive);
    if ( getsockopt(so, SOL_SOCKET, SO_KEEPALIVE, keepalive, &optlen) == -1 ) {
      return -1;
    }
  }

  if ( keepidle ) {
    socklen_t optlen = sizeof(*keepidle);
    if ( setsockopt(so, IPPROTO_TCP, TCP_KEEPIDLE, keepidle, optlen) == -1 ) {
      return -1;
    }
  }

  if ( keepintvl ) {
    socklen_t optlen = sizeof(*keepintvl);
    if ( setsockopt(so, IPPROTO_TCP, TCP_KEEPINTVL, keepintvl, optlen) == -1 ) {
      return -1;
    }
  }

  if ( keepcnt ) {
    socklen_t optlen = sizeof(*keepcnt);
    if ( setsockopt(so, IPPROTO_TCP, TCP_KEEPCNT, keepcnt, optlen) == -1 ) {
      return -1;
    }
  }

  return 0;
}


int so_close_connection(int so, int abort)
{
  if ( so == -1 ) {
    errno = EINVAL;
    return -1;
  }

  if ( abort ) {
    struct linger lo = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(so, SOL_SOCKET, SO_LINGER, &lo, sizeof(lo));
  }

  shutdown(so, SHUT_RDWR);
  return close(so);
}


int so_tcp_connect(const char * servername, uint16_t port, int tmo_sec)
{
  struct sockaddr_in sa;
  uint32_t address = INADDR_NONE;
  int so = -1;
  int fOk = 0;

  if ( so_resolve(servername, &address, &port) == -1 ) {
    goto end;
  }

  if ( !port ) {
    errno = EDESTADDRREQ;
    goto end;
  }

  if ( (so = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1 ) {
    goto end;
  }

  so_set_send_timeout(so, tmo_sec);
  so_set_recv_timeout(so, tmo_sec);

  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = htonl(address);
  sa.sin_port = htons(port);

  if ( connect(so, (struct sockaddr*)&sa, sizeof(sa)) != 0 ) {
    goto end;
  }

  fOk = 1;

end: ;

  if ( !fOk && so != -1 ) {
    int temp = errno;
    so_close_connection(so, 1);
    errno = temp;
    so = -1;
  }

  return so;
}


int so_bind(int so, uint32_t addrs, uint16_t port)
{
  struct sockaddr_in sin = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(addrs),
      .sin_port = htons(port),
      .sin_zero = {0}

  };

  so_set_reuse_addrs(so, 1);

  return bind(so, (struct sockaddr*) &sin, sizeof(sin));
}


int so_tcp_listen(uint32_t addrs, uint16_t port)
{
  int so = -1;
  int fok = 0;

  if ( (so = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1 ) {
    goto end;
  }

  if ( so_bind(so, addrs, port) == -1 ) {
    goto end;
  }

  if ( listen(so, SOMAXCONN) == -1 ) {
    goto end;
  }

  fok = 1;


end: ;

  if ( !fok ) {
    if (so != -1) {
      close(so), so = -1;
    }
  }

  return so;
}


int so_tcp_listen2(uint32_t addrs, uint16_t port, int rxbufsz, int txbufsz)
{
  int so = -1;
  int fok = 0;

  if ( (so = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1 ) {
    goto end;
  }

  if ( rxbufsz > 0 ) {
    so_set_recvbuf(so, rxbufsz);
  }

  if ( txbufsz > 0 ) {
    so_set_sendbuf(so, txbufsz);
  }


  if ( so_bind(so, addrs, port) == -1 ) {
    goto end;
  }

  if ( listen(so, SOMAXCONN) == -1 ) {
    goto end;
  }


  fok = 1;


end: ;

  if ( !fok ) {
    if (so != -1) {
      close(so), so = -1;
    }
  }

  return so;
}


int so_ssend(int so, const char * format, ...)
{
  char * cmsg = NULL;
  va_list arglist;
  int n;

  va_start(arglist, format);
  if ( (n = vasprintf(&cmsg, format, arglist)) > 0 ) {
    n = send(so, cmsg, n, MSG_NOSIGNAL);
  }
  va_end(arglist);

  free(cmsg);
  return n;
}

ssize_t so_srecv(int so, char line[], size_t size)
{
  ssize_t cbrecv;
  size_t cb = 0;

  for ( ; cb < size - 1 && (cbrecv = recv(so, &line[cb], 1, MSG_NOSIGNAL)) == 1; ++cb ) {
    if ( line[cb] == 0 ) {
      break;
    }
    if ( line[cb] == '\n' ) {
      if ( cb > 0 && line[cb - 1] == '\r' ) {
        --cb;
      }
      break;
    }
  }

  line[cb] = 0;

  return cb ? (ssize_t)(cb) : cbrecv < 0 ? -1 : 0;
}
