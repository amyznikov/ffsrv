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

/** use stat() to check if path points to directory */
bool is_directory(const char * path);

/** Recursive mkdir() */
bool create_path(mode_t mode, const char * path);

/** unlink all files matching given mask from given directoty */
int  unlink_files(const char * path, const char * wildcard);


//char * getdirname(const char * pathname);

#ifdef __cplusplus
}
#endif

#endif /* __pathfuncs_h__ */
