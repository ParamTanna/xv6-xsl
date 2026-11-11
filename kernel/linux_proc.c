//
// XSL: process system calls for Linux programs:
// clone (fork), execve, wait4, exit, process ids, and basic signals.
//
// these are what a shell needs to run "ls | wc -l": the shell makes a
// pipe, clones itself twice, each child points stdin or stdout at the
// pipe (dup3) and execs the program, and the shell waits with wait4.
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

// clone(flags, stack, parent_tid, tls, child_tid).
// glibc's fork() is clone(CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID |
// SIGCHLD, 0, 0, 0, &tid): a new process with a copy of the memory,
// which is exactly xv6's fork. sharing the memory (CLONE_VM, which is
// how threads are made) is not supported.
//   CLONE_CHILD_SETTID: write the child's pid at child_tid in the child
//   CLONE_PARENT_SETTID: write it at parent_tid in the parent
//   CLONE_CHILD_CLEARTID: only matters for threads, ignored
//   low byte (SIGCHLD): the signal for the parent at exit, ignored
uint64
linux_clone(void)
{
  uint64 flags, stack, ptid, ctid;
  int pid;
  struct proc *p = myproc();

  argaddr(0, &flags);
  argaddr(1, &stack);
  argaddr(2, &ptid);
  argaddr(4, &ctid);
  if ((flags & CLONE_VM) || stack != 0)
    return -EINVAL; // a thread, or a child on its own stack

  pid = kfork((flags & CLONE_CHILD_SETTID) ? ctid : 0);
  if (pid < 0)
    return -EAGAIN; // process table full or out of memory
  if ((flags & CLONE_PARENT_SETTID) && ptid != 0)
    copyout(p->pagetable, p->sz, ptid, (char *)&pid, sizeof(pid));
  // the child returns 0: kfork set its a0 to 0
  return pid;
}

// copy a user array of strings (argv or envp, ending with a 0
// pointer) into kernel pages, the way sys_exec() does for argv.
// out[] must have MAXARG+1 slots and start all 0.
static int
fetchstrings(uint64 uarr, char **out)
{
  uint64 uptr;
  int i;

  for (i = 0; i < MAXARG; i++) {
    if (fetchaddr(uarr + i * sizeof(uint64), &uptr) < 0)
      return -EFAULT;
    if (uptr == 0)
      return 0;
    if ((out[i] = kalloc()) == 0)
      return -ENOMEM;
    if (fetchstr(uptr, out[i], PGSIZE) < 0)
      return -EFAULT;
  }
  return -E2BIG; // too many strings
}

static void
freestrings(char **a)
{
  int i;

  for (i = 0; i <= MAXARG && a[i]; i++)
    kfree(a[i]);
}

// execve(path, argv, envp). unlike xv6's exec it also passes the
// environment. kexec() can start Linux and xv6 programs alike, so a
// Linux shell can run xv6's own programs too.
uint64
linux_execve(void)
{
  char path[MAXPATH], *argv[MAXARG + 1], *envp[MAXARG + 1];
  uint64 uargv, uenvp;
  struct inode *ip;
  int r;

  argaddr(1, &uargv);
  argaddr(2, &uenvp);
  if (argstr(0, path, MAXPATH) < 0)
    return -EFAULT;

  // check first that the file is there: then a failure of kexec means
  // "not a program" (ENOEXEC), and a missing file is ENOENT, which
  // tells the shell to try the next directory in PATH.
  begin_op();
  ip = namei(path);
  if (ip)
    iput(ip);
  end_op();
  if (ip == 0)
    return -ENOENT;

  memset(argv, 0, sizeof(argv));
  memset(envp, 0, sizeof(envp));
  r = fetchstrings(uargv, argv);
  if (r == 0 && uenvp != 0) // envp may be NULL, that means empty
    r = fetchstrings(uenvp, envp);
  if (r == 0) {
    // 0 for a Linux program, argc for an xv6 program (its main wants
    // argc in a0), -1 if it isn't a program we can run
    r = kexec(path, argv, envp);
    if (r < 0)
      r = -ENOEXEC;
  }
  freestrings(argv);
  freestrings(envp);
  return r;
}

// wait4(pid, status, options, rusage): the work is in kwait4()
// in proc.c, next to xv6's kwait().
uint64
linux_wait4(void)
{
  int pid, options, r;
  uint64 status, rusage;
  static char zeros[144]; // struct rusage
  struct proc *p = myproc();

  argint(0, &pid);
  argaddr(1, &status);
  argint(2, &options);
  argaddr(3, &rusage);

  // pid 0 and pid < -1 mean "a child in a process group". there are
  // no process groups here, so they mean any child, like -1.
  if (pid == 0 || pid < -1)
    pid = -1;
  r = kwait4(pid, status, options);
  // we don't count resource usage, report all zero
  if (r > 0 && rusage != 0)
    copyout(p->pagetable, p->sz, rusage, zeros, sizeof(zeros));
  return r;
}

uint64
linux_getpid(void)
{
  return myproc()->pid;
}

// with no threads, the thread id is the process id
uint64
linux_gettid(void)
{
  return myproc()->pid;
}

uint64
linux_getppid(void)
{
  int ppid = 0;
  struct proc *p = myproc();

  acquire(&wait_lock);
  if (p->parent)
    ppid = p->parent->pid;
  release(&wait_lock);
  return ppid;
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

// xv6 has no users: everyone is root (uid 0, gid 0), and setuid()/
// setgid() (BusyBox calls them to drop privileges) can only "succeed".
uint64
linux_getuid(void)
{
  return 0;
}

uint64
linux_setuid(void)
{
  return 0;
}

// no process groups or sessions either: every process is alone in
// its own group, whose id is its pid.
uint64
linux_getpgid(void)
{
  return myproc()->pid;
}

uint64
linux_setpgid(void)
{
  return 0;
}

// no permissions in xv6, so the file creation mask is just reported
uint64
linux_umask(void)
{
  return 022;
}

uint64
linux_sched_yield(void)
{
  yield();
  return 0;
}

// prctl(option, arg): BusyBox uses it to read and set the process
// name, which is p->name (what ctrl-p shows).
uint64
linux_prctl(void)
{
  int option;
  uint64 arg;
  char name[16];
  struct proc *p = myproc();

  argint(0, &option);
  argaddr(1, &arg);
  if (option == PR_SET_NAME) {
    if (fetchstr(arg, name, sizeof(name)) < 0)
      return -EFAULT;
    safestrcpy(p->name, name, sizeof(p->name));
    return 0;
  }
  if (option == PR_GET_NAME) {
    if (copyout(p->pagetable, p->sz, arg, p->name, sizeof(p->name)) < 0)
      return -EFAULT;
    return 0;
  }
  return -EINVAL;
}

//
// basic signals. the kernel can't run a program's signal handlers
// (that would need a signal frame on the user stack and rt_sigreturn),
// so a signal does its default action: most signals kill the process,
// a few (SIGCHLD, SIGCONT, SIGURG, SIGWINCH) are ignored.
// a parent's wait4() then reports which signal killed the child.
//
static uint64
sendsig(int pid, int sig)
{
  if (sig < 0 || sig > 64)
    return -EINVAL;
  if (pid <= 0)
    return -ESRCH; // no process groups, no "all processes"
  if (sig == SIGCHLD || sig == SIGCONT || sig == SIGURG || sig == SIGWINCH)
    sig = 0; // ignored by default: only check that pid exists
  if (kkill(pid, sig) < 0)
    return -ESRCH;
  return 0;
}

// kill(pid, sig)
uint64
linux_kill(void)
{
  int pid, sig;

  argint(0, &pid);
  argint(1, &sig);
  return sendsig(pid, sig);
}

// tkill(tid, sig): thread id = process id here
uint64
linux_tkill(void)
{
  int tid, sig;

  argint(0, &tid);
  argint(1, &sig);
  return sendsig(tid, sig);
}

// tgkill(tgid, tid, sig): glibc's raise() and abort() use it
uint64
linux_tgkill(void)
{
  int tid, sig;

  argint(1, &tid);
  argint(2, &sig);
  return sendsig(tid, sig);
}

// rt_sigaction(sig, act, oldact, size): install a signal handler.
// the shell and Lua install handlers (for ctrl-c, SIGCHLD). since
// handlers are never run here, we accept the call and report the
// previous action as "default" (an all-zero struct sigaction).
uint64
linux_rt_sigaction(void)
{
  int sig;
  uint64 act, oldact;
  char zero[24]; // struct sigaction on riscv64: handler, flags, mask
  struct proc *p = myproc();

  argint(0, &sig);
  argaddr(1, &act);
  argaddr(2, &oldact);
  if (sig < 1 || sig > 64)
    return -EINVAL;
  if ((sig == SIGKILL || sig == SIGSTOP) && act != 0)
    return -EINVAL; // these two can never be caught, also on Linux
  memset(zero, 0, sizeof(zero));
  if (oldact != 0 && copyout(p->pagetable, p->sz, oldact, zero, sizeof(zero)) < 0)
    return -EFAULT;
  return 0;
}

// rt_sigprocmask(how, set, oldset, size): block or unblock signals.
// nothing is ever delivered, so there is nothing to block; the old
// mask is "nothing blocked".
uint64
linux_rt_sigprocmask(void)
{
  uint64 oldset, zero = 0;
  struct proc *p = myproc();

  argaddr(2, &oldset);
  if (oldset != 0 && copyout(p->pagetable, p->sz, oldset, (char *)&zero, sizeof(zero)) < 0)
    return -EFAULT;
  return 0;
}
