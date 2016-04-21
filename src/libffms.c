/*
 * libffms.c
 *
 *  Created on: Apr 2, 2016
 *      Author: amyznikov
 */

#include "ffms.h"
#include "libffms.h"
#include "sockopt.h"
#include <limits.h>
#include <pthread.h>
#include "debug.h"
#include <stdlib.h>

enum cotask_id {
  cotask_none = 0,
  cotask_schedule_io = 1,
  cotask_start_cothread = 2,
};

struct cotask {
  int64_t id; // prevent valgrind warning

  union {
    struct {
      int (*callback)(void *, uint32_t);
      void * cookie;
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

};


static struct {
  cclist threads; // <struct pclthread>
  int ncpu;
  bool initialized:1;
} ffms = {
  .initialized = false
};


struct pclthread {
  pthread_t tid;
  int loadrate;
  int pipe[2];
  bool started;
};


static __thread struct pclthread
  * g_current_pclthread = NULL;


static struct pclthread * get_pclthread_min_loadrate(void)
{
  struct pclthread * best = NULL;
  //int minrate = INT_MAX;

  int i, x;


  x = rand() % ffms.ncpu;
  i = 0;

  for ( cclist_node * node = cclist_head(&ffms.threads); node != NULL; node = node->next ) {

    if ( i++ == x ) {

      struct pclthread * temp = cclist_peek(node);
      //if ( temp->loadrate < minrate )
      {
        // minrate = temp->loadrate;
        best = temp;
      }

      break;

    }
  }

  return best;
}


static bool push_task(struct pclthread * thread, const struct cotask * t)
{
  bool fok;
  //PDBG("C write()");
  fok = (write(thread->pipe[1], t, sizeof(*t)) == (ssize_t) sizeof(*t));
  //PDBG("R write(): fok=%d", fok);

  return fok;
}


static int pop_task(void * cookie, uint32_t events)
{
  struct pclthread * thread = cookie;
  struct cotask task;
  ssize_t size;


//  PDBG("enter: pipe[0]=%d events=0x%0X", thread->pipe[0], events);

  if ( events & EPOLLERR ) {
    PDBG("FATAL: EPOLLERR");
    return -1;
  }

  if ( !(events & EPOLLIN) ) {
    PDBG("FATAL: NO EPOLLIN");
    return 0;
  }

  while ( (size = read(thread->pipe[0], &task, sizeof(task))) == (ssize_t) sizeof(task) ) {

    switch ( task.id ) {
      case cotask_none :
        //PDBG("cotask_none");
      break;

      case cotask_schedule_io :
        //PDBG("cotask_schedule_io");
        if ( !co_schedule_io(task.io.callback, task.io.so, task.io.flags, task.io.cookie, task.io.stack_size) ) {
          PDBG("FATAL: co_schedule_io() fails: %s", strerror(errno));
        }
        else {
          ++thread->loadrate;
        }
      break;

      case cotask_start_cothread :
        //PDBG("cotask_start_cothread");
        if ( !co_schedule(task.thread.func, task.thread.arg, task.thread.stack_size) ) {
          PDBG("FATAL: co_schedule_io() fails: %s", strerror(errno));
        }
        else {
          ++thread->loadrate;
        }
      break;

      default :
        PDBG("FATAL :  task.id=%"PRId64"", task.id);
        exit(1);
      break;
    }
  }

  if ( size != -1 || errno != EAGAIN ) {
    PDBG("FATAL: read(task) fails: %s", strerror(errno));
    exit(1);
    return -1;
  }

  //  PDBG("leave");
  return 0;
}




static void * pclthread(void * arg)
{
  struct pclthread * ctx = arg;
  g_current_pclthread = ctx;

  PDBG("started");

  pthread_detach(pthread_self());

  if ( pipe(ctx->pipe) != 0 ) {
    PDBG("FATAL: pipe() fails: %d %s", errno, strerror(errno));
    goto end;
  }

  PDBG("pipe[0] = %d", ctx->pipe[0]);
  PDBG("pipe[1] = %d", ctx->pipe[1]);

  so_set_noblock(ctx->pipe[0], true);
  so_set_noblock(ctx->pipe[1], true);

  if ( !co_scheduler_init() ) {
    PDBG("FATAL: co_scheduler_init() fails: %d %s", errno, strerror(errno));
    goto end;
  }

  ctx->started = true;
  co_schedule_io(pop_task, ctx->pipe[0], EPOLLIN, ctx, 32 * 1024);
  co_scheduler_run();

end: ;
  ctx->started = true;

  co_thread_cleanup();

  PDBG("finished");
  return NULL;
}



bool ffms_init(int ncpu)
{
  if ( ncpu < 1 ) {
    ncpu = 1;
  }

  if ( !cclist_init(&ffms.threads, ncpu, sizeof(struct pclthread)) ) {
    PDBG("cclist_init() fails: %s", strerror(errno));
    goto end;
  }

  ffms.ncpu = ncpu;
  ffinput_initialize();

  for ( int i = 0; i < ncpu; ++i ) {
    struct pclthread * t = cclist_peek(cclist_push_back(&ffms.threads, NULL));
    t->pipe[0] = t->pipe[1] = -1;
    t->loadrate = 0;
    t->started = false;
    if ( (errno = pthread_create(&t->tid, NULL, pclthread, t)) ) {
      PDBG("pthread_create(pclthread) fails: %d %s", errno, strerror(errno));
      goto end;
    }

    while ( !t->started ) {
      usleep(20 * 1000);
    }

  }

  ffms.initialized = true;

end: ;

  return ffms.initialized;
}


bool ffms_add_input(struct ffms_input_params args)
{
  struct ffinput * input = NULL;
  bool fok = false;

  if ( !(input = ff_create_input((struct ff_create_input_args ) { .p = args })) ) {
    goto end;
  }

  fok = true;

end : ;

  if ( !fok && input ) {
    ff_destroy_input(input);
  }

  return fok;
}



bool ffms_schedule_io(int so, int (*callback)(void *, uint32_t), void * cookie, uint32_t flags, size_t stack_size)
{
  struct pclthread * thread;
  bool fok = false;

  if ( !(thread = get_pclthread_min_loadrate()) ) {
    PDBG("get_pclthread_min_loadrate() fails: %s", strerror(errno));
  }
  else {
    fok = push_task(thread, &(struct cotask ) {
          .id = cotask_schedule_io,
          .io.callback = callback,
          .io.cookie = cookie,
          .io.stack_size = stack_size,
          .io.flags = flags,
          .io.so = so
          });

    if ( !fok ) {
      PDBG("push_task() fails: %s", strerror(errno));
    }
  }

  return fok;
}

bool ffms_start_cothread(void (*func)(void*), void * arg, size_t stack_size)
{
  struct pclthread * thread;
  bool fok = false;

  //PDBG("ENTER");

  if ( (thread = get_pclthread_min_loadrate()) ) {

    //PDBG("C push_task()");

    fok = push_task(thread, &(struct cotask ) {
          .id = cotask_start_cothread,
          .thread.func = func,
          .thread.arg = arg,
          .thread.stack_size = stack_size
          });

    //PDBG("R push_task()");
  }

  //PDBG("LEAVE: fok=%d", fok);

  return fok;
}


void ffms_shutdown(void)
{
  return;
}
