/*
 * smap.c
 *
 *  Created on: Jun 18, 2016
 *      Author: amyznikov
 */

#include "smap.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>

bool ffsmap_init(struct ffsmap * smap, const char * string)
{
  char * e = NULL;
  int from, to;

  bool fok = true;

  if ( !string || !*string ) {
    for ( uint i = 0; i < MAX_SMAP_SIZE; ++i ) {
      smap->map[i] = i;
    }
  }
  else {
    // smap = from:to [from:to] [from:to]

    for ( uint i = 0; i < MAX_SMAP_SIZE; ++i ) {
      smap->map[i] = -1;
    }

    while ( *string ) {

      if ( isspace(*string) ) {
         ++string;
         continue;
       }

      if ( (from = strtol(string, &e, 10)) < 0 || e == string || *e != ':' || isspace(*++e) ) {
        fok = false;
        break;
      }

      to = strtol(string = e, &e, 10);
      if ( e == string || (*e && !isspace(*e)) ) {
        fok = false;
         break;
      }

      smap->map[from] = to;

      string = e;
    }
  }

  return fok;
}

int ffsmap(struct ffsmap * smap, int stidx)
{
  return stidx < 0 || stidx >= MAX_SMAP_SIZE ? -1 : smap->map[stidx];
}
