/*
 * daemon.c
 *
 *  Created on: Oct 2, 2015
 *      Author: amyznikov
 */
#include "daemon.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <execinfo.h>

#include "debug.h"



static void my_signal_handler(int signum, siginfo_t *si, void * context)
{
  typedef/* This structure mirrors the one found in /usr/include/asm/ucontext.h */
  struct _sig_ucontext {
    ulong uc_flags;
    struct ucontext * uc_link;
    stack_t uc_stack;
    struct sigcontext uc_mcontext;
    sigset_t uc_sigmask;
  } sig_ucontext_t;

  int ignore = 0;
  int status = 0;
  const sig_ucontext_t * uc = (sig_ucontext_t *) context;
  void * caller_address;

#ifdef __arm__
  caller_address = (void *) uc->uc_mcontext.arm_pc;
#else
  caller_address = (void *) uc->uc_mcontext.rip;
#endif

  if ( signum != SIGWINCH ) {
    PDBG("SIGNAL %d (%s)", signum, strsignal(signum));
  }

  switch ( signum ) {
    case SIGINT :
    case SIGQUIT :
    case SIGTERM :
      status = 0;
    break;

    case SIGSEGV :
    case SIGSTKFLT :
    case SIGILL :
    case SIGBUS :
    case SIGSYS :
    case SIGFPE :
    case SIGABRT :
      status = EXIT_FAILURE;
      PDBG("Fault address:%p from %p", si->si_addr, caller_address);
      PBT();
    break;

    default :
      ignore = 1;
    break;
  }

  if ( !ignore ) {
    exit(status);
  }
}


/**
 * setup_signal_handler()
 *    see errno on failure
 */
bool setup_signal_handler(void)
{
  struct sigaction sa;
  int sig;

  memset(&sa, 0, sizeof(sa));

  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sa.sa_sigaction = my_signal_handler;

  for ( sig = 1; sig <= SIGUNUSED; ++sig ) {
    /* skip unblockable signals */
    if ( sig != SIGKILL && sig != SIGSTOP && sig != SIGCONT && sigaction(sig, &sa, NULL) != 0 ) {
      return false;
    }
  }

  return true;
}


/** become_daemon()
 *    fork() and become daemon
 */
pid_t become_daemon(void)
{
  pid_t pid;

  if ( (pid = fork()) == 0 ) {

    /* In child process close all files
     * */
    int fd, fdmax;

    if ( (fdmax = sysconf(_SC_OPEN_MAX)) < 0 ) {
      fdmax = 1024;
    }

    for ( fd = fdmax - 1; fd >= 0; --fd ) {
      close(fd);
    }

    // umask(0);

    /* Redirect stdin/stdout/stderr to /dev/null
     * */
    for ( fd = 0; fd < 3; ++fd ) {
      open("/dev/null", O_RDWR);
    }
  }

  return pid;
}

