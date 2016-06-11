/*
 * ffdb-pg.c
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#include "ffcfg.h"
#include "ffdb-pg.h"
//#include "pgdb.h"
#include "debug.h"



bool ffdb_pg_init(void)
{
  return false;
#if 0  
  pg_setup(ffsrv.db.pg.host, ffsrv.db.pg.port, ffsrv.db.pg.db, ffsrv.db.pg.user, ffsrv.db.pg.psw, ffsrv.db.pg.options,
      ffsrv.db.pg.tty);
  return true;
#endif  
}

bool ffdb_pg_find_object(const char * name, enum ffobject_type * type, ffobj_params * params)
{
  return false;
  #if 0 
  PgConnection * pgc = NULL;
  PGresult * rc = NULL;
  bool fok = false;
  int n;

  char objname[64] = "";
  char objtype[16] = "";
  char objsource[256] = "";
  char objopts[256] = "";
  int  re = 0, genpts = 0, rtmo = 0, itmo = 0;

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
      "%d", &rtmo,
      "%d", &itmo,
      NULL);


  if ( n < 4 ) {
    PDBG("pg_getvalues() fails: n=%d, errmsg='%s'", n, pg_errmsg());
    goto end;
  }

//  PDBG("objname=%s", objname);
//  PDBG("objtype=%s", objtype);
//  PDBG("objsource=%s", objsource);
//  PDBG("objopts=%s", objopts);
//  PDBG("re=%d", re);
//  PDBG("genpts=%d", genpts);
//  PDBG("rtmo=%d", rtmo);
//  PDBG("itmo=%d", itmo);

  switch ( (*type = str2objtype(objtype)) ) {
    case object_type_input : {
      struct ffinput_params * input = &params->input;
      if ( *objsource ) {
        input->source = strdup(objsource);
      }
      if ( *objopts ) {
        input->opts = strdup(objopts);
      }
      input->re = re;
      input->genpts = genpts;
      input->rtmo = rtmo;
      input->itmo = itmo;
      fok = true;
    }
    break;

    case object_type_encoder : {
      struct ffencoder_params * encoder = &params->encoder;
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
#endif  
}
