/*
 * coscheduler.c
 *
 *  Created on: Apr 5, 2016
 *      Author: amyznikov
 */

#include "co-scheduler.h"
#include "cclist.h"
#include "pthread_wait.h"
#include "sockopt.h"
#include "cdns.h"
#include "ipaddrs.h"
#include "strfuncs.h"

#include <inttypes.h>
#include <time.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <arpa/inet.h>

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

struct io_waiter {
  int64_t tmo;
  struct io_waiter * prev, * next;
  coroutine_t co;
  uint32_t mask;
  uint32_t events;
  uint32_t revents;
};

enum {
  iowait_io,
  iowait_eventfd
};

struct iorq {
  struct io_waiter * w;
  int so;
  int type;
};

static inline int64_t gettime_ms(void)
{
  struct timespec tm = { .tv_sec = 0, .tv_nsec = 0 };
  clock_gettime(CLOCK_MONOTONIC, &tm);
  return ((int64_t) tm.tv_sec * 1000 + (int64_t) (tm.tv_nsec / 1000000));
}

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
  int cs[2];
  pthread_spinlock_t cslock;
  bool started :1, csbusy : 1;
};


struct creq {
  union {
    struct {
      int (*callback)(void *, uint32_t);
      void * callback_arg;
      size_t stack_size;
      uint32_t flags;
      int so;
    } io;

    struct {
      void (*func)(void*);
      void * arg;
      size_t stack_size;
    } thread;
  };

  enum {
    creq_schedule_io = 1,
    creq_start_cothread = 2,
  } req;
};


struct iocb {
  struct iorq e;
  struct cclist_node * node;
  int (*fn)(void * cookie, uint32_t events);
  void * cookie;
};



static struct co_scheduler_context
  ** g_sched_array = NULL;

static int g_ncpu = 0;

static __thread struct co_scheduler_context
  * current_core = NULL;


bool is_in_cothread(void)
{
  return current_core != NULL;
}

static inline struct cclist_node * add_waiter(struct co_scheduler_context * core, struct io_waiter * w)
{
  struct cclist_node * node;

  w->mask |= UNMASKED_EVENTS;
  node = cclist_push_back(&core->waiters, w);

  return node;
}

static inline void remove_waiter(struct co_scheduler_context * core, struct cclist_node * node)
{
  cclist_erase(&core->waiters, node);
}


static void iocb_handler(void * arg)
{
  struct iocb * cb = arg;

  while ( cb->fn(cb->cookie, cb->e.w->revents) == 0 ) {
    co_call(current_core->main);
  }

  epoll_remove(cb->e.so);
  remove_waiter(current_core, cb->node);
  free(cb);
}




static inline void cslock(struct co_scheduler_context * core)
{
  pthread_spin_lock(&core->cslock);
  while ( core->csbusy ) {
    pthread_spin_unlock(&core->cslock);
    co_yield();
    pthread_spin_lock(&core->cslock);
  }

  core->csbusy = true;
  pthread_spin_unlock(&core->cslock);
}

static inline void csunlock(struct co_scheduler_context * core)
{
  pthread_spin_lock(&core->cslock);
  core->csbusy = false;
  pthread_spin_unlock(&core->cslock);
}

static inline int send_creq(struct co_scheduler_context * core, const struct creq * rq)
{
  int32_t status = 0;

  if ( !is_in_cothread() ) {
    if ( send(core->cs[0], rq, sizeof(*rq), MSG_NOSIGNAL) != (ssize_t) sizeof(*rq) ) {
      status = errno;
    }
    else if ( recv(core->cs[0], &status, sizeof(status), MSG_NOSIGNAL) != (ssize_t) sizeof(status) ) {
      status = errno;
    }
  }
  else {

    cslock(core);

    if ( co_send(core->cs[0], rq, sizeof(*rq), 0) != (ssize_t) sizeof(*rq) ) {
      status = errno;
    }
    else if ( co_recv(core->cs[0], &status, sizeof(status), 0) != (ssize_t) sizeof(status) ) {
      status = errno;
    }

    csunlock(core);
  }

  return status;
}


static void creq_listener(void * arg)
{
  (void)(arg);
  coroutine_t co;
  struct iocb * cb;
  struct creq creq;
  ssize_t size;
  int32_t status;

  PDBG("ENTER: current_core = %p", current_core);

  while ( (size = co_recv(current_core->cs[1], &creq, sizeof(creq), 0)) == (ssize_t) sizeof(creq) ) {

    errno = 0;

    if ( creq.req == creq_start_cothread ) {


      if ( ccfifo_is_full(&current_core->queue) ) {
        status = EBUSY;
      }
      else if ( !(co = co_create(creq.thread.func, creq.thread.arg, 0, creq.thread.stack_size)) ) {
        status = errno ? errno : ENOMEM;
      }
      else {
        emgr_lock();
        ccfifo_ppush(&current_core->queue, co);
        emgr_signal();
        emgr_unlock();

        status  = 0;
      }

    }
    else if ( creq.req == creq_schedule_io ) {

      if ( ccfifo_is_full(&current_core->queue) ) {
        status = EBUSY;
      }
      else if ( !(cb = calloc(1, sizeof(*cb))) ) {
        status = ENOMEM;
      }
      else if ( !(co = co_create(iocb_handler, cb, NULL, creq.io.stack_size)) ) {
        status = errno ? errno : ENOMEM;
      }
      else {
        cb->fn = creq.io.callback;
        cb->cookie = creq.io.callback_arg;
        cb->e.so = creq.io.so;
        cb->e.type = iowait_io;
        cb->e.w = cclist_peek(cb->node =
            add_waiter(current_core, &(struct io_waiter ) {
                  .co = co,
                  .mask = creq.io.flags,
                  .tmo = -1,
                }));

        if ( !epoll_add(&cb->e, creq.io.flags) ) {
          PDBG("BUG: epoll_add(so=%d) fails: %s", creq.io.so, strerror(errno));
          exit(1);
        }
        status = 0;
      }
    }


    if ( co_send(current_core->cs[1], &status, sizeof(status), 0) != sizeof(status) ) {
      PDBG("FATAL: cosocket_send(status) fails: %s", strerror(errno));
      exit(1);
    }

  }

  PDBG("LEAVE: cosocket_recv(creq) fails: %s", strerror(errno));
  exit(1);
}





static inline int walk_waiters_list(int64_t ct, coroutine_t cc[], int ccmax, int *wtmo)
{
  struct cclist_node * node, * next_node = NULL;
  struct io_waiter * w;
  int dt, tmo;
  int n;

  for ( n = 0, tmo = INT_MAX, node = cclist_head(&current_core->waiters); node; node = next_node ) {

    // send cache request for next node as early as possible ?
    next_node = node->next;

    if ( !(w = cclist_peek(node))->co ) {
      continue;
    }

    if ( w->events & w->mask ) {
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



static void * pclthread(void * arg)
{
  const int ccmax = 1000;
  coroutine_t cc[ccmax];
  coroutine_t co;

  int n, tmo;
  int64_t t0;

  pthread_detach(pthread_self());

  current_core = arg;
  PDBG("started: gs=%p", current_core);

  pthread_spin_init(&current_core->cslock, 0);

  if ( co_thread_init() != 0 ) {
    PDBG("FATAL: co_thread_init() fails");
    exit(1);
  }

  if ( !(current_core->main = co_current()) ) {
    PDBG("FATAL: co_current() fails");
    exit(1);
  }

  if ( !(co = co_create(creq_listener, NULL, NULL, 100 * 1024)) ) {
    PDBG("FATAL: co_create(creq_listener) fails");
    exit(1);
  }

  emgr_lock();
  ccfifo_ppush(&current_core->queue, co);

  current_core->started = true;

  while ( 42 ) {

    while ( ccfifo_pop(&current_core->queue, &co) ) {
      emgr_unlock();
      co_call(co);
      emgr_lock();
    }

    t0 = gettime_ms();

    if ( !(n = walk_waiters_list(t0, cc, ccmax, &tmo)) ) {
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


  co_thread_cleanup();
  pthread_spin_destroy(&current_core->cslock);

  PDBG("finished");
  return NULL;
}


static pthread_t new_pcl_thread(void)
{
  struct co_scheduler_context * ctx = NULL;
  pthread_t pid = 0;

  int status;

  if ( !(ctx = calloc(1, sizeof(*ctx))) ) {
    goto end;
  }

  ctx->cs[0] = ctx->cs[1] = -1;

//  if ( pthread_spin_init(&ctx->mtx, 0) != 0 ) {
//    goto end;
//  }

  if ( socketpair(AF_LOCAL, SOCK_STREAM, 0, ctx->cs) != 0 ) {
    goto end;
  }

  so_set_noblock(ctx->cs[1], true);

  if ( !ccfifo_init(&ctx->queue, 1000, sizeof(coroutine_t)) ) {
    goto end;
  }

  if ( !cclist_init(&ctx->waiters, 10000, sizeof(struct io_waiter)) ) {
    goto end;
  }

  g_sched_array[g_ncpu++] = ctx;

  if ( (status = pthread_create(&pid, NULL, pclthread, ctx)) ) {
    g_sched_array[--g_ncpu] = NULL;
    errno = status;
    goto end;
  }

  while ( !ctx->started ) {
    usleep(20 * 1000);
  }

end:

  if ( !pid && ctx ) {
    cclist_cleanup(&ctx->waiters);
    ccfifo_cleanup(&ctx->queue);

    for ( int i = 0; i < 2; ++i ) {
      if ( ctx->cs[i] != -1 ) {
        close(ctx->cs[i]);
      }
    }

//    pthread_spin_destroy(&current_core->mtx);
    free(ctx);
  }

  return pid;
}



//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool co_scheduler_init(int ncpu)
{
  bool fok = false;

  if ( ncpu < 1 ) {
    ncpu = 1;
  }

  if ( !emgr_init() ) {
    goto end;
  }

  if ( !(g_sched_array = calloc(ncpu, sizeof(struct co_scheduler_context*))) ) {
    goto end;
  }


  while ( ncpu > 0 && new_pcl_thread() ) {
    --ncpu;
  }

  fok = (ncpu == 0);

end:
  return fok;
}





bool co_schedule(void (*func)(void*), void * arg, size_t stack_size)
{
  struct co_scheduler_context * core;
  int status;

  core = g_sched_array[rand() % g_ncpu];

  status = send_creq(core, &(struct creq) {
        .req = creq_start_cothread,
        .thread.func = func,
        .thread.arg = arg,
        .thread.stack_size = stack_size
      });

  if ( status ) {
    errno = status;
  }

  return status == 0;
}

bool co_schedule_io(int so, uint32_t events, int (*callback)(void * arg, uint32_t events), void * arg,
    size_t stack_size)
{
  struct co_scheduler_context * core;
  int status;

  core = g_sched_array[rand() % g_ncpu];

  status = send_creq(core, &(struct creq) {
        .req = creq_schedule_io,
        .io.so = so,
        .io.flags = events,
        .io.callback = callback,
        .io.callback_arg = arg,
        .io.stack_size = stack_size
      });


  if ( status ) {
    errno = status;
  }

  return status == 0;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


void co_sleep(uint64_t usec)
{
  struct cclist_node * node;

  node = add_waiter(current_core, &(struct io_waiter ) {
        .co = co_current(),
        .tmo = gettime_ms() + (usec + 500) / 1000,
      });

  co_call(current_core->main);

  remove_waiter(current_core, node);
}

void co_yield(void)
{
  if ( is_in_cothread() ) {
    co_sleep(0);
  }
  else {
    sched_yield();
  }
}






//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct comtx {
  struct iorq e;
  coroutine_t co;
  pthread_spinlock_t sp;
};

struct comtx * comtx_create(void)
{
  struct comtx * mtx = NULL;
  bool fok = false;

  if ( !(mtx = calloc(1, sizeof(struct comtx))) ) {
    PDBG("calloc(struct comtx) fails: %s", strerror(errno));
    goto end;
  }

  pthread_spin_init(&mtx->sp, 0);

  mtx->e.type = iowait_eventfd;

  if ( !(mtx->e.so = eventfd(0, 0)) ) {
    PDBG("eventfd() fails: %s", strerror(errno));
    goto end;
  }


  if ( so_set_noblock(mtx->e.so, true) != 0 ) {
    PDBG("so_set_noblock(so=%d) fails: %s", mtx->e.so, strerror(errno));
    exit(1);
  }

  if ( !epoll_add(&mtx->e, EPOLLIN) ) {
    PDBG("epoll_add() fails: emgr.eso=%d mtx->e.so=%d errno=%d %s", emgr.eso, mtx->e.so, errno, strerror(errno));
    goto end;
  }

  fok = true;

end:

  if ( !fok && mtx ) {
    if ( mtx->e.so != -1 ) {
      close(mtx->e.so);
    }
    free(mtx);
    mtx = NULL;
  }

  return mtx;
}

void comtx_destroy(struct comtx ** mtx)
{
  if ( mtx && *mtx ) {

    if ( (*mtx)->e.so != -1 ) {
      epoll_remove((*mtx)->e.so);
      close((*mtx)->e.so);
    }

    pthread_spin_destroy(&(*mtx)->sp);

    free(*mtx), *mtx = NULL;
  }

}

void comtx_lock(struct comtx * mtx)
{
  struct cclist_node * node;

  pthread_spin_lock(&mtx->sp);

  while ( mtx->co ) {

    pthread_spin_unlock(&mtx->sp);

    node = add_waiter(current_core, &(struct io_waiter ) {
          .co = co_current(),
          .tmo = -1,
          .mask = EPOLLIN
        });

    epoll_queue(&mtx->e, cclist_peek(node));

    co_call(current_core->main);

    epoll_dequeue(&mtx->e, cclist_peek(node));
    remove_waiter(current_core, node);

    pthread_spin_lock(&mtx->sp);
  }

  mtx->co = co_current();
  pthread_spin_unlock(&mtx->sp);
}


void comtx_unlock(struct comtx * mtx)
{
  pthread_spin_lock(&mtx->sp);

  if ( mtx->co != co_current() ) {
    PDBG("Invalid call: mtx->co=%p co_current()=%p", mtx->co, co_current());
    raise(SIGINT);
    exit(1);
  }

  mtx->co = NULL;
  eventfd_write(mtx->e.so, 1);
  pthread_spin_unlock(&mtx->sp);
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct coevent {
  struct iorq e;
};

struct coevent_waiter {
  struct co_scheduler_context * core;
  struct cclist_node * node;
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
    PDBG("epoll_add() fails: emgr.eso=%d e->e.so=%d errno=%d %s", emgr.eso, e->e.so, errno, strerror(errno));
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
  struct coevent_waiter * ww = malloc(sizeof(struct coevent_waiter));

  if ( ww ) {

    ww->node = add_waiter(ww->core = current_core,
        &(struct io_waiter ) {
              .co = NULL,
              .mask = EPOLLIN,
              .tmo = -1,
            });

    epoll_queue(&e->e, cclist_peek(ww->node));
  }

  return ww;
}

void coevent_remove_waiter(struct coevent * e, struct coevent_waiter * w)
{
  if ( w ) {

    epoll_dequeue(&e->e, cclist_peek(w->node));
    remove_waiter(w->core, w->node);

    free(w);
  }
}

bool coevent_wait(struct coevent_waiter * ww, int tmo_ms)
{
  struct cclist_node * node = ww->node;
  struct io_waiter * w = cclist_peek(node);

  if ( current_core != ww->core ) {
    PDBG("APP BUG: core not match: current_core=%p ww->core=%p", current_core, ww->core);
    raise(SIGINT); // break for gdb
    exit(1);
  }

  w->co = co_current();
  w->tmo = tmo_ms >= 0 ? gettime_ms() + tmo_ms : -1;

  co_call(current_core->main);
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
  int rcvtmo, sndtmo;
};

struct cosocket * cosocket_create(int so)
{
  struct cosocket * cc = calloc(1, sizeof(struct cosocket));
  cc->e.so = so;
  cc->e.type = iowait_io;
  cc->e.w = NULL;
  cc->rcvtmo = cc->sndtmo = -1;

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


void cosocket_set_rcvtmout(struct cosocket * cc, int tmo_sec)
{
  cc->rcvtmo = tmo_sec;
}

void cosocket_set_sndtmout(struct cosocket * cc, int tmo_sec)
{
  cc->sndtmo = tmo_sec;
}

ssize_t cosocket_send(struct cosocket * cc, const void * buf, size_t buf_size, int flags)
{
  const uint8_t * ptr = buf;
  ssize_t sent = 0;
  ssize_t size;

  struct cclist_node * node =
      add_waiter(current_core, &(struct io_waiter ) {
          .co = co_current(),
          .tmo = cc->sndtmo >= 0 ? gettime_ms() + cc->sndtmo * 1000 : -1,
          .mask = EPOLLOUT
          });

  struct io_waiter * w = cclist_peek(node);

  epoll_queue(&cc->e, w);

  while ( sent < (ssize_t) buf_size ) {

    if ( (size = send(cc->e.so, ptr + sent, buf_size - sent, flags | MSG_NOSIGNAL | MSG_DONTWAIT)) > 0 ) {

      sent += size;

      if ( cc->sndtmo >= 0 ) {
        w->tmo = gettime_ms() + cc->sndtmo * 1000;
      }

    }
    else if ( errno == EAGAIN ) {

      co_call(current_core->main);

      if ( !(w->revents & EPOLLOUT) ) {
        PDBG("WARNING: w->events=0x%0X so=%d buf_size=%zu size=%zd sent=%zd errno=%s", w->revents, cc->e.so, buf_size, size, sent, strerror(errno));
        break;
      }

    }
    else {
      sent = size;
      break;
    }
  }

  epoll_dequeue(&cc->e, w);
  remove_waiter(current_core, node);

  return sent;
}

ssize_t cosocket_recv(struct cosocket * cc, void * buf, size_t buf_size, int flags)
{
  ssize_t size;
  struct io_waiter * w;

  struct cclist_node * node =
      add_waiter(current_core, &(struct io_waiter ) {
        .co = co_current(),
        .tmo = cc->rcvtmo >= 0 ? gettime_ms() + cc->rcvtmo * 1000: -1,
        .mask = EPOLLIN
      });


  epoll_queue(&cc->e, w = cclist_peek(node));

  while ( (size = recv(cc->e.so, buf, buf_size, flags | MSG_DONTWAIT | MSG_NOSIGNAL)) < 0 && errno == EAGAIN ) {
    if ( w->tmo != -1 && gettime_ms() >= w->tmo ) {
      break;
    }
    co_call(current_core->main);
  }

  epoll_dequeue(&cc->e, cclist_peek(node));
  remove_waiter(current_core, node);

  return size;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////




int co_poll(struct pollfd *__fds, nfds_t __nfds, int __timeout_ms)
{
  struct {
    struct iorq e;
    struct cclist_node * node;
  } c[__nfds];

  int64_t tmo = __timeout_ms >= 0 ? gettime_ms() + __timeout_ms : -1;
  coroutine_t co = co_current();
  uint32_t event_mask;

  int n = 0;


  for ( nfds_t i = 0; i < __nfds; ++i ) {

    event_mask = ((__fds[i].events & POLLIN) ? EPOLLIN : 0) | ((__fds[i].events & POLLOUT) ? EPOLLOUT : 0);
    __fds[i].revents = 0;

    c[i].e.so = __fds[i].fd;
    c[i].e.type = iowait_io;
    c[i].e.w = cclist_peek(c[i].node =
        add_waiter(current_core, &(struct io_waiter ) {
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

  co_call(current_core->main);

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

    remove_waiter(current_core, c[i].node);
  }

  return n;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool co_io_wait(int so, uint32_t events, int tmo)
{
  struct cclist_node * node;

  struct iorq e = {
    .so = so,
    .type = iowait_io,
    .w = cclist_peek(node =
        add_waiter(current_core, &(struct io_waiter ) {
          .co = co_current(),
          .tmo = tmo,
          .mask = events,
        })),
  };

  if ( !epoll_add(&e, events) ) {
    PDBG("emgr_add(so=%d) fails: %s", so, strerror(errno));
    PBT();
    raise(SIGINT);// for gdb
    exit(1);
  }


  co_call(current_core->main);

  epoll_remove(so);
  remove_waiter(current_core, node);

  return true;
}


ssize_t co_send(int so, const void * buf, size_t buf_size, int flags)
{
  const uint8_t * ptr = buf;
  ssize_t sent = 0;
  ssize_t size;

  while ( sent < (ssize_t) buf_size ) {
    if ( (size = send(so, ptr + sent, buf_size - sent, flags | MSG_DONTWAIT)) > 0 ) {
      sent += size;
    }
    else if ( errno != EAGAIN || !co_io_wait(so, EPOLLOUT, -1) ) {
      break;
    }
  }

  return sent;
}

ssize_t co_recv(int so, void * buf, size_t buf_size, int flags)
{
  ssize_t size;

  while ( (size = recv(so, buf, buf_size, flags | MSG_DONTWAIT)) < 0 && errno == EAGAIN ) {
    if ( !co_io_wait(so, EPOLLIN, -1) ) {
      break;
    }
  }
  return size;
}


ssize_t co_read(int fd, void * buf, size_t buf_size)
{
  ssize_t size;

  while ( (size = read(fd, buf, buf_size)) < 0 && errno == EAGAIN ) {
    if ( !co_io_wait(fd, EPOLLIN, -1) ) {
      break;
    }
  }

  return size;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


bool co_resolve4(const char * name, char addrs[INET_ADDRSTRLEN], time_t timeout)
{
  struct cdns_query * q = NULL;
  struct addrinfo * ent = NULL;
  int so;
  int status;

  if ( timeout > 0 ) {
    timeout = time(NULL) + timeout;
  }

  status = cdns_query_submit(&q, name, &(struct addrinfo ) {
        .ai_family = PF_INET,
        .ai_socktype = SOCK_STREAM,
        .ai_flags = AI_V4MAPPED
      });

  if ( status ) {
    goto end;
  }

  errno = 0;

  while ( 42 ) {

    if ( (status = cdns_query_fetch(q, &ent)) == 0 ) {
      inet_ntop(AF_INET, &((struct sockaddr_in*) ent->ai_addr)->sin_addr.s_addr, addrs, INET_ADDRSTRLEN);
      free(ent);
      break;
    }

    if ( status == EAGAIN ) {
      if ( timeout > 0 && time(NULL) > timeout ) {
        errno = ETIMEDOUT;
        break;
      }

      if ( (so = cdns_query_pollfd(q)) == -1 ) {
        PDBG("cdns_query_pollfd() returns invalid pollfd. errno=%s", strerror(errno));
        break;
      }

      co_io_wait(so, EPOLLIN, 2000);
      continue;
    }

    PDBG("Can not resolve '%s': status=%d (%s)", name, status, cdns_strerror(status));
    break;
  }

end:;

  cdns_query_destroy(&q);

  return status == 0;
}


char * co_resolve_url_4(const char * url, time_t timeout)
{
  char proto[64];
  char auth[128];
  char host[512];
  char path[1024];
  uint8_t a, b, c, d;
  int port = 0;

  char * out = NULL;

  parse_url(url, proto, sizeof(proto), auth, sizeof(auth), host, sizeof(host), &port, path, sizeof(path));

  if ( !*host || sscanf(host, "%hhu:%hhu:%hhu:%hhu", &a, &b, &c, &d) == 4 ) {
    out = strdup(url);
  }
  else if ( co_resolve4(host, host, timeout) ) {
    out = make_url(proto, auth, host, port, path);
  }

  return out;
}
