/*
 * url-parser.c
 *
 *  Created on: Apr 20, 2016
 *      Author: amyznikov
 */

#include "url-parser.h"
#include <string.h>
#include <stdlib.h>


static inline size_t min(size_t x, size_t y)
{
  return x < y ? x : y;
}


static void strncpyz(char * dest, const char * src, size_t dest_size)
{
  if ( dest_size > 0 ) {
    strncpy(dest, src, dest_size - 1)[dest_size - 1] = 0;
  }
}

void parse_url(const char * url, char proto[], size_t proto_size, char auth[], size_t auth_size, char host[],
    size_t host_size, int * port_ptr, char path[], size_t path_size)
{
  const char *p, *ls, *ls2, *at, *at2, *col, *brk;

  if ( port_ptr ) {
    *port_ptr = -1;
  }

  if ( proto_size > 0 ) {
    proto[0] = 0;
  }

  if ( auth_size > 0 ) {
    *auth = 0;
  }

  if ( host_size > 0 ) {
    *host = 0;
  }

  if ( path_size > 0 ) {
    *path = 0;
  }

  /* parse protocol */
  if ( !(p = strchr(url, ':')) ) {
    /* no protocol means plain filename */
    strncpyz(path, url, path_size);
    return;
  }


  strncpyz(proto, url, min(proto_size, p + 1 - url));
  /* skip '://' */
  if ( *++p == '/' ) {
    ++p;
  }
  if ( *p == '/' ) {
    ++p;
  }

  /* separate path from hostname */
  ls2 = strchr(p, '?');
  if ( !(ls = strchr(p, '/')) ) {
    ls = ls2;
  }
  else if ( ls && ls2 && ls2 < ls ) {
    ls = ls2;
  }

  if ( ls ) {
    strncpyz(path, ls, path_size);
  }
  else {
    ls = &p[strlen(p)];    // XXX
  }

  /* the rest is hostname, use that to parse auth/port */
  if ( ls != p ) {
    /* authorization (user[:pass]@hostname) */
    at2 = p;
    while ( (at = strchr(p, '@')) && at < ls ) {
      strncpyz(auth, at2, min(auth_size, at + 1 - at2));
      p = at + 1; /* skip '@' */
    }

    if ( *p == '[' && (brk = strchr(p, ']')) && brk < ls ) {
      /* [host]:port */
      strncpyz(host, p + 1, min(host_size, brk - p));
      if ( brk[1] == ':' && port_ptr ) {
        *port_ptr = atoi(brk + 2);
      }
    }
    else if ( (col = strchr(p, ':')) && col < ls ) {
      strncpyz(host, p, min(col + 1 - p, host_size));
      if ( port_ptr ) {
        *port_ptr = atoi(col + 1);
      }
    }
    else {
      strncpyz(host, p, min(ls + 1 - p, host_size));
    }
  }
}


void split_stream_path(const char * stream_path, char objname[], size_t objname_size, char params[], size_t params_size)
{
  // stream_path = inputname[/outputname][?params]
  char * ps;
  size_t n;

  *objname = 0;
  *params = 0;

  if ( !(ps = strpbrk(stream_path, "?")) ) {
    strncpy(objname, stream_path, objname_size - 1)[objname_size - 1] = 0;
    return;
  }

  if ( (n = ps - stream_path) < objname_size ) {
    objname_size = n;
  }

  strncpy(objname, stream_path, objname_size - 1)[objname_size - 1] = 0;
  stream_path += n + 1;

  if ( *ps == '?' ) {
    strncpy(params, ps + 1, params_size - 1)[params_size - 1] = 0;
  }

}

