/*
 * ffms-ssl.h
 *
 *  Created on: May 27, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffms_ssl_h__
#define __ffms_ssl_h__


#include <stddef.h>
#include <stdbool.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include "coscheduler.h"



#ifdef __cplusplus
extern "C" {
#endif


SSL_CTX * ffms_get_ssl_context(void);
void ffms_destroy_ssl_context(SSL_CTX ** ssl_ctx);

SSL * ffms_ssl_new(SSL_CTX * ssl_ctx, struct cosocket * so);
void ffms_ssl_free(SSL ** ssl);

#ifdef __cplusplus
}
#endif

#endif /* __ffms_ssl_h__ */
