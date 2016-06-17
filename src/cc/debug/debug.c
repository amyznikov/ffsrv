/*
 * debug.c
 *
 *  Created on: Mar 18, 2016
 *      Author: amyznikov
 */
#include "debug.h"
#include "co-scheduler.h"
#include "strfuncs.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <pthread.h>
#include <syscall.h>
#include <unistd.h>
#include <execinfo.h>
#include <malloc.h>
#include <openssl/conf.h>
#include <openssl/err.h>




#define dbglock() \
  pthread_mutex_lock(&g_dbglock)

#define dbgunlock() \
  pthread_mutex_unlock(&g_dbglock)


static FILE * g_fplog;
static pthread_mutex_t g_dbglock = PTHREAD_MUTEX_INITIALIZER;



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
      struct cct ct;
      getcct(&ct);
      fprintf(g_fplog, "\n\n%s NEW LOG STARTED\n", cct2str()), fflush(g_fplog);
    }
  }

  dbgunlock();
}



void pdbgv(const char * func, int line, const char * format, va_list arglist)
{
  dbglock();

  if ( g_fplog ) {
    fprintf(g_fplog, "[%6d][0x%.16llx] %s %-28s(): %4d :", gettid(),
        (unsigned long long)co_current(), cct2str(), func, line);
    vfprintf(g_fplog, format, arglist);
    fputc('\n', g_fplog);
    fflush(g_fplog);
  }

  dbgunlock();
}


void pdbg(const char * func, int line, const char * format, ...)
{
  va_list arglist;
  va_start(arglist, format);
  pdbgv(func, line, format, arglist);
  va_end(arglist);
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

void pssl(void)
{
  dbglock();
  if ( g_fplog ) {
    ERR_print_errors_fp(g_fplog);
  }
  dbgunlock();
}

