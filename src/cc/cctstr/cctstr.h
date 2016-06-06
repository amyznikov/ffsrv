/*
 * ctstring.h
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ctstring_h__
#define __ctstring_h__

#include <time.h>


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

#ifdef __cplusplus
}
#endif

#endif /* __ctstring_h__*/
