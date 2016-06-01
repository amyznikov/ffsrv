/*
 * pgdb.c
 *
 *  Created on: Jan 17, 2016
 *      Author: amyznikov
 */

#define _GNU_SOURCE

#include "pgdb.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>


/** Database login
 * */
static struct dbcfg {
  char * host;    // database server host name
  char * port;    // database server port as string
  char * dbname;  // database name
  char * user;    // database user name
  char * psw;     // database password
  char * options; // pg options
  char * tty;     // tty
} cfg;

static __thread char errmsg[256];



/** debug messages
 * */
#define LOG_ERRNO(fn) \
    snprintf(errmsg, sizeof(errmsg) - 1, "%s() fails: %d %s", fn, errno, strerror(errno))

#define LOG_PGRES(fn,rc) \
    snprintf(errmsg, sizeof(errmsg)-1, "%s() fails: %s/%s\n", \
        fn, PQresStatus(PQresultStatus(rc)),PQresultErrorMessage(rc))

#define LOG_PGCONN(pgc) \
    strncpy(errmsg, PQerrorMessage(pgc), sizeof(errmsg) - 1)



/**
 * db_setup()
 */
void pg_setup(const char * host, const char * port, const char * dbname, const char * user, const char * psw,
    const char * options, const char * tty)
{
  free(cfg.host), cfg.host = host ? strdup(host) : NULL;
  free(cfg.port), cfg.port = port ? strdup(port) : NULL;
  free(cfg.dbname), cfg.dbname = dbname ? strdup(dbname) : NULL;
  free(cfg.user), cfg.user = user ? strdup(user) : NULL;
  free(cfg.psw), cfg.psw = psw ? strdup(psw) : NULL;
  free(cfg.options), cfg.options = options ? strdup(options) : NULL;
  free(cfg.tty), cfg.tty = tty ? strdup(tty) : NULL;
}

/**
 * db_errmsg()
 *  get last error message
 */
const char * pg_errmsg(void)
{
  return errmsg;
}


/**
 * db_open()
 *  Create DbConnection object and login to actual database
 */
PgConnection * pg_open(void)
{
  PgConnection * dbc = calloc(1, sizeof(struct PgConnection));
  if ( !dbc ) {
    LOG_ERRNO("calloc");
  }
  else {
    dbc->pgc = PQsetdbLogin(cfg.host, cfg.port, cfg.options, cfg.tty, cfg.dbname, cfg.user, cfg.psw);
    if ( PQstatus(dbc->pgc) != CONNECTION_OK ) {
      LOG_PGCONN(dbc->pgc);
      PQfinish(dbc->pgc);
      free(dbc);
      dbc = NULL;
    }
  }
  return dbc;
}

/**
 * db_close()
 *  Close databsse connection and destroy DbConnection object
 */
void pg_close(PgConnection ** dbc)
{
  if ( dbc && *dbc ) {
    if ( (*dbc)->pgc ) {
      PQfinish((*dbc)->pgc);
    }
    free(*dbc), *dbc = NULL;
  }
}



/**
 * db_execv()
 *  Execute an SQL command or query on databsse
 */
static PGresult * db_execv(PgConnection * dbc, const char * format, va_list arglist)
{
  char * query = NULL;
  PGresult * rc = NULL;

  if ( vasprintf(&query, format, arglist) > 0 ) {
    rc = PQexec(dbc->pgc, query);
  }

  free(query);

  return rc;
}


/**
 * db_getvaluesv()
 *  Extact values from a tuple
 */
static int db_getvaluesv(const PGresult * rc, int tup_num, va_list arglist)
{
  const char * fmt;
  void * val;
  const char * s;
  int n = 0;

  while ( (fmt = va_arg(arglist, const char *)) && (val = va_arg(arglist, void*)) ) {

    if ( !(s = PQgetvalue(rc, tup_num, n)) ) {
      LOG_PGRES("PQgetvalue",rc);
      break;
    }

    if ( *s != 0 && sscanf(s, fmt, val) != 1 ) {
      LOG_ERRNO("sscanf");
      break;
    }

    ++n;
  }

  return n;
}




/**
 * db_iterator_open()
 *  Execute SQL query and return iterator to iterate result rows
 */
PgIterator * pg_iterator_open(PgConnection * dbc, const char * format, ...)
{
  va_list arglist;
  PgIterator * iterator = NULL;
  PGresult * rc = NULL;
  bool fok = false;

  if ( !(iterator = calloc(1, sizeof(*iterator))) ) {
    LOG_ERRNO("calloc");
    goto end;
  }

  va_start(arglist, format);
  rc = db_execv(dbc, format, arglist);
  va_end(arglist);
  if ( !rc ) {
    LOG_ERRNO("db_execv()");
    goto end;
  }

  if ( PQresultStatus(rc) != PGRES_TUPLES_OK ) {
    LOG_PGRES("db_exec", rc);
    goto end;
  }

  iterator->rc = rc;
  iterator->n_tuples = PQntuples(rc);
  iterator->n_fields = PQnfields(rc);
  fok = true;

end : ;

  if ( !fok ) {
    PQclear(rc);
    free(iterator);
    iterator = NULL;
  }

  return iterator;
}

/**
 * db_iterator_next()
 *  Return:
 *    >0 on success
 *     0 on end of data
 *    -1 on error
 * */
int pg_iterator_next(PgIterator * curpos, ... )
{
  va_list arglist;
  int status = 0;

  if ( curpos->n < curpos->n_tuples ) {

    va_start(arglist, curpos);
    status = db_getvaluesv(curpos->rc, curpos->n, arglist);
    va_end(arglist);

    if ( status == curpos->n_fields ) {
      ++curpos->n;
    }
    else {
      status = -1;
    }
  }

  return status;
}


/**
 * db_iterator_close()
 *  Close database iterator.
 */
void pg_iterator_close(PgIterator ** iter)
{
  if ( iter && *iter ) {
    PQclear((*iter)->rc);
    free(*iter), *iter = NULL;
  }
}


/**
 * db_exec()
 *  Execute an SQL command or query on databsse
 */
PGresult * pg_exec(PgConnection * dbc, const char * format, ...)
{
  va_list arglist;
  PGresult * rc = NULL;

  va_start(arglist, format);
  if ( !(rc = db_execv(dbc, format, arglist)) ) {
    LOG_ERRNO("db_execv()");
  }
  va_end(arglist);

  return rc;
}

bool pg_exec_command(PgConnection * dbc, const char * format, ...)
{
  va_list arglist;
  PGresult * rc = NULL;
  bool fok = false;

  va_start(arglist, format);
  rc = db_execv(dbc, format, arglist);
  va_end(arglist);

  if ( !rc ) {
    LOG_ERRNO("db_execv");
  }
  else if ( PQresultStatus(rc) != PGRES_COMMAND_OK ) {
    LOG_PGRES("db_execv", rc);
  }
  else {
    fok = true;
  }

  PQclear(rc);

  return fok;
}

PGresult * pg_exec_query(PgConnection * dbc, const char * format, ...)
{
  va_list arglist;
  PGresult * rc = NULL;

  va_start(arglist, format);
  rc = db_execv(dbc, format, arglist);
  va_end(arglist);

  if ( !rc ) {
    LOG_ERRNO("db_execv");
  }
  else if ( PQresultStatus(rc) != PGRES_TUPLES_OK ) {
    LOG_PGRES("db_execv", rc);
    PQclear(rc);
    rc = NULL;
  }

  return rc;
}


/**
 * db_getvalues()
 *  Extact values from tuple
 *  Example:
 *    n = db_getvalues(rc, tup_num, "%d", &x, "%f", &y, NULL);
 */
int pg_getvalues(const PGresult * rc, int tup_num, ...)
{
  va_list arglist;
  int n;

  *errmsg = 0;
  va_start(arglist, tup_num);
  n = db_getvaluesv(rc, tup_num, arglist);
  va_end(arglist);

  return n;
}




/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static int64_t db_gettime(void)
{
  struct timespec tm;
  clock_gettime(CLOCK_MONOTONIC, &tm);
  return ((int64_t) tm.tv_sec * 1000 + tm.tv_nsec / 1000000);
}

bool pg_listen_notify(PgConnection * dbc, const char * event)
{
  return pg_exec_command(dbc, "LISTEN %s", event);
}

bool pg_unlisten_notify(PgConnection * dbc, const char * event)
{
  return pg_exec_command(dbc, "UNLISTEN %s", event);
}

PGnotify * pg_get_notify(PgConnection * dbc, int tmo)
{
  PGnotify * notify = NULL;
  int64_t tc, te;

  te = tmo < 0 ? INT64_MAX : db_gettime() + tmo;
  errno = EAGAIN;

  while ( !(notify = PQnotifies(dbc->pgc)) &&(tc = db_gettime()) <= te ) {

    struct pollfd pollfd = {
        .fd = PQsocket(dbc->pgc),
        .events = POLLIN,
        .revents = 0
    };

    if ( poll(&pollfd, 1, (int) (te - tc)) < 0 ) {
      break;
    }

    if ( (pollfd.revents & POLLIN) ) {
      PQconsumeInput(dbc->pgc);
    }
    else if ( (pollfd.revents & (POLLERR | POLLHUP | POLLNVAL)) ) {
      errno = EINVAL;
      break;
    }
  }

  return notify;
}

