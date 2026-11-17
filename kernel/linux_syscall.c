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
//   sysfile.c        files, directories, pipes, fds
//   linux_proc.c     processes (clone, execve, wait4), ids, signals
//   linux_mm.c       memory (brk, mmap, munmap, mprotect)
//   linux_time.c     clocks and sleeping
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

// sysfile.c
extern uint64 linux_getcwd(void);
extern uint64 linux_dup(void);
extern uint64 linux_dup3(void);
extern uint64 linux_fcntl(void);
extern uint64 linux_ioctl(void);
extern uint64 linux_mkdirat(void);
extern uint64 linux_unlinkat(void);
extern uint64 linux_faccessat(void);
extern uint64 linux_chdir(void);
extern uint64 linux_openat(void);
extern uint64 linux_close(void);
extern uint64 linux_pipe2(void);
extern uint64 linux_getdents64(void);
extern uint64 linux_lseek(void);
extern uint64 linux_read(void);
extern uint64 linux_write(void);
extern uint64 linux_readv(void);
extern uint64 linux_writev(void);
extern uint64 linux_readlinkat(void);
extern uint64 linux_newfstatat(void);
extern uint64 linux_fstat(void);
// linux_proc.c
extern uint64 linux_exit(void);
extern uint64 linux_clone(void);
extern uint64 linux_execve(void);
extern uint64 linux_wait4(void);
extern uint64 linux_getpid(void);
extern uint64 linux_gettid(void);
extern uint64 linux_getppid(void);
extern uint64 linux_set_tid_address(void);
extern uint64 linux_set_robust_list(void);
extern uint64 linux_getuid(void);
extern uint64 linux_setuid(void);
extern uint64 linux_getpgid(void);
extern uint64 linux_setpgid(void);
extern uint64 linux_umask(void);
extern uint64 linux_sched_yield(void);
extern uint64 linux_prctl(void);
extern uint64 linux_kill(void);
extern uint64 linux_tkill(void);
extern uint64 linux_tgkill(void);
extern uint64 linux_rt_sigaction(void);
extern uint64 linux_rt_sigprocmask(void);
// linux_mm.c
extern uint64 linux_brk(void);
extern uint64 linux_munmap(void);
extern uint64 linux_mmap(void);
extern uint64 linux_mprotect(void);
// linux_time.c
extern uint64 linux_nanosleep(void);
extern uint64 linux_clock_gettime(void);
extern uint64 linux_clock_nanosleep(void);

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
  // files, directories, pipes, fds (sysfile.c)
  [LSYS_getcwd]          = {"getcwd",          linux_getcwd},
  [LSYS_dup]             = {"dup",             linux_dup},
  [LSYS_dup3]            = {"dup3",            linux_dup3},
  [LSYS_fcntl]           = {"fcntl",           linux_fcntl},
  [LSYS_ioctl]           = {"ioctl",           linux_ioctl},
  [LSYS_mkdirat]         = {"mkdirat",         linux_mkdirat},
  [LSYS_unlinkat]        = {"unlinkat",        linux_unlinkat},
  [LSYS_faccessat]       = {"faccessat",       linux_faccessat},
  [LSYS_chdir]           = {"chdir",           linux_chdir},
  [LSYS_openat]          = {"openat",          linux_openat},
  [LSYS_close]           = {"close",           linux_close},
  [LSYS_pipe2]           = {"pipe2",           linux_pipe2},
  [LSYS_getdents64]      = {"getdents64",      linux_getdents64},
  [LSYS_lseek]           = {"lseek",           linux_lseek},
  [LSYS_read]            = {"read",            linux_read},
  [LSYS_write]           = {"write",           linux_write},
  [LSYS_readv]           = {"readv",           linux_readv},
  [LSYS_writev]          = {"writev",          linux_writev},
  [LSYS_readlinkat]      = {"readlinkat",      linux_readlinkat},
  [LSYS_newfstatat]      = {"newfstatat",      linux_newfstatat},
  [LSYS_fstat]           = {"fstat",           linux_fstat},
  [LSYS_faccessat2]      = {"faccessat2",      linux_faccessat},
  // processes, ids, signals (linux_proc.c)
  [LSYS_exit]            = {"exit",            linux_exit},
  [LSYS_exit_group]      = {"exit_group",      linux_exit},
  [LSYS_set_tid_address] = {"set_tid_address", linux_set_tid_address},
  [LSYS_set_robust_list] = {"set_robust_list", linux_set_robust_list},
  [LSYS_sched_yield]     = {"sched_yield",     linux_sched_yield},
  [LSYS_kill]            = {"kill",            linux_kill},
  [LSYS_tkill]           = {"tkill",           linux_tkill},
  [LSYS_tgkill]          = {"tgkill",          linux_tgkill},
  [LSYS_rt_sigaction]    = {"rt_sigaction",    linux_rt_sigaction},
  [LSYS_rt_sigprocmask]  = {"rt_sigprocmask",  linux_rt_sigprocmask},
  [LSYS_setgid]          = {"setgid",          linux_setuid},
  [LSYS_setuid]          = {"setuid",          linux_setuid},
  [LSYS_setpgid]         = {"setpgid",         linux_setpgid},
  [LSYS_getpgid]         = {"getpgid",         linux_getpgid},
  [LSYS_getsid]          = {"getsid",          linux_getpgid},
  [LSYS_setsid]          = {"setsid",          linux_getpgid},
  [LSYS_umask]           = {"umask",           linux_umask},
  [LSYS_prctl]           = {"prctl",           linux_prctl},
  [LSYS_getpid]          = {"getpid",          linux_getpid},
  [LSYS_getppid]         = {"getppid",         linux_getppid},
  [LSYS_getuid]          = {"getuid",          linux_getuid},
  [LSYS_geteuid]         = {"geteuid",         linux_getuid},
  [LSYS_getgid]          = {"getgid",          linux_getuid},
  [LSYS_getegid]         = {"getegid",         linux_getuid},
  [LSYS_gettid]          = {"gettid",          linux_gettid},
  [LSYS_clone]           = {"clone",           linux_clone},
  [LSYS_execve]          = {"execve",          linux_execve},
  [LSYS_wait4]           = {"wait4",           linux_wait4},
  // time (linux_time.c)
  [LSYS_nanosleep]       = {"nanosleep",       linux_nanosleep},
  [LSYS_clock_gettime]   = {"clock_gettime",   linux_clock_gettime},
  [LSYS_clock_nanosleep] = {"clock_nanosleep", linux_clock_nanosleep},
  // memory (linux_mm.c)
  [LSYS_brk]             = {"brk",             linux_brk},
  [LSYS_munmap]          = {"munmap",          linux_munmap},
  [LSYS_mmap]            = {"mmap",            linux_mmap},
  [LSYS_mprotect]        = {"mprotect",        linux_mprotect},
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
  uint64 a0 = p->trapframe->a0; // saved for the trace,
  uint64 a1 = p->trapframe->a1; // a0 gets overwritten
  uint64 a2 = p->trapframe->a2;
  uint64 ret;
  char *name;

  if (num < 0 || num >= NELEM(linux_syscalls) || linux_syscalls[num].fn == 0) {
    // not implemented: say so, so missing pieces are easy to find
    printk("xsl: %s (pid %d) used Linux system call %d, not implemented\n",
           p->name, p->pid, num);
    p->trapframe->a0 = -ENOSYS;
    return;
  }

  name = linux_syscalls[num].name;
  if (p->trace && (num == LSYS_exit || num == LSYS_exit_group))
    printk("[%d] %s(%ld)\n", p->pid, name, a0); // print now, never returns

  ret = linux_syscalls[num].fn();

  if (p->trace) {
    // like strace: name, first three arguments, result
    if ((int64)ret >= -4096 && (int64)ret < 4096)
      printk("[%d] %s(0x%lx, 0x%lx, 0x%lx) = %ld\n", p->pid, name, a0, a1, a2, ret);
    else
      printk("[%d] %s(0x%lx, 0x%lx, 0x%lx) = 0x%lx\n", p->pid, name, a0, a1, a2, ret);
  }
  p->trapframe->a0 = ret;
}
