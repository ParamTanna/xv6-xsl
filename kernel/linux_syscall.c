//
// XSL: the Linux system call table.
//
// a Linux program makes system calls the same way an xv6 program
// does: number in a7, arguments in a0..a5, ecall, result in a0.
// but the numbers mean different things (write is 64 on Linux and
// 16 on xv6), and on failure Linux returns -errno instead of -1.
// so syscall() sends ABI_LINUX processes here, and we look the
// number up in a second table that uses the Linux numbers.
//
// where the handlers are:
//   linux_proc.c     processes (clone, execve, wait4), ids, signals
//   this file        system information (uname, prlimit64, getrandom)
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "linux.h"

// linux_proc.c
extern uint64 linux_exit(void);
extern uint64 linux_getpid(void);
extern uint64 linux_set_tid_address(void);
extern uint64 linux_set_robust_list(void);

// prlimit64(pid, resource, new, old): read resource limits.
// xv6 has no limits, so we say "unlimited", except for the
// stack, which really is LINUX_STACK_PAGES pages.
static uint64
linux_prlimit64(void)
{
  int pid, resource;
  uint64 old;
  struct linux_rlimit r;
  struct proc *p = myproc();

  argint(0, &pid);
  argint(1, &resource);
  argaddr(3, &old);
  if (pid != 0 && pid != p->pid)
    return -ESRCH;

  r.cur = r.max = ~0UL; // RLIM_INFINITY
  if (resource == RLIMIT_STACK)
    r.cur = r.max = LINUX_STACK_PAGES * PGSIZE;
  if (old != 0 && copyout(p->pagetable, p->sz, old, (char *)&r, sizeof(r)) < 0)
    return -EFAULT;
  return 0; // a new limit (argument 2) is ignored
}

// pseudo random numbers for AT_RANDOM and getrandom().
// xorshift, seeded from the cpu cycle counter. good enough for
// glibc's hash seeds and stack canary on a teaching OS, but not
// cryptographically secure.
static uint64 seed;

uint64
linux_random(void)
{
  if (seed == 0)
    seed = r_time() | 1;
  seed ^= seed << 13;
  seed ^= seed >> 7;
  seed ^= seed << 17;
  return seed;
}

// getrandom(buf, len, flags)
static uint64
linux_getrandom(void)
{
  uint64 buf, len, i, r, n;
  struct proc *p = myproc();

  argaddr(0, &buf);
  argaddr(1, &len);
  for (i = 0; i < len; i += n) {
    r = linux_random();
    n = len - i < 8 ? len - i : 8;
    if (copyout(p->pagetable, p->sz, buf + i, (char *)&r, n) < 0)
      return -EFAULT;
  }
  return len;
}

// uname(buf): describe the system.
static uint64
linux_uname(void)
{
  uint64 addr;
  struct linux_utsname u;
  struct proc *p = myproc();

  argaddr(0, &addr);
  memset(&u, 0, sizeof(u));
  safestrcpy(u.sysname, "xv6", sizeof(u.sysname));
  safestrcpy(u.nodename, "xv6", sizeof(u.nodename));
  safestrcpy(u.release, "xsl-1.0", sizeof(u.release));
  safestrcpy(u.version, "xv6-riscv with the XSL Linux layer", sizeof(u.version));
  safestrcpy(u.machine, "riscv64", sizeof(u.machine));
  if (copyout(p->pagetable, p->sz, addr, (char *)&u, sizeof(u)) < 0)
    return -EFAULT;
  return 0;
}

// the table, indexed by Linux system call number.
struct linux_syscall {
  char *name;
  uint64 (*fn)(void);
};

static struct linux_syscall linux_syscalls[] = {
  // clang-format off
  // processes, ids, signals (linux_proc.c)
  [LSYS_exit]            = {"exit",            linux_exit},
  [LSYS_exit_group]      = {"exit_group",      linux_exit},
  [LSYS_set_tid_address] = {"set_tid_address", linux_set_tid_address},
  [LSYS_set_robust_list] = {"set_robust_list", linux_set_robust_list},
  [LSYS_getpid]          = {"getpid",          linux_getpid},
  // system information (this file)
  [LSYS_uname]           = {"uname",           linux_uname},
  [LSYS_prlimit64]       = {"prlimit64",       linux_prlimit64},
  [LSYS_getrandom]       = {"getrandom",       linux_getrandom},
  // clang-format on
};

// called by syscall() for every system call of a Linux process.
void
linux_syscall(void)
{
  struct proc *p = myproc();
  int num = p->trapframe->a7;
  uint64 ret;

  if (num < 0 || num >= NELEM(linux_syscalls) || linux_syscalls[num].fn == 0) {
    // not implemented: say so, so missing pieces are easy to find
    printk("xsl: %s (pid %d) used Linux system call %d, not implemented\n",
           p->name, p->pid, num);
    p->trapframe->a0 = -ENOSYS;
    return;
  }


  ret = linux_syscalls[num].fn();

  p->trapframe->a0 = ret;
}
