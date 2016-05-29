/*
 * ffdb-pg.h
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffdb_pg_h__
#define __ffdb_pg_h__

#include "ffdb.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ffdb_pg_init(void);
bool ffdb_pg_find_object(const char * name, enum ffobject_type * type, ffobj_params * params);



#ifdef __cplusplus
}
#endif

#endif /* __ffdb_pg_h__ */
