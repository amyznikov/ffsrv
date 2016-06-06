/*
 * ctstring.c
 *
 *  Created on: Jun 6, 2016
 *      Author: amyznikov
 */

#include <stdio.h>
#include "cctstr.h"

void getcct(struct cct * ct)
{
  struct timespec t;
  struct tm * tm;

  clock_gettime(CLOCK_REALTIME, &t);
  tm = gmtime(&t.tv_sec);

  ct->year = tm->tm_year + 1900;
  ct->month = tm->tm_mon + 1;
  ct->day = tm->tm_mday;
  ct->hour = tm->tm_hour;
  ct->min = tm->tm_min;
  ct->sec = tm->tm_sec;
  ct->msec = t.tv_nsec / 1000000;
}


const char * getcctstr(char buf[32])
{
  struct cct ct;
  getcct(&ct);
  snprintf(buf, 31, "%.4d-%.2d-%.2d-%.2d:%.2d:%.2d.%.3d",
      ct.year, ct.month, ct.day, ct.hour, ct.min, ct.sec,
      ct.msec);
  return buf;
}

const char * getcctstr2(char buf[32])
{
  struct cct ct;
  getcct(&ct);
  snprintf(buf, 31, "%.4d-%.2d-%.2d-%.2d-%.2d-%.2d-%.3d",
      ct.year, ct.month, ct.day, ct.hour, ct.min, ct.sec,
      ct.msec);
  return buf;

}
