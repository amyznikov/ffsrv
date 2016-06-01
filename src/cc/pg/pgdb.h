/*
 * pgdb.h
 *
 *  Created on: Jan 17, 2016
 *      Author: amyznikov
 */


#ifndef __pgdb_h__
#define __pgdb_h__

#include "libpq-fe.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif


/** DbConnection
 * */
typedef
struct PgConnection {
  PGconn * pgc;
} PgConnection;


/** DbIterator
 * */
typedef
struct PgIterator {
  PGresult * rc;
  int n, n_tuples, n_fields;
} PgIterator;




/**
 * db_setup()
 */
void pg_setup(const char * host, const char * port, const char * dbname,const char * user, const char * psw,
    const char * options, const char * tty);


/**
 * db_errmsg()
 *  get last error message
 */
const char * pg_errmsg(void);

/**
 * db_open()
 *  Setup new database connection
 */
PgConnection * pg_open(void);

/**
 * db_close()
 *  Close database connection and free the DbConnection instance.
 */
void pg_close(PgConnection ** dbc);



/**
 * db_exec()
 *  Execute an SQL command or query on database
 *  Example:
 *    rc = db_exec(dbc, "select * from users where uid=%d", uid);
 */
PGresult * pg_exec(PgConnection * dbc, const char * format, ...)
  __attribute__ ((__format__ (__printf__, 2, 3)));

/**
* db_exec_command()
*  Execute an SQL command not returning result
*  Example:
*    if ( !db_exec(dbc, "delete from users where uid=%d", uid) ) {
*     ...
*    }
*/
bool pg_exec_command(PgConnection * dbc, const char * format, ...)
  __attribute__ ((__format__ (__printf__, 2, 3)));


/**
* db_exec_query()
*  Execute an SQL query returning set of tuples
*  Example:
*    if ( !(rc=db_exec(dbc, "select * from users where uid=%d", uid)) ) {
*     ...
*    }
*/
PGresult * pg_exec_query(PgConnection * dbc, const char * format, ...)
  __attribute__ ((__format__ (__printf__, 2, 3)));


/**
 * getvalues()
 *  Extact values from tuple
 *  Example:
 *    n = db_getvalues(rc, tup_num, "%d", &x, "%f", &y, NULL);
 */
int pg_getvalues(const PGresult * rc, int tup_num, ...);


/**
 * db_iterator_open()
 *  Execute SQL query and return iterator to iterate result rows
 *  Example:
 *    it = db_iterator_open(dbc, "select * from users where name like '%s' ", name);
 */
PgIterator * pg_iterator_open(PgConnection * dbc, const char * format, ...)
  __attribute__ ((__format__ (__printf__, 2, 3)));


/**
 * db_iterator_next()
 *  Return:
 *    >0 on success
 *     0 on end of data
 *    -1 on error
 * */
int pg_iterator_next(PgIterator * curpos, ... );

/**
 * db_iterator_close()
 *  Close database iterator.
 */
void pg_iterator_close(PgIterator ** it);



/**
 *
 */
bool pg_listen_notify(PgConnection * dbc, const char * event);
bool pg_unlisten_notify(PgConnection * dbc, const char * event);
PGnotify * pg_get_notify(PgConnection * dbc, int tmo);




#ifdef __cplusplus
}
#endif

#endif /* __pgdb_h__ */
