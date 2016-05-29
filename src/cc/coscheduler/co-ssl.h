/*
 * co-ssl.h
 *
 *  Created on: May 27, 2016
 *      Author: amyznikov
 */

#pragma once

#ifndef __ffsrv_ssl_h__
#define __ffsrv_ssl_h__


#include <stddef.h>
#include <stdbool.h>
#include <openssl/conf.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "../coscheduler/co-scheduler.h"


#ifdef __cplusplus
extern "C" {
#endif


SSL_CTX * co_ssl_create_context(const char * certfile, const char * keyfile);
void co_ssl_delete_context(SSL_CTX ** ssl_ctx);
SSL * co_ssl_new(SSL_CTX * ssl_ctx, struct cosocket * so);
void co_ssl_free(SSL ** ssl);

#ifdef __cplusplus
}
#endif

#endif /* __ffsrv_ssl_h__ */
