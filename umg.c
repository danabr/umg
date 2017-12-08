#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include "stdlib.h"
#include "unistd.h"
#include "stdio.h"
#include "error.h"
#include "errno.h"
#include "signal.h"
#include "sys/ptrace.h"
#include "sys/types.h"
#include "sched.h"
#include "elf.h"
#include "sys/uio.h"
#include "sys/user.h"
#include "string.h"


// Start thread
// Ask to "supervise" the other thread
// See what happens

#define STACK_SIZE (1024*2)

long unsigned int fac(int n) {
  if(n <= 1) { return 1; }
  return n * fac(n-1);
}

void * get_pc () { return __builtin_return_address(0); }

static int count_sheep(void* data) {
  printf("THREAD: count_sheep can be found at %p\n", get_pc());
  for(int i = 0; i < 1000; i++) {
    char* ptr = malloc(i); 
    printf("Baah %d\n", i);
    sleep(1);
    printf("Buh %lu\n", fac(i + 1));
    free(ptr);
  }
  return 0;
}

static int count_cats(void *data) {
  for(int i = 0; i < 1000; i++) {
    printf("Miauuh %d\n", i);
    sleep(1);
  }
  return 0;
}

int main(int argc, char** argv) {
  pid_t thread_pid;
  pid_t parent_pid = getpid();
  char* child_stack = malloc(STACK_SIZE);
  if(child_stack == NULL) {
    error(1, errno, "Failed to allocate memory for child thread");
  }
  memset(child_stack, 0, STACK_SIZE);
  char* stack_top = child_stack + STACK_SIZE;
  int arg = 1000;

  printf("size is %lu\n", sizeof(unsigned long long int));

  void * addr = count_sheep;
  printf("count_sheep can be found at %p\n", addr);

  thread_pid = clone(count_sheep, stack_top, CLONE_SIGHAND | CLONE_VM | CLONE_THREAD, 0); 
  printf("Started thread %ld at %ld\n", (long) thread_pid, (long) stack_top);
  if(thread_pid == -1) {
    error(1, errno, "Failed to start thread");
  }
  sleep(5);
  if(ptrace(PTRACE_ATTACH, thread_pid, 0, 0) != 0) {
    error(1, errno, "ptrace failed");
  }

  int i = 0;
  for(; i < 100; i++) {
    sleep(1);
    printf("Disabling\n");
    ptrace(PTRACE_INTERRUPT, thread_pid, 0, 0);
    sleep(1);
   
    struct user_regs_struct regs;
    struct iovec vec = {&regs, 1024};

    errno = 0;
    unsigned long long int* rip;
    if(ptrace(PTRACE_GETREGSET, thread_pid, NT_PRSTATUS, &vec) != 0) {
      error(1, errno, "GETREGSET failed");
    }
    printf("Read %u bytes of registers\n", (unsigned) vec.iov_len);
    printf("r15: %llu\n", regs.r15);
    printf("r14: %llu\n", regs.r14);
    printf("r13: %llu\n", regs.r13);
    printf("r12: %llu\n", regs.r12);
    printf("rbp: %llu\n", regs.rbp);
    printf("rbx: %llu\n", regs.rbx);
    printf("r11: %llu\n", regs.r11);
    printf("r10: %llu\n", regs.r10);
    printf("r09: %llu\n", regs.r9);
    printf("r08: %llu\n", regs.r8);
    printf("rax: %llu\n", regs.rax);
    printf("rcx: %llu\n", regs.rdx);
    printf("rdx: %llu\n", regs.rcx);
    printf("rsi: %llu\n", regs.rsi);
    printf("rdi: %llu\n", regs.rdi);
    printf("orig_rax: %llu\n", regs.orig_rax);
    printf("rip: %llu\n", regs.rip);
    printf("cs: %llu\n", regs.cs);
    printf("eflags: %llu\n", regs.eflags);
    printf("rsp: %llu\n", regs.rsp);
    printf("ss: %llu\n", regs.ss);
    printf("fs_base: %llu\n", regs.fs_base);
    printf("gs_base: %llu\n", regs.gs_base);
    printf("ds: %llu\n", regs.ds);
    printf("es: %llu\n", regs.es);
    printf("fs: %llu\n", regs.fs);
    printf("gs: %llu\n", regs.gs);

    printf("memory: ");
    int x = 0;
    for(; x < 512; x++) {
      printf("%d ", child_stack[STACK_SIZE - x - 1]);
    }
    printf("\n");
    
  
    if(ptrace(PTRACE_SETREGSET, thread_pid, NT_PRSTATUS, &vec) != 0) {
      error(1, errno, "SETREGSET failed");
    }

    sleep(1);
    printf("Restarting\n");
    ptrace(PTRACE_CONT, thread_pid, 0, 0); 
  }
  
  sleep(10);

  return 0;
}
