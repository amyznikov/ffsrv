/*
 * pathfuncs.h
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */


#ifndef __pathfuncs_h__
#define __pathfuncs_h__

#include <stddef.h>
#include <stdbool.h>
#include <sys/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_MKDIR_MODE (S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)

/** Recursive mkdir() */
bool create_path(mode_t mode, const char * path);
bool is_directory(const char * path);
char * getdirname(const char * pathname);

#ifdef __cplusplus
}
#endif

#endif /* __pathfuncs_h__ */
