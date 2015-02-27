// Spawns a thread that waits for work.
// Runs two different "worker" functions on that thread.
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

#define STACK_SIZE (1024*1024)

#define MSG_SIZE sizeof(struct message) - sizeof(long)

#define MSG_PID 1
#define MSG_SNAPSHOT_REQ 2
#define MSG_SNAPSHOT_RSP 3
#define MSG_START_WORK_REQ 6
#define MSG_CONTEXT_SWITCH_RSP 9
#define MSG_COORDINATOR_REQ 10

#define MSG_COORDINATOR_REQ_CONTEXT_SWITCH 1
#define MSG_COORDINATOR_REQ_SPAWN 2
#define MSG_COORDINATOR_REQ_EXIT 3

int mq;

typedef struct message {
  long mtype;
  char subtype;
  union {
    pid_t pid;
    struct user_regs_struct regs;
    int (*worker_fun)();
  };
} message;


typedef struct worker {
  char* stack;
  struct user_regs_struct regs;
  struct worker* next;
} worker;

typedef struct worker_thread {
  pid_t pid;
  char* starting_stack;
  char* working_stack;
  struct user_regs_struct starting_regs;
  worker* run_q_head;
  worker* run_q_tail;
} worker_thread;

// Context thread
// * stop thread
// * save regs
// * save stack
// * replace regs
// * replace stack
// * resume thread

void scheduler_context_switch(pid_t worker_pid) {
  message msg;
  struct iovec worker_vec = {&msg.regs, sizeof(msg.regs)};

  if(ptrace(PTRACE_INTERRUPT, worker_pid, 0, 0) != 0) {
    error(1, errno, "scheduler_context_switch: Failed to stop worker thread");
  }

  sleep(1);

  if(ptrace(PTRACE_GETREGSET, worker_pid, NT_PRSTATUS, &worker_vec) != 0) {
    error(1, errno, "scheduler_context_switch: GETREGSET for worker thread failed");
  }

  msg.mtype = MSG_COORDINATOR_REQ;
  msg.subtype = MSG_COORDINATOR_REQ_CONTEXT_SWITCH;
  if(msgsnd(mq, &msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "scheduler_context_switch: Failed to send ctcx switch request");
  }

  if(msgrcv(mq, &msg, MSG_SIZE, MSG_CONTEXT_SWITCH_RSP, 0) == -1) {
    error(1, errno, "scheduler_context_switch: Failed to read ctxt switch response");
  }

  if(ptrace(PTRACE_SETREGSET, worker_pid, NT_PRSTATUS, &worker_vec) != 0) {
    error(1, errno, "scheduler_context_switch: Failed to reset worker registers");
  }

  if(ptrace(PTRACE_CONT, worker_pid, 0, 0) != 0) {
    error(1, errno, "scheduler_context_switch: Failed to resume worker thread");
  }
}

void coordinator_context_switch(struct worker_thread* w, message msg) {
  if(w->run_q_head->next != NULL) {
    // Save stack
    memcpy(w->run_q_head->stack, w->working_stack, STACK_SIZE);

    // Save regs
    w->run_q_head->regs = msg.regs;

    w->run_q_head = w->run_q_head->next;
    memcpy(w->working_stack, w->run_q_head->stack, STACK_SIZE);

    msg.mtype = MSG_CONTEXT_SWITCH_RSP;
    msg.regs = w->run_q_head->regs;
    if(msgsnd(mq, &msg, MSG_SIZE, 0) == -1) {
      error(1, errno, "cordinator: Failed to perform context switch");
    }
  } else {
    error(1, 0, "coordinator: Nothing to context switch too!");
  }
}

// Worker thread.
static int run_worker_thread(void *data) {
  struct message msg;
  if(msgrcv(mq, &msg, MSG_SIZE, MSG_START_WORK_REQ, 0) == -1) {
    error(1, errno, "worker: Failed waiting on worker function");
  }

  return msg.worker_fun();
}

static void spawn(int (*fun)()) {
  message msg;
  msg.worker_fun = fun;
  msg.mtype = MSG_COORDINATOR_REQ;
  msg.subtype = MSG_COORDINATOR_REQ_SPAWN;
  if(msgsnd(mq, &msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to put worker pid on mq");
  }
}

static int count_dogs(void *data) {
  printf("worker: Counting dogs!\n");
  for(int i = 0; i < 1000; i++) {
    printf("worker: Bark %d\n", i);
    usleep(500000);
  }
  return 0;
}

static int count_cats(void *data) {
  printf("worker: Counting cats!\n");
  for(int i = 0; i < 1000; i++) {
    if(i > 1 && (i % 7) == 0) {
      spawn(count_dogs);
    }
    printf("worker: Miauuh %d\n", i);
    sleep(1);
  }
  return 0;
}

// Coordinator thread. Starts the worker thread and helps with snapshotting the stack.
static int coordinate(void* arg) {
  worker_thread worker_thread;
  worker cat_worker;

  worker_thread.starting_stack = malloc(STACK_SIZE);
  if(worker_thread.starting_stack == NULL) {
    error(1, errno, "Failed to allocate memory for worker thread");
  }
  worker_thread.working_stack = malloc(STACK_SIZE);
  if(worker_thread.working_stack == NULL) {
    error(1, errno, "Failed to allocate memory for worker thread");
  }
  cat_worker.stack = malloc(STACK_SIZE);
  if(cat_worker.stack == NULL) {
    error(1, errno, "Failed to allocate memory for cat worker");
  }

  worker_thread.pid = clone(run_worker_thread, worker_thread.working_stack + STACK_SIZE, CLONE_SIGHAND | CLONE_VM | CLONE_THREAD | CLONE_PTRACE, 0);
  if(worker_thread.pid == -1) {
    error(1, errno, "Failed to start worker thread");
  }

  message msg;
  msg.mtype = MSG_PID;
  msg.pid = worker_thread.pid;
  if(msgsnd(mq, &msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to put worker pid on mq");
  }

  if(msgrcv(mq, &msg, MSG_SIZE, MSG_SNAPSHOT_REQ, 0) == -1) {
    error(1, errno, "Failed to read snapshot request from parent");
  }

  memcpy(worker_thread.starting_stack, worker_thread.working_stack, STACK_SIZE);
  worker_thread.starting_regs = msg.regs;

  msg.mtype = MSG_SNAPSHOT_RSP;
  if(msgsnd(mq, &msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to inform parent that stack had been copied");
  }

  worker_thread.run_q_head = &cat_worker;
  worker_thread.run_q_tail = &cat_worker;
  cat_worker.next = &cat_worker;

  msg.mtype = MSG_START_WORK_REQ;
  msg.worker_fun = count_cats;
  if(msgsnd(mq, &msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to send start work message");
  }

  for(;;) {
    if(msgrcv(mq, &msg, MSG_SIZE, MSG_COORDINATOR_REQ, 0) == -1) {
      error(1, errno, "coordinator: Failed to receive message");
    }
    if(msg.subtype == MSG_COORDINATOR_REQ_CONTEXT_SWITCH) {
      printf("coordinator: Context switching\n");
      coordinator_context_switch(&worker_thread, msg);
    } else if(msg.subtype == MSG_COORDINATOR_REQ_SPAWN) {
      printf("coordinator: Spawning new worker\n");
      worker* worker = malloc(sizeof(struct worker));
      if(worker == NULL) {
        error(1, errno, "Failed to allocate memory for worker");
      }
      worker->stack = malloc(STACK_SIZE);
      if(worker->stack == NULL) {
        error(1, errno, "Failed to allocate stack memory for worker");
      }

      worker->next = worker_thread.run_q_head;
      memcpy(worker->stack, worker_thread.starting_stack, STACK_SIZE);
      worker->regs = worker_thread.starting_regs;

      worker_thread.run_q_tail->next = worker;
      worker_thread.run_q_tail = worker;

      msg.mtype = MSG_START_WORK_REQ;
      // msg.worker_fun = msg.worker_fun;
      if(msgsnd(mq, &msg, MSG_SIZE, 0) == -1) {
        error(1, errno, "Failed to send start worker req");
      }
    } else if(msg.subtype == MSG_COORDINATOR_REQ_EXIT) {
      printf("coordinator: Exiting as planned");
      return 0;
    }
  }
}

int main(int argc, char** argv) {
  mq = msgget(IPC_PRIVATE, 0600);
  if(mq == -1) {
    error(1, errno, "Failed to set up mq");
  }

  pid_t coordinator_pid;
  char* coordinator_stack = malloc(STACK_SIZE);
  if(coordinator_stack == NULL) {
    error(1, errno, "Failed to allocate memory for coordinator thread");
  }
  char* coordinator_stack_top = coordinator_stack + STACK_SIZE;

  coordinator_pid = clone(coordinate, coordinator_stack_top, SIGCHLD, 0);
  if(coordinator_pid == -1) {
    error(1, errno, "Failed to start coordinator thread");
  }
  if(ptrace(PTRACE_SEIZE, coordinator_pid, 0, 0) != 0) {
    error(1, errno, "Failed to attach ptrace to coordinator thread");
  }

  message msg;
  if(msgrcv(mq, &msg, MSG_SIZE, MSG_PID, 0) == -1) {
    error(1, errno, "Failed to read worker PID from mq");
  }

  struct user_regs_struct worker_regs;
  struct iovec worker_vec = {&worker_regs, sizeof(worker_regs)};
  if(ptrace(PTRACE_GETREGSET, msg.pid, NT_PRSTATUS, &worker_vec) != 0) {
    error(1, errno, "GETREGSET for worker thread failed");
  }

  // Ask coordinator to take a snapshot
  message snapshot_msg;
  snapshot_msg.mtype = MSG_SNAPSHOT_REQ;
  snapshot_msg.regs = worker_regs;
  if(msgsnd(mq, &snapshot_msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to put snapshot msg on mq");
  }


  if(msgrcv(mq, &snapshot_msg, MSG_SIZE, MSG_SNAPSHOT_RSP, 0) == -1) {
    error(1, errno, "Failed to read snapshot response from qm: %d", errno);
  }

  if(ptrace(PTRACE_CONT, msg.pid, 0, 0) != 0) {
    error(1, errno, "Failed to resume worker thread");
  }

  printf("scheduler: Entering context switching mode.\n");
  for(int i= 0; i < 20; i++) {
    sleep(2);
    scheduler_context_switch(msg.pid);
  }

  snapshot_msg.mtype = MSG_COORDINATOR_REQ;
  snapshot_msg.subtype = MSG_COORDINATOR_REQ_EXIT;
  if(msgsnd(mq, &snapshot_msg, MSG_SIZE, 0) == -1) {
    error(1, errno, "Failed to send exit msg to coordinator");
  }

  printf("scheduler: Waiting for coordinator to exit\n");
  waitpid(coordinator_pid, NULL, WEXITED);

  if(msgctl(mq, IPC_RMID, NULL) == -1) {
    error(1, errno, "Failed to remove mq");
  }

  return 0;
}
