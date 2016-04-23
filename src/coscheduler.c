/*
 * coscheduler.c
 *
 *  Created on: Apr 5, 2016
 *      Author: amyznikov
 */

#include "coscheduler.h"
#include "cclist.h"
#include "pthread_wait.h"
#include "sockopt.h"
#include <inttypes.h>
#include <time.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include "debug.h"


#define UNMASKED_EVENTS (EPOLLERR|EPOLLHUP|EPOLLRDHUP)

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static struct {
  pthread_wait_t lock;
  int eso;
} emgr = {
  .lock = PTHREAD_WAIT_INITIALIZER,
  .eso = -1,
};

enum {
  iowait_io,
  iowait_eventfd,
};

struct io_waiter {
  int64_t tmo;
  struct io_waiter * prev, * next;
  coroutine_t co;
  uint32_t mask;
  uint32_t events;
  uint32_t revents;
};

struct iorq {
  struct io_waiter * w;
  int so;
  int type;
};


static inline int64_t gettime(void)
{
  struct timespec tm = {};
  clock_gettime(CLOCK_MONOTONIC, &tm);
  return ((int64_t)tm.tv_sec * 1000 + (int64_t) (tm.tv_nsec / 1000000));
}

//static inline int64_t gettime_us(void)
//{
//  struct timespec tm = {};
//  clock_gettime(CLOCK_MONOTONIC, &tm);
//  return ((int64_t)tm.tv_sec * 1000000 + (int64_t) (tm.tv_nsec / 1000));
//}

static inline void emgr_lock(void)
{
  pthread_wait_lock(&emgr.lock);
}

static inline void emgr_unlock(void)
{
  pthread_wait_unlock(&emgr.lock);
}

static inline int emgr_wait(int tmo)
{
  return pthread_wait(&emgr.lock, tmo);
}

static inline void emgr_signal(void)
{
  pthread_wait_broadcast(&emgr.lock);
}

static inline bool epoll_add(struct iorq * e, uint32_t events)
{
  int status;

  status = epoll_ctl(emgr.eso, EPOLL_CTL_ADD, e->so,
      &(struct epoll_event ) {
            .data.ptr = e,
            .events = (events | ((events & EPOLLONESHOT) ? 0 : EPOLLET))
            });

  return status == 0;
}

static inline bool epoll_remove(int so)
{
  return epoll_ctl(emgr.eso, EPOLL_CTL_DEL, so, NULL) == 0;
}

static inline int epoll_wait_events(struct epoll_event events[], int nmax)
{
  int n;
  while ( (n = epoll_wait(emgr.eso, events, nmax, -1)) < 0 && errno == EINTR )
    {}
  return n;
}

// always insert into head
static inline void epoll_queue(struct iorq * e, struct io_waiter * w)
{
  if ( w ) {

    emgr_lock();

    if ( e->w ) {
      e->w->prev = w;
      w->next = e->w;
    }
    e->w = w;

    emgr_unlock();
  }
}

static inline void epoll_dequeue(struct iorq * e, struct io_waiter * w)
{
  if ( w ) {

    emgr_lock();

    if ( w->prev ) {
      w->prev->next = w->next;
    }

    if ( w->next ) {
      w->next->prev = w->prev;
    }

    if ( e->w == w ) {
      e->w = w->next;
    }

    emgr_unlock();
  }
}


static void * epoll_listener_thread(void * arg)
{
  (void) (arg);

  const int MAX_EPOLL_EVENTS = 5000;
  struct epoll_event events[MAX_EPOLL_EVENTS];

  struct iorq * e;
  struct io_waiter * w;

  int i, n, c;

  pthread_detach(pthread_self());


  PDBG("enter");

  while ( (n = epoll_wait_events(events, MAX_EPOLL_EVENTS)) >= 0 ) {

    emgr_lock();

    for ( i = 0, c = 0; i < n; ++i ) {

      e = events[i].data.ptr;

      if ( e->type == iowait_eventfd ) {
        eventfd_t x;
        while ( eventfd_read(e->so, &x) == 0 ) {}
      }

      for ( w = e->w; w; w = w->next ) {
        if ( ((w->events |= events[i].events) & w->mask) && w->co ) {
          ++c;
        }
      }
    }

    if ( c ) {
      emgr_signal();
    }

    emgr_unlock();
  }

  PDBG("leave");
  return NULL;
}


static bool emgr_init()
{
  static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;

  pthread_t pid;
  int status = 0;

  pthread_mutex_lock(&mtx);

  if ( emgr.eso == -1 ) {

    if ( (emgr.eso = epoll_create1(0)) == -1 ) {
      PDBG("epoll_create1() fails: %s", strerror(errno));
      status = -1;
    }
    else if ( (status = pthread_create(&pid, NULL, epoll_listener_thread, NULL)) ) {
      PDBG("pthread_create() fails: %s", strerror(errno = status));
      close(emgr.eso), emgr.eso = -1;
    }
  }

  pthread_mutex_unlock(&mtx);

  return status == 0;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct co_scheduler_context {
  coroutine_t main;
  ccfifo queue;
  cclist waiters;
  int cpu_load;
};

static __thread struct co_scheduler_context
  * gs = NULL;


bool is_in_cothread(void)
{
  return gs != NULL;
}


bool co_scheduler_init(void)
{
  bool fok = false;

  if ( gs != NULL ) {
    fok = true;
    goto end;
  }

  if ( !(gs = calloc(1, sizeof(*gs))) ) {
    goto end;
  }

  if ( !ccfifo_init(&gs->queue, 1000, sizeof(coroutine_t)) ) {
    goto end;
  }

  if ( !cclist_init(&gs->waiters, 10000, sizeof(struct io_waiter)) ) {
    goto end;
  }

  if ( !emgr_init() ) {
    goto end;
  }

  if ( co_thread_init() != 0 ) {
    goto end;
  }

  if ( !(gs->main = co_current()) ) {
    goto end;
  }


  fok = true;

end:;

  if ( !fok && gs != NULL ) {
    cclist_cleanup(&gs->waiters);
    ccfifo_cleanup(&gs->queue);
    free(gs), gs = NULL;
  }

  return fok;
}

void co_scheduler_exit(void)
{
  co_thread_cleanup();
}


static inline struct cclist_node * add_waiter(struct io_waiter * w)
{
  w->mask |= UNMASKED_EVENTS;
  return cclist_push_back(&gs->waiters, w);
}

static inline void remove_waiter(struct cclist_node * node)
{
  cclist_erase(&gs->waiters, node);
}


static inline int walk_waiters_list(int64_t ct, coroutine_t cc[], int ccmax, int *wtmo)
{
  struct cclist_node * node, * next_node = NULL;
  struct io_waiter * w;
  int dt, tmo;
  int n;

  for ( n = 0, tmo = INT_MAX, node = cclist_head(&gs->waiters); node; node = next_node ) {

    // send cache request for next node as early as possible ?
    next_node = node->next;

    if ( !(w = cclist_peek(node))->co ) {
      continue;
    }

    if ( w->events & w->mask ) {    // | UNMASKED_EVENTS
      cc[n++] = w->co, w->revents = w->events, w->events &= ~w->mask;
      if ( n == ccmax ) {
        break;
      }
    }

    if ( w->tmo == 0 ) {
      PDBG("BUG: temp->tmo==0");
      exit(1);
    }

    if ( w->tmo > 0 ) {

      if ( ct >= w->tmo ) {
        cc[n++] = w->co, w->revents = w->events, w->events &= ~w->mask;
        if ( n == ccmax ) {
          break;
        }
      }

      if ( (dt = (int)(w->tmo - ct)) < tmo ) {
        tmo = dt;
      }
    }
  }

  *wtmo = tmo;

  return n;
}


void co_scheduler_run(void)
{
  const int ccmax = 1000;
  coroutine_t cc[ccmax];
  coroutine_t co;
  int n, tmo;

  emgr_lock();

  while ( 42 ) {

    while ( ccfifo_pop(&gs->queue, &co) ) {
      emgr_unlock();
      co_call(co);
      emgr_lock();
    }

    if ( !(n = walk_waiters_list(gettime(), cc, ccmax, &tmo)) ) {
      emgr_wait(tmo);
    }
    else {
      emgr_unlock();
      for ( int i = 0; i < n; ++i ) {
        co_call(cc[i]);
      }
      emgr_lock();
    }

  }

  emgr_unlock();
}



//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool co_schedule(void (*fn)(void*), void * arg, size_t stack_size)
{
  coroutine_t co = NULL;

  // PDBG("enter");

  if ( ccfifo_is_full(&gs->queue) ) {
    PDBG("BUG: ccfifo_is_full()");
    exit(1);
    goto end;
  }

  if ( !(co = co_create(fn, arg, 0, stack_size)) ) {
    PDBG("FATAL: co_create() fails: %s", strerror(errno));
    exit(1);
    goto end;
  }

  ccfifo_push(&gs->queue, &co);

end:

  // PDBG("leave: co=%p", co);
  return co != NULL;
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void co_sleep(uint64_t usec)
{
  struct cclist_node * node;

  node = add_waiter(&(struct io_waiter ) {
        .co = co_current(),
        .tmo = gettime() + usec / 1000,
      });

  co_call(gs->main);

  remove_waiter(node);
}

void co_yield(void)
{
  co_sleep(0);
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct coevent {
  struct iorq e;
};

struct coevent * coevent_create(void)
{
  struct coevent * e = NULL;
  bool fok = false;

  if ( !(e = calloc(1, sizeof(*e))) ) {
    PDBG("calloc(struct co_event) fails: %s", strerror(errno));
    goto end;
  }

  e->e.type = iowait_eventfd;
  e->e.w = NULL;

  if ( !(e->e.so = eventfd(0,0)) ) {
    PDBG("eventfd() fails: %s", strerror(errno));
    goto end;
  }

  if ( so_set_noblock(e->e.so, true) != 0 ) {
    PDBG("so_set_noblock(so=%d) fails: %s", e->e.so, strerror(errno));
    exit(1);
  }

  if ( !epoll_add(&e->e, EPOLLIN) ) {
    PDBG("emgr_add() fails: emgr.eso=%d e->e.so=%d errno=%d %s", emgr.eso, e->e.so, errno, strerror(errno));
    goto end;
  }

  fok = true;

end: ;

  if ( !fok && e ) {
    if ( e->e.so != -1 ) {
      close(e->e.so);
    }
    free(e);
    e = NULL;
  }

  return e;
}


void coevent_delete(struct coevent ** e)
{
  if ( e && *e ) {
    if ( (*e)->e.so != -1 ) {
      epoll_remove((*e)->e.so);
      close((*e)->e.so);
    }
    free(*e);
    *e = NULL;
  }
}



struct coevent_waiter * coevent_add_waiter(struct coevent * e)
{
  struct cclist_node * node;
  struct io_waiter * w;

  node = add_waiter(&(struct io_waiter ) {
        .co = NULL,
        .mask = EPOLLIN,
        .tmo = -1,
      });

  epoll_queue(&e->e, w = cclist_peek(node));

  return (struct coevent_waiter * )node;
}

void coevent_remove_waiter(struct coevent * e, struct coevent_waiter * ww)
{
  struct cclist_node * node = (struct cclist_node * )ww;
  struct io_waiter * w = cclist_peek(node);
  epoll_dequeue(&e->e, w);
  remove_waiter(node);
}

bool coevent_wait(struct coevent_waiter * ww, int tmo)
{
  struct cclist_node * node = (struct cclist_node * )ww;
  struct io_waiter * w = cclist_peek(node);

  w->co = co_current();
  w->tmo = tmo >= 0 ? gettime() + tmo : -1;
  co_call(gs->main);
  w->co = NULL;

  return w->revents != 0;
}

bool coevent_set(struct coevent * e)
{
  if ( !e ) {
    errno = EINVAL;
    return false;
  }
  return (eventfd_write(e->e.so, 1) == 0);
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct cosocket {
  struct iorq e;
};

struct cosocket * cosocket_create(int so)
{
  struct cosocket * cc = calloc(1, sizeof(struct cosocket));
  cc->e.so = so;
  cc->e.type = iowait_io;
  cc->e.w = NULL;

  if ( !epoll_add(&cc->e, EPOLLIN | EPOLLOUT) ) {
    PDBG("emgr_add() fails: %s", strerror(errno));
    exit(1);
  }

  return cc;
}

void cosocket_delete(struct cosocket ** cc)
{
  if ( cc && *cc ) {
    epoll_remove((*cc)->e.so);
    free(*cc);
    *cc = NULL;
  }
}

ssize_t cosocket_send(struct cosocket * cc, const void * buf, size_t buf_size, int flags)
{
  const uint8_t * ptr = buf;
  ssize_t sent = 0;
  ssize_t size;

  struct cclist_node * node =
      add_waiter(&(struct io_waiter ) {
          .co = co_current(),
          .tmo = -1,
          .mask = EPOLLOUT
          });

  struct io_waiter * w = cclist_peek(node);

  epoll_queue(&cc->e, w);

  while ( sent < (ssize_t) buf_size ) {
    if ( (size = send(cc->e.so, ptr + sent, buf_size - sent, flags | MSG_NOSIGNAL | MSG_DONTWAIT)) > 0 ) {
      sent += size;
      // PDBG("[<%p> SO=%d] size=%zd buf_size=%zu", co_current(), cc->e.so, size, buf_size);
    }
    else if ( errno == EAGAIN ) {

//      PDBG("[EAGAIN <%p> %d] size=%zd sent=%zd buf_size=%zu", co_current(), cc->e.so, size, sent, buf_size);
      co_call(gs->main);

      if ( !(w->revents & EPOLLOUT) ) {
        PDBG("strange w->events=0x%0X so=%d buf_size=%zu size=%zd sent=%zd errno=%s", w->revents, cc->e.so, buf_size,
            size, sent, strerror(errno));
        exit(1);
      }

    }
    else {
      //PDBG("send(so=%d) fails: buf_size=%zu size=%zd sent=%zd errno=%s", cc->e.so, buf_size, size, sent, strerror(errno));
      sent = size;
      break;
    }
  }

  epoll_dequeue(&cc->e, w);
  remove_waiter(node);

  // PDBG("[%d] %zu", cc->e.so, sent);

  return sent;
}

ssize_t cosocket_recv(struct cosocket * cc, void * buf, size_t buf_size, int flags)
{
  ssize_t size;

  struct cclist_node * node =
      add_waiter(&(struct io_waiter ) {
        .co = co_current(),
        .tmo = -1,
        .mask = EPOLLIN
      });

  epoll_queue(&cc->e, cclist_peek(node));

  while ( (size = recv(cc->e.so, buf, buf_size, flags | MSG_DONTWAIT | MSG_NOSIGNAL)) < 0 && errno == EAGAIN ) {
    co_call(gs->main);
  }

  epoll_dequeue(&cc->e, cclist_peek(node));
  remove_waiter(node);
  return size;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




int co_poll(struct pollfd *__fds, nfds_t __nfds, int __timeout)
{
  struct {
    struct iorq e;
    struct cclist_node * node;
  } c[__nfds];

  __timeout = 5000;

  int64_t tmo = __timeout >= 0 ? gettime() + __timeout : -1;
  coroutine_t co = co_current();
  uint32_t event_mask;

  int n = 0;

  // PDBG("***** C POLL __timeout=%d", __timeout);


  for ( nfds_t i = 0; i < __nfds; ++i ) {

    //PDBG("fd[%lu]=%d __timeout=%d", i, __fds[i].fd, __timeout );

    event_mask = ((__fds[i].events & POLLIN) ? EPOLLIN : 0) | ((__fds[i].events & POLLOUT) ? EPOLLOUT : 0);
    __fds[i].revents = 0;

    // PDBG("[<%p> SO=%d] E=0x%0X", co_current(), __fds[i].fd, event_mask);

    c[i].e.so = __fds[i].fd;
    c[i].e.type = iowait_io;
    c[i].e.w = cclist_peek(c[i].node =
        add_waiter(&(struct io_waiter ) {
          .co = co,
          .tmo = tmo,
          .mask = event_mask,
        }));


    if ( !c[i].node ) {
      PDBG("add_waiter() fails: %s", strerror(errno));
      exit(1);
    }

    if ( !epoll_add(&c[i].e, event_mask | EPOLLONESHOT) ) {
      PDBG("emgr_add() fails: %s", strerror(errno));
      exit(1);
    }
  }

  co_call(gs->main);

  for ( nfds_t i = 0; i < __nfds; ++i ) {

    epoll_remove(c[i].e.so);

    if ( (__fds[i].events & POLLIN) && (c[i].e.w->revents & EPOLLIN) ) {
      __fds[i].revents |= POLLIN;
    }

    if ( (__fds[i].events & POLLOUT) && (c[i].e.w->revents & EPOLLOUT) ) {
      __fds[i].revents |= POLLOUT;
    }

    if ( (c[i].e.w->revents & EPOLLERR) ) {
      __fds[i].revents |= POLLERR;
    }

    if ( __fds[i].revents ) {
      ++n;
    }

    remove_waiter(c[i].node);
  }

  //PDBG("***** R POLL: n=%d", n);
  return n;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


static bool co_io_wait(int so, uint32_t events, int tmo)
{
  struct cclist_node * node;

  struct iorq e = {
    .so = so,
    .type = iowait_io,
    .w = cclist_peek(node =
        add_waiter(&(struct io_waiter ) {
          .co = co_current(),
          .tmo = tmo,
          .mask = events,
        })),
  };

  if ( !epoll_add(&e, events) ) {
    PDBG("emgr_add(so=%d) fails: %s", so, strerror(errno));
    exit(1);
  }

  co_call(gs->main);

  epoll_remove(so);

  remove_waiter(node);

  return true;
}


ssize_t co_send(int so, const void * buf, size_t buf_size, int flags)
{
  const uint8_t * ptr = buf;
  ssize_t sent = 0;
  ssize_t size;

  //PDBG("enter");

  while ( sent < (ssize_t) buf_size ) {
    if ( (size = send(so, ptr + sent, buf_size - sent, flags | MSG_DONTWAIT)) > 0 ) {
      sent += size;
    }
    else if ( errno != EAGAIN || !co_io_wait(so, EPOLLOUT, -1) ) {
      break;
    }
  }

  // PDBG("leave: sent=%zd", sent);
  return sent;
}

ssize_t co_recv(int so, void * buf, size_t buf_size, int flags)
{
  ssize_t size;

  //PDBG("enter");

  while ( (size = recv(so, buf, buf_size, flags | MSG_DONTWAIT)) < 0 && errno == EAGAIN ) {
    if ( !co_io_wait(so, EPOLLIN, -1) ) {
      break;
    }
  }

  //PDBG("leave: size=%zd", size);
  return size;
}


ssize_t co_read(int fd, void * buf, size_t buf_size)
{
  ssize_t size;


  //PDBG("enter");

  while ( (size = read(fd, buf, buf_size)) < 0 && errno == EAGAIN ) {
    if ( !co_io_wait(fd, EPOLLIN, -1) ) {
      break;
    }
  }

  //PDBG("leave: size=%zd", size);
  return size;
}



//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


struct iocb {
  struct iorq e;
  struct cclist_node * node;
  int (*fn)(void * cookie, uint32_t events);
  void * cookie;
};


static void iocb_handler(void * arg)
{
  struct iocb * cb = arg;

  PDBG("ENTER");

  while ( cb->fn(cb->cookie, cb->e.w->revents) == 0 ) {
    //PDBG("CALL gs->main");
    co_call(gs->main);
    //PDBG("revents=0x%0X", cb->e.w->revents);
  }

  epoll_remove(cb->e.so);
  remove_waiter(cb->node);
  free(cb);

  PDBG("LEAVE");
}


bool co_schedule_io(int (*fn)(void * cookie, uint32_t events ), int so, uint32_t events, void * cookie,
    size_t stack_size)
{
  struct iocb * cb = NULL;
  bool fok = false;

  if ( !(cb = calloc(1, sizeof(*cb))) ) {
    PDBG("calloc(cb) fails: %s", strerror(errno));
    goto end;
  }

  cb->fn = fn;
  cb->cookie = cookie;
  cb->e.so = so;
  cb->e.type = iowait_io;
  cb->e.w = cclist_peek(cb->node =
      add_waiter(&(struct io_waiter ) {
        .co = co_create(iocb_handler, cb, NULL, stack_size),
        .mask = events,
        .tmo = -1,
      }));


  if ( !cb->e.w->co ) {
    PDBG("co_create(iocb_handler) fails: %s", strerror(errno));
    exit(1);
  }

  if ( !epoll_add(&cb->e, events) ) {
    PDBG("emgr_add(so=%d) fails: %s", so, strerror(errno));
    exit(1);
  }


  fok = true;

end:


  return fok;
}

