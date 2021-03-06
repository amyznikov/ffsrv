/*
 * debug.h
 *
 *  Created on: Mar 18, 2016
 *      Author: amyznikov
 */

// #pragma once

#ifndef __debug_h__
#define __debug_h__

#include <string.h>
#include <stdarg.h>
#include <errno.h>


#ifdef __cplusplus
extern "C" {
#endif


#define DEBUG 1

#if !DEBUG
# define PDBG(...)
#else
# define PDBG(...)  pdbg(__func__,__LINE__,__VA_ARGS__)
# define PBT()      pbt()
# define PSSL()     pssl()
#endif

void set_logfilename(const char * fname);
void pbt(void);

void pdbg(const char * func, int line, const char * format, ... )
  __attribute__ ((__format__ (__printf__, 3, 4)));

void pdbgv(const char * funct, int line, const char * format,
    va_list arglist);

void pssl(void);

#ifdef __cplusplus
}
#endif

#endif /* __debug_h__ */
