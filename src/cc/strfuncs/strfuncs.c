/*
 * ctstring.c
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include "strfuncs.h"


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

void getcct(struct cct * ct)
{
  struct timespec t;
  struct tm * tm;

  clock_gettime(CLOCK_REALTIME, &t);
  tm = gmtime(&t.tv_sec);

  ct->year = tm->tm_year + 1900;
  ct->month = tm->tm_mon + 1;
  ct->day = tm->tm_mday;
  ct->hour = tm->tm_hour;
  ct->min = tm->tm_min;
  ct->sec = tm->tm_sec;
  ct->msec = t.tv_nsec / 1000000;
}

const char * getcctstr(char buf[32])
{
  struct cct ct;
  getcct(&ct);
  snprintf(buf, 31, "%.4d-%.2d-%.2d-%.2d:%.2d:%.2d.%.3d",
      ct.year, ct.month, ct.day, ct.hour, ct.min, ct.sec,
      ct.msec);
  return buf;
}

const char * getcctstr2(char buf[32])
{
  struct cct ct;
  getcct(&ct);
  snprintf(buf, 31, "%.4d-%.2d-%.2d-%.2d-%.2d-%.2d-%.3d",
      ct.year, ct.month, ct.day, ct.hour, ct.min, ct.sec,
      ct.msec);
  return buf;

}

char * strtrim(char str[], const char chars[])
{
  const size_t size = strlen(str);
  char * e = str + size;

  while ( e >= str && strchr(chars, *e) ) {
    *e-- = 0;
  }

  return str;
}

char * strupath(char r[])
{
  char * p, * e;
  int n;

  p = r;

  while ( *p ) {

    switch ( *p ) {

      //////////////////////////////////////
      default : {
        ++p;
        continue;
      }


      //////////////////////////////////////
      case '/' : {
        if ( !*(p+1) ) {
          *p = 0;
        }
        else {
          e = ++p;

          while ( *e == '/' ) {
            ++e;
          }
          if ( e > p ) {
            strcpy(p, e);
          }
        }
        continue;
      }


      //////////////////////////////////////
      case '.' : {

        n = 1;
        while ( *(p + n) == '.' ) {
          ++n;
        }

        switch ( n ) {

          //////////////////////////////////////
          default : {
            p += n;
            continue;
          }


          //////////////////////////////////////
          case 1 : {

            if ( p == r ) {
              if ( *(p + 1) == '/' ) {
                strcpy(p, p + 1);
              }
            }
            else if ( *(p - 1) == '/' && *(p + 1) == '/' ) {
              strcpy(p - 1, p + 1), --p;
            }
            else {
              ++p;
            }
            continue;
          }

          //////////////////////////////////////
          case 2 : {

            if ( p == r ) {
              if ( *(p + 2) == '/' ) {
                strcpy(p, p + 2);
              }
              continue;
            }

            if ( *(p - 1) != '/' || *(p + 2) != '/' ) {
              ++p;
              continue;
            }

            e = p-- + 2;

            while ( p > r && *(p-1) != '/' ) {
              --p;
            }
            while ( p > r && *(p-1) == '/' ) {
              --p;
            }

            strcpy(p, e);

            continue;
          }
        }
      }
      break;

    }
  }

  return r;
}

char * strmkpath(const char * format, ...)
{
  char * path = NULL;
  va_list arglist;
  int n;

  va_start(arglist, format);
  if ( (n = vasprintf(&path, format, arglist)) >= 0 ) {
    strtrim(strupath(path), " /\t\n\r");
  }
  va_end(arglist);

  return path;
}



void split_url(const char * url, char urlpath[], size_t urlpath_size, char urlargs[], size_t urlargs_size)
{
  // url = path[?args]
  char * ps;
  size_t n;

  *urlpath = 0;
  *urlargs = 0;

  if ( !(ps = strpbrk(url, "?")) ) {
    strncpy(urlpath, url, urlpath_size - 1)[urlpath_size - 1] = 0;
    goto end;
  }

  if ( (n = ps - url) < urlpath_size ) {
    urlpath_size = n;
  }

  strncpy(urlpath, url, urlpath_size - 1)[urlpath_size - 1] = 0;
  url += n + 1;

  if ( *ps == '?' ) {
    strncpy(urlargs, ps + 1, urlargs_size - 1)[urlargs_size - 1] = 0;
  }

end: ;

  strupath(urlpath);
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


char * make_url(const char * proto, const char * auth, const char * host, int port, const char * path)
{
  size_t outlen  = 0;
  char * out = NULL;

  if ( proto && *proto ) {
    outlen += strlen(proto) + 3; // proto://
  }

  if ( auth && *auth ) {
    outlen += strlen(auth) + 1; // proto://auth@
  }

  if ( host && *host ) {
    outlen += strlen(host);  // proto://auth@host
  }

  if ( port > 0 ) {
    outlen += 6;  // proto://auth@host:65535
  }

  if ( path && *path ) {
    outlen += strlen(path) + 1; // proto://auth@host:65535/path
  }


  if ( (out = calloc(1, outlen + 1)) ) {

    if ( proto && *proto ) {
      strcat(strcat(out, proto), "://");
    }

    if ( auth && *auth ) {
      strcat(strcat(out, auth), "@");
    }

    if ( host && *host ) {
      strcat(out, host);
    }

    if ( port > 0 ) {
      char buf[16];
      sprintf(buf, ":%hu", (uint16_t) (port));
      strcat(out, buf);
    }

    if ( path && *path ) {
      if ( *path != '/' ) {
        strcat(out, "/");
      }
      strcat(out, path);
    }
  }

  return out;
}
