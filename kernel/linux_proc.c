//
// XSL: process system calls for Linux programs:
// exit, getpid and the small calls glibc makes at startup.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "linux.h"

extern struct spinlock wait_lock; // proc.c, protects p->parent

// exit(status) and exit_group(status). they are the same for us,
// because there are no threads.
uint64
linux_exit(void)
{
  int n;

  argint(0, &n);
  kexit(n & 0xff); // Linux keeps only the low 8 bits of the status
  return 0;        // not reached
}

uint64
linux_getpid(void)
{
  return myproc()->pid;
}

// glibc tells the kernel where its thread id is kept, so it can be
// cleared when a thread exits. no threads here: just return the id.
uint64
linux_set_tid_address(void)
{
  return myproc()->pid;
}

// list of locks to release if a thread dies. no threads here.
uint64
linux_set_robust_list(void)
{
  return 0;
}
