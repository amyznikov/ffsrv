/*
 * ffdb-pg.c
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#include "ffms-pg.h"
#include "pgdb.h"
#include "ffcfg.h"
#include "debug.h"



bool ffms_pg_setup(void)
{
  pg_setup(ffms.db.pg.host, ffms.db.pg.port, ffms.db.pg.db, ffms.db.pg.user, ffms.db.pg.psw, ffms.db.pg.options,
      ffms.db.pg.tty);
  return true;
}

bool ffms_pg_find_object(const char * name, enum ffobject_type * type, ffobj_params * params)
{
  PgConnection * pgc = NULL;
  PGresult * rc = NULL;
  bool fok = false;
  int n;

  char objname[64] = "";
  char objtype[16] = "";
  char objsource[256] = "";
  char objopts[256] = "";
  int  re = 0, genpts = 0;

  * type = object_type_unknown;

  if ( !(pgc = pg_open()) ) {
    PDBG("pg_open() fails: %s", pg_errmsg());
    goto end;
  }

  if ( !(rc = pg_exec_query(pgc, "select * from objects where name='%s'", name)) ) {
    PDBG("pg_exec_query() fails: %s", pg_errmsg());
    goto end;
  }

  n = pg_getvalues(rc, 0,
      "%63s", objname,
      "%15s", objtype,
      "%255c", objsource,
      "%255c", objopts,
      "%d", &re,
      "%d", &genpts,
      NULL);

  if ( n < 4 ) {
    PDBG("pg_getvalues() fails: n=%d, errmsg='%s'", n, pg_errmsg());
    goto end;
  }


  switch ( (*type = str2objtype(objtype)) ) {
    case object_type_input : {
      struct ff_input_params * input = &params->input;
      if ( *objsource ) {
        input->source = strdup(objsource);
      }
      if ( *objopts ) {
        input->opts = strdup(objopts);
      }
      input->re = re;
      input->genpts = genpts;
      fok = true;
    }
    break;

    case object_type_encoder : {
      struct ff_encoder_params * encoder = &params->encoder;
      if ( *objsource ) {
        encoder->source = strdup(objsource);
      }
      if ( *objopts ) {
        encoder->opts = strdup(objopts);
      }
      fok = true;
    }
    break;

    case object_type_mixer : {
      // fixme: mixer not implemented
    }
    break;
    case object_type_decoder : {
      // fixme: special decoder setting not implemented
    }
    break;

    default:
      break;
  }

end:;

  PQclear(rc);
  pg_close(&pgc);

  return fok;
}
