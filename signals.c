#define _XOPEN_SOURCE 500

#include "unistd.h"
#include "stdio.h"
#include "error.h"
#include "errno.h"
#include "signal.h"

void suspend(int s) {
  printf("In signal handler for USR1, waiting for USR2\n");
  while (1) {}
 // sigset_t set;
 // sigemptyset(&set);
 // sigaddset(&set, SIGUSR2);
 // int sig;
 // sigwait(&set, &sig);
} 

void resume(int s) {
  printf("Resuming execution after being suspended");
}

int main(int argc, char** argv) {
  printf("My pid is %d\n", getpid());
  struct sigaction action;
  action.sa_handler = suspend;
  action.sa_flags = SA_NODEFER;
  if(sigaction(SIGUSR1, &action, NULL) != 0) {
    error(1, errno, "Failed to install signal handler for suspend");
  }

  struct sigaction resaction;
  resaction.sa_handler = resume;
  if(sigaction(SIGUSR2, &resaction, NULL) != 0)  {
    error(1, errno, "Failed to install signal handler for resume");
  }

  printf("Managed to install signal handler. Counting sheep.\n");
  for(int i = 0; i < 100; i++) {
    printf("Baah %d\n", (i+1));
    sleep(1);
  }
  return 0;
}
