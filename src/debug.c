/*
 * debug.c
 *
 *  Created on: Mar 18, 2016
 *      Author: amyznikov
 */
#include "debug.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <syscall.h>
#include <unistd.h>
#include <execinfo.h>
#include <malloc.h>
#include "coscheduler.h"


#define ct2str() \
    getctstr((char[32]) {0})

#define dbglock() \
  pthread_mutex_lock(&g_dbglock)

#define dbgunlock() \
  pthread_mutex_unlock(&g_dbglock)


static FILE * g_fplog;
static pthread_mutex_t g_dbglock = PTHREAD_MUTEX_INITIALIZER;

// current time
struct ct {
  int year;
  int month;
  int day;
  int hour;
  int min;
  int sec;
  int msec;
};

static inline void getct(struct ct * ct)
{
  struct timespec t;
  struct tm * tm;

  clock_gettime(CLOCK_REALTIME, &t);
  tm = localtime(&t.tv_sec);

  ct->year = tm->tm_year + 1900;
  ct->month = tm->tm_mon + 1;
  ct->day = tm->tm_mday;
  ct->hour = tm->tm_hour;
  ct->min = tm->tm_min;
  ct->sec = tm->tm_sec;
  ct->msec = t.tv_nsec / 1000000;
}

static const char * getctstr(char buf[32])
{
  struct ct ct;
  getct(&ct);
  snprintf(buf, 128, "%.4d/%.2d/%.2d %.2d:%.2d:%.2d.%.3d", ct.year, ct.month, ct.day, ct.hour, ct.min, ct.sec, ct.msec);
  return buf;
}


static inline pid_t gettid(void) {
  return (pid_t) syscall (SYS_gettid);
}

void set_logfilename(const char * fname)
{
  dbglock();

  if ( g_fplog != NULL ) {
    if ( g_fplog != stderr && g_fplog != stdout ) {
      fclose(g_fplog);
    }
    g_fplog = NULL;
  }

  if ( fname != NULL ) {
    if ( strcasecmp(fname, "stderr") == 0 ) {
      g_fplog = stderr;
    }
    else if ( strcasecmp(fname, "stdout") == 0 ) {
      g_fplog = stdout;
    }
    else if ( (g_fplog = fopen(fname, "a")) ) {
      struct ct ct;
      getct(&ct);
      fprintf(g_fplog, "\n\n%s NEW LOG STARTED\n", ct2str()), fflush(g_fplog);
    }
  }

  dbgunlock();
}



void pdbg(const char * func, int line, const char * format, ...)
{
  dbglock();

  if ( g_fplog ) {
    va_list arglist;
    fprintf(g_fplog, "[%6d][%p] %s %-28s(): %4d :", gettid(), co_current(), ct2str(), func, line);
    va_start(arglist, format);
    vfprintf(g_fplog, format, arglist);
    va_end(arglist);
    fputc('\n', g_fplog);
    fflush(g_fplog);
  }

  dbgunlock();
}


void pbt(void)
{
  void * array[256] = {NULL};
  char ** messages = NULL;

  dbglock();

  if ( g_fplog ) {
    int size = backtrace(array, sizeof(array) / sizeof(array[0]));
    messages = backtrace_symbols(array, size);
    for ( int i = 0; i < size; ++i ) {
      fprintf(g_fplog, "[bt]: (%d) %p %s\n", i, array[i], messages[i]);
    }
  }

  dbgunlock();

  free(messages);
}

