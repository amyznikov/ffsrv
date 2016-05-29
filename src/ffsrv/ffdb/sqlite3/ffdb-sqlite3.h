/*
 * ffdb-sqlite3.h
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffdb_sqlite3_h__
#define __ffdb_sqlite3_h__

#include "ffdb.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ffdb_sqlite3_init(void);
bool ffdb_sqlite3_find_object(const char * name, enum ffobject_type * type, ffobj_params * params);



#ifdef __cplusplus
}
#endif

#endif /* __ffdb_sqlite3_h__ */
