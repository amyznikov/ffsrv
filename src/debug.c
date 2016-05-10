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


#if DEBUG

static inline pid_t gettid(void) {
  return (pid_t) syscall (SYS_gettid);
}


void pdbg(const char * func, int line, const char * format, ...)
{
  static pthread_mutex_t pdbg_lock = PTHREAD_MUTEX_INITIALIZER;

  va_list arglist;

  struct timespec t = {
    .tv_sec = 0,
    .tv_nsec = 0
  };

  clock_gettime(CLOCK_REALTIME, &t);

  int day = t.tv_sec / (24 * 3600);
  int hour = (t.tv_sec - day * (24 * 3600)) / 3600;
  int min = (t.tv_sec - day * (24 * 3600) - hour * 3600) / 60;
  int sec = (t.tv_sec - day * (24 * 3600) - hour * 3600 - min * 60);
  int msec = t.tv_nsec / 1000000;


  pthread_mutex_lock(&pdbg_lock);

  fprintf(stderr, "[%6d][%p] %.2d:%.2d:%.2d.%.3d %-28s(): %4d :", gettid(), co_current(), hour, min, sec, msec, func, line);
  va_start(arglist, format);
  vfprintf(stderr,format, arglist);
  va_end(arglist);
  fputc('\n', stderr);
  fflush(stderr);

  pthread_mutex_unlock(&pdbg_lock);
}


void pbt(void)
{
  void * array[256] = {NULL};
  char ** messages = NULL;
  int size, i;

  size = backtrace(array, sizeof(array) / sizeof(array[0]));
  messages = backtrace_symbols(array, size);

  for ( i = 0; i < size; ++i ) {
    fprintf(stderr, "[bt]: (%d) %p %s\n", i, array[i], messages[i]);
  }

  free(messages);
}

#endif // DEBUG
