/*
 * ffms-txtfile.h
 *
 *  Created on: May 25, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffms_txtfile_h__
#define __ffms_txtfile_h__

#include "ffms-db.h"

#ifdef __cplusplus
extern "C" {
#endif

bool ffms_txtfile_setup(void);
bool ffms_txtfile_find_object(const char * name, enum ffobject_type * type, ffobj_params * params);


#ifdef __cplusplus
}
#endif

#endif /* __ffms_txtfile_h__ */
