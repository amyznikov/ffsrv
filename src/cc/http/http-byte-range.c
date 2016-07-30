/*
 * http-byte-range.c
 *
 *  Created on: Jul 29, 2016
 *      Author: amyznikov
 */

#include "http-byte-range.h"
#include <string.h>
#include <stdio.h>


int http_parse_byte_range(const char * s, size_t content_length, struct http_byte_range ranges[], int maxranges)
{
  const char * rb, * re;

  char buf[256];
  ssize_t firstpos, lastpos;
  size_t suffix_length;
  size_t n;

  int numranges = 0;
  int np;

  // The only range unit defined by HTTP/1.1 is "bytes". HTTP/1.1
  static const char sbytes[] = "bytes=";

  // Implementations MAY ignore ranges specified using other units.
  // Range unit "bytes" is case-insensitive
  if ( strncasecmp(s, sbytes, sizeof(sbytes) - 1) != 0 ) {
    numranges = 0;
    goto end;
  }

  s += sizeof(sbytes) - 1;

  while ( numranges < maxranges ) {

    while ( *s && *s != ';' ) {
      ++s;
    }

    if ( !*(rb = s) ) {
      break;
    }

    if ( (re = strchr(rb, ',')) > rb ) {

      if ( (n = re - rb) > sizeof(buf) - 1 ) {
        numranges = -1; // Too long request
        goto end;
      }

      strncpy(buf, rb, n)[n - 1] = 0;

      if ( (np = sscanf(buf, "%zd - %zd", &firstpos, &lastpos)) == 1 ) {
        if ( firstpos < (ssize_t)content_length ) {
          ranges[numranges].firstpos = firstpos;
          ranges[numranges].lastpos = content_length - 1;
          ++numranges;
        }
      }
      else if ( np == 2 ) {
        // If the last-byte-pos value is present, it MUST be greater than or equal to
        // the first-byte-pos in that byte-range-spec
        if ( firstpos <= lastpos ) {
          ranges[numranges].firstpos = firstpos;
          ranges[numranges].lastpos = lastpos < (ssize_t)content_length ? lastpos : (ssize_t)content_length - 1;
          ++numranges;
        }
      }

      else if ( sscanf(buf, " - %zu", &suffix_length) == 1 ) {
        if ( suffix_length ) {
          ranges[numranges].firstpos = content_length
              - (suffix_length < content_length ? suffix_length : content_length);
          ranges[numranges].lastpos = content_length - 1;
          ++numranges;
        }
      }
      else {
        numranges = -1; // syntax error
        goto end;
      }
    }

    s = re + 1;
  }


end:

  return numranges;
}
