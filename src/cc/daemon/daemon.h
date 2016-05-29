/*
 * daemon.h
 *
 *  Created on: Oct 2, 2015
 *      Author: amyznikov
 */


#ifndef __daemon_h__
#define __daemon_h__

#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif


/**
 * setup_signal_handler()
 *    see errno on failure
 */
bool setup_signal_handler(void);


/**
 * fork() and become daemon
 *    see errno on failure
 */
pid_t become_daemon(void);


#ifdef __cplusplus
}
#endif

#endif /* __daemon_h__ */
