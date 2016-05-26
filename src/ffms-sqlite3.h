/*
 * ffms-sqlite3.h
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffms_sqlite3_h__
#define __ffms_sqlite3_h__

#include "ffdb.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ffms_sqlite3_setup(void);
bool ffms_sqlite3_find_object(const char * name, enum ffobject_type * type, ffobj_params * params);



#ifdef __cplusplus
}
#endif

#endif /* __ffms_sqlite3_h__ */
