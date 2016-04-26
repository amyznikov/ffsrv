/*
 * coscheduler.h
 *
 *  Created on: Apr 5, 2016
 *      Author: amyznikov
 */

//#pragma once

#ifndef __co_scheduler_h__
#define __co_scheduler_h__

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <pcl.h>
#include <poll.h>
#include <sys/epoll.h>


#ifdef __cplusplus
extern "C" {
#endif


bool co_scheduler_init(int ncpu);
bool is_in_cothread(void);


bool co_schedule(void (*fn)(void*), void * arg, size_t stack_size);
bool co_schedule_io(int so, uint32_t events, int (*callback)(void * arg, uint32_t events),
    void * arg, size_t stack_size);





typedef struct coevent
  coevent;
typedef struct coevent_waiter
  coevent_waiter;

struct coevent * coevent_create(void);
void coevent_delete(struct coevent ** e);
struct coevent_waiter * coevent_add_waiter(struct coevent * e);
void coevent_remove_waiter(struct coevent * e, struct coevent_waiter * ew);
bool coevent_wait(struct coevent_waiter * ew, int tmo);
bool coevent_set(struct coevent * e);



struct cosocket;
struct cosocket * cosocket_create(int so);
void cosocket_delete(struct cosocket ** cc);
ssize_t cosocket_send(struct cosocket * cc, const void * buf, size_t size, int flags);
ssize_t cosocket_recv(struct cosocket * cc, void * buf, size_t size, int flags);


void co_sleep(uint64_t usec);
void co_yield(void);

int  co_poll(struct pollfd *__fds, nfds_t __nfds, int __timeout);
ssize_t co_send(int so, const void * buf, size_t size, int flags);
ssize_t co_recv(int so, void * buf, size_t size, int flags);
ssize_t co_read(int fd, void * buf, size_t size);


#ifdef __cplusplus
}
#endif

#endif /* __co_scheduler_h__ */
