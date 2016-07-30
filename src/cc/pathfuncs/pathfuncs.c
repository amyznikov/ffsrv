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
#include <fnmatch.h>
#include <dirent.h>
#include <unistd.h>
#include <malloc.h>

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

/** use stat() to check if path points to directory */
bool is_directory(const char * path)
{
  struct stat st;
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}




/** unlink all files matching given mask from given directoty */

static __thread const char * fmask;
static int wildcardfilter(const struct dirent * e) {
  return fnmatch(fmask, e->d_name, FNM_PATHNAME) == 0;
}

int unlink_files(const char * path, const char * wildcard)
{
  char fullname[PATH_MAX];
  struct dirent ** namelist = NULL;
  char * p;
  int n;

  fmask = wildcard;

  if ( (n = scandir(path, &namelist, wildcardfilter, NULL)) > 0 ) {
    p = stpcpy(stpcpy(fullname, path), "/");
    for ( int i = 0; i < n ; ++i ) {
      strcpy(p, namelist[i]->d_name);
      unlink(fullname);
      free(namelist[i]);
    }
  }

  free(namelist);

  return n;
}

