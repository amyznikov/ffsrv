/*
 * ffdb-sqlite3.c
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#include "ffms-sqlite3.h"


bool ffms_sqlite3_setup(void)
{
  return false;
}

bool ffms_sqlite3_find_object(const char * name, enum ffobject_type * type, ffobj_params * params)
{
  (void)(name);
  (void)(type);
  (void)(params);
  return false;
}
