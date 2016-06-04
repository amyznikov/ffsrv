/*
 * sockopt.h
 *
 *  Created on: Oct 6, 2015
 *      Author: amyznikov
 */


#ifndef __sockopt_h__
#define __sockopt_h__

#include <inttypes.h>
#include <sys/types.h>
#include <sys/socket.h>




#ifdef __cplusplus
extern "C" {
#endif


int so_geterror(int so);

int so_set_sendbuf(int so, int size);
int so_get_sendbuf(int so, int * size);

int so_set_recvbuf(int so, int size);
int so_get_recvbuf(int so, int * size);

int so_set_send_timeout(int so, int sec);
int so_get_send_timeout(int so, int * sec);

int so_set_recv_timeout(int so, int sec);
int so_get_recv_timeout(int so, int * sec);

int so_set_nodelay(int so, int optval);
int so_get_nodelay(int so, int * optval);

int so_set_reuse_addrs(int so, int optval);
int so_get_reuse_addrs(int so, int * optval);

int so_is_listening(int so);

int so_set_noblock(int so, int optval);

int so_set_keepalive(int so, int keepalive, int keepidle, int keepintvl, int keepcnt);

int so_resolve(const char * servername, uint32_t * address, uint16_t * port);

int so_close_connection(int so, int abort);

int so_bind(int so, uint32_t addrs, uint16_t port);

int so_tcp_connect(const char * servername, uint16_t port, int tmo_sec);
int so_tcp_listen(uint32_t addrs, uint16_t port);
int so_tcp_listen_addrs(const struct sockaddr * addrs, socklen_t addrslen);

int so_ssend(int so, const char * format, ... ) __attribute__ ((__format__ (__printf__, 2, 3)));

ssize_t so_srecv(int so, char line[], size_t size);

int so_get_outq_size(int so);


#ifdef __cplusplus
}
#endif

#endif /* __sockopt_h__ */
