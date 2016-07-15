/*
 * strfuncs.h
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */

// #pragma once

#ifndef __ctstring_h__
#define __ctstring_h__

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// current time
struct cct {
  int year;
  int month;
  int day;
  int hour;
  int min;
  int sec;
  int msec;
};

void getcct(struct cct * cct);
const char * getcctstr(char buf[32]);
const char * getcctstr2(char buf[32]);

#define cct2str() \
    getcctstr((char[32]) {0})

#define cct2str2() \
    getcctstr2((char[32]) {0})

char * strtrim(char str[],
    const char chars[]);

char * strupath(char path[]);

char * strmkpath(const char * format, ...)
  __attribute__ ((__format__ (__printf__, 1, 2)));


void split_url(const char * url,
    char urlpath[], size_t urlpath_size,
    char urlargs[], size_t urlargs_size);

void parse_url(const char * url,
    char proto[], size_t proto_size,
    char auth[], size_t auth_size,
    char host[], size_t host_size,
    int * port_ptr,
    char path[],
    size_t path_size);

char * make_url(const char * proto,
    const char * auth,
    const char * host,
    int port,
    const char * path);

bool looks_like_url(const char * path);

bool strendswith(const char * str,
    const char * substr);

char * strpattexpand(const char * str);

#ifdef __cplusplus
}
#endif

#endif /* __ctstring_h__*/
