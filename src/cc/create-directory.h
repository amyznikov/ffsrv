/*
 * create-directory.h
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */


#ifndef __create_directory_h__
#define __create_directory_h__

#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_MKDIR_MODE (S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)

/** Recursive mkdir()
 * */
static inline bool create_directory(mode_t mode, const char * path)
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

//static inline bool create_directory2(mode_t mode, const char * format, ...)
//{
//  char * path = NULL;
//  va_list arglist;
//  bool fok = false;
//
//  va_start(arglist,format);
//  if ( vasprintf(&path, format, arglist) >= 0 ) {
//    fok = create_directory(mode, path);
//  }
//  va_end(arglist);
//
//  free(path);
//  return fok;
//}

#ifdef __cplusplus
}
#endif

#endif /* __create_directory_h__ */
