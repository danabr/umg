// Spawns a thread that does some work, then restarts it by resetting
// its stack and registers to their initial value.
#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include "stdlib.h"     // malloc
#include "stdio.h"      // printf
#include "error.h"      // error
#include "errno.h"      // errno
#include "sys/ptrace.h" // ptrace
#include "sched.h"      // clone
#include "elf.h"        // NT_PRSTATUS
#include "sys/user.h"   // sleep
#include "string.h"     // memcpy
#include "mqueue.h"     // iovec
#include "sys/msg.h"    // msgsnd, msgrcv
#include "sys/wait.h"   // SIGCHLD

#define MQ 0            // Create with ipcmk -Q
#define STACK_SIZE (1024*512)

#define MSG_SIZE sizeof(pid_t)

#define MSG_PID 1
#define MSG_SNAPSHOT_REQ 2
#define MSG_SNAPSHOT_RSP 3
#define MSG_REPLACE_STACK_REQ 4
#define MSG_REPLACE_STACK_RSP 5

struct pid_msg {
  long mtype;
  pid_t pid;
};

// Worker thread.
static int count_cats(void *data) {
  printf("worker: Counting cats!\n");
  for(int i = 0; i < 1000; i++) {
    printf("worker: Miauuh %d\n", i);
    sleep(1);
  }
  return 0;
}

// Coordinator thread. Starts the worker thread and helps with snapshotting the stack.
static int coordinate(void* arg) {
  printf("coordinator: Coordinator started\n");

  char* worker_stack = malloc(STACK_SIZE);
  if(worker_stack == NULL) {
    error(1, errno, "Failed to allocate memory for worker thread");
  }
  printf("coordinator: Worker stack allocated\n");

  printf("coordinator: Starting worker thread\n");
  pid_t worker_pid = clone(count_cats, worker_stack + STACK_SIZE, CLONE_SIGHAND | CLONE_VM | CLONE_THREAD | CLONE_PTRACE, 0);
  if(worker_pid == -1) {
    error(1, errno, "Failed to start orker thread");
  }
  printf("coordinator: Worker thread started\n");

  struct pid_msg msg;
  msg.mtype = MSG_PID;
  msg.pid = worker_pid;
  if(msgsnd(MQ, &msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to put worker pid on mq");
  }

  printf("coordinator: Waiting on snapshot request from parent\n");
  if(msgrcv(MQ, &msg, MSG_SIZE, MSG_SNAPSHOT_REQ, 0) == -1) {
    error(1, errno, "Failed to read snapshot request from parent");
  }
  printf("coordinator: Snapshotting worker thread\n");

  char* snapshot_stack = malloc(STACK_SIZE);
  if(snapshot_stack == NULL) {
    error(1, errno, "Failed to allocate memory for snapshot stack");
  }
  memcpy(snapshot_stack, worker_stack, STACK_SIZE);

  msg.mtype = MSG_SNAPSHOT_RSP;
  if(msgsnd(MQ, &msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to inform parent that stack had been copied");
  }

  if(msgrcv(MQ, &msg, MSG_SIZE, MSG_REPLACE_STACK_REQ, 0) == -1) {
    error(1, errno, "Failed to read replace request from parent");
  }

  printf("coordinator: Replacing stack\n");
  memcpy(worker_stack, snapshot_stack, STACK_SIZE);

  msg.mtype = MSG_REPLACE_STACK_RSP;
  if(msgsnd(MQ, &msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to inform parent that stack has been replaced");
  }

  sleep(10);
}

int main(int argc, char** argv) {
  pid_t coordinator_pid;
  char* coordinator_stack = malloc(STACK_SIZE);
  if(coordinator_stack == NULL) {
    error(1, errno, "Failed to allocate memory for coordinator thread");
  }
  char* coordinator_stack_top = coordinator_stack + STACK_SIZE;

  coordinator_pid = clone(coordinate, coordinator_stack_top, SIGCHLD, 0);
  printf("scheduler: Started coordinator thread %ld at %ld\n", (long) coordinator_pid, (long) coordinator_stack_top);
  if(coordinator_pid == -1) {
    error(1, errno, "Failed to start coordinator thread");
  }
  if(ptrace(PTRACE_SEIZE, coordinator_pid, 0, 0) != 0) {
    error(1, errno, "Failed to attach ptrace to coordinator thread");
  }
  printf("scheduler: ptrace of coordinator thread started\n");

  printf("scheduler: Awaiting worker pid\n");
  struct pid_msg msg;
  if(msgrcv(MQ, &msg, MSG_SIZE, MSG_PID, 0) == -1) {
    error(1, errno, "Failed to read worker PID from MQ");
  }
  printf("scheduler: Received worker pid with pid %d\n", (int)msg.pid);

  struct user_regs_struct worker_regs;
  struct iovec worker_vec = {&worker_regs, sizeof(worker_regs)};
  if(ptrace(PTRACE_GETREGSET, msg.pid, NT_PRSTATUS, &worker_vec) != 0) {
    error(1, errno, "GETREGSET for worker thread failed");
  }

  // Ask coordinator to take a snapshot
  printf("scheduler: Asking coordinator to take stack snapshot\n");
  struct pid_msg snapshot_msg;
  snapshot_msg.mtype = MSG_SNAPSHOT_REQ;
  snapshot_msg.pid = msg.pid;
  if(msgsnd(MQ, &snapshot_msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to put snapshot msg on mq");
  }
  

  printf("scheduler: Awaiting coordinator response\n");
  if(msgrcv(MQ, &snapshot_msg, MSG_SIZE, MSG_SNAPSHOT_RSP, 0) == -1) {
    error(1, errno, "Failed to read snapshot response from qm: %d", errno);
  }
  printf("scheduler: Worker thread snapshotted, resuming\n");
 
  if(ptrace(PTRACE_CONT, msg.pid, 0, 0) != 0) {
    error(1, errno, "Failed to resume worker thread");
  }

  sleep(5);

  printf("scheduler: Stopping worker thread to restart it\n");
  if(ptrace(PTRACE_INTERRUPT, msg.pid, 0, 0) != 0) {
    error(1, errno, "Failed to stop sheep thread");
  }

  sleep(1);
  snapshot_msg.mtype = MSG_REPLACE_STACK_REQ;
  if(msgsnd(MQ, &snapshot_msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to ask coordinator to replace stack");
  }
  

  if(msgrcv(MQ, &snapshot_msg, MSG_SIZE, MSG_REPLACE_STACK_RSP, 0) == -1) {
    error(1, errno, "Failed to read coordinator response (replace stack)");
  }

  if(ptrace(PTRACE_SETREGSET, msg.pid, NT_PRSTATUS, &worker_vec) != 0) {
    error(1, errno, "Failed to reset worker registers");
  }

  if(ptrace(PTRACE_CONT, msg.pid, 0, 0) != 0) {
    error(1, errno, "Failed to restart worker thread");
  }
  printf("scheduler: Restarted worker thread\n");

  sleep(10);

  return 0;
}
