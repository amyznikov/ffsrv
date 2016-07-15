/*
 * pathfuncs.c
 *
 *  Created on: Jul 15, 2016
 *      Author: amyznikov
 */

#include "pathfuncs.h"
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <fcntl.h>

/** Recursive mkdir()
 * */
bool create_path(mode_t mode, const char * path)
{
  size_t size;
  char tmp[(size = strlen(path ? path : "")) + 1];

  if ( strcpy(tmp, path ? path : "")[size - 1] == '/' ) {
    tmp[size - 1] = 0;
  }

  for ( char * p = tmp + 1; *p; p++ ) {
    if ( *p == '/' ) {
      *p = 0;
      if ( mkdir(tmp, mode) != 0 && errno != EEXIST ) {
        return false;
      }
      *p = '/';
    }
  }

  return mkdir(tmp, mode) == 0 || errno == EEXIST ? true : false;
}

bool is_directory(const char * path)
{
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

char * getdirname(const char * pathname)
{
  char * dir;
  char * p;

  p = (dir = strdup(pathname)) + strlen(pathname);
  while ( p > dir && *p != '/' ) {
    --p;
  }
  *p = 0;
  return dir;
}
