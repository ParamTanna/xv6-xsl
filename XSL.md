# XSL: a Linux subsystem for xv6

XSL lets the xv6 kernel (RISC-V) run **unmodified Linux programs**: programs
compiled on Ubuntu with the normal Linux cross compiler
(`riscv64-linux-gnu-gcc -static`) run on xv6 without being changed or
recompiled. This includes the **Lua 5.4 interpreter** and the **BusyBox**
shell and tools, with pipes and redirection. It is the same idea as WSL1
(Linux programs on the Windows kernel) and FreeBSD's Linux layer.

Operating Systems course project, IIT Gandhinagar. Param Tanna (24110236).

## How it works

Linux and xv6 programs enter the kernel the same way on RISC-V: `ecall`,
system call number in `a7`, arguments in `a0` to `a5`, result in `a0`.
What differs is the ABI: the numbers (`write` is 64 on Linux, 16 on xv6),
errors (`-errno` instead of `-1`), flag bits, struct layouts, the memory
layout and the initial stack. XSL adds a **Linux personality** to xv6:

| Part | Where |
|---|---|
| `exec` recognises a Linux binary by its GNU ABI note and loads it the Linux way: segments at their link address, 64 page stack, `argc/argv/envp/auxv` | `kernel/linux_exec.c`, `kernel/exec.c` |
| each process has `p->abi`; `syscall()` sends Linux processes to a second table indexed by Linux numbers | `kernel/syscall.c`, `kernel/linux_syscall.c` |
| memory: `brk`, `mmap`, `munmap`, `mprotect` (pages allocated eagerly, holes fault) | `kernel/linux_mm.c`, `kernel/vm.c` |
| files, directories, pipes, fds: `openat`, `read`, `write`, `lseek`, `stat`, `getdents64`, `getcwd`, `pipe2`, `dup3`, `fcntl`, ... | `kernel/sysfile.c` (end of file) |
| processes: `clone`, `execve`, `wait4`, ids, basic signals | `kernel/linux_proc.c`, `kernel/proc.c` |
| time: `clock_gettime` (cycle counter, Goldfish RTC, per process cpu time), `nanosleep` | `kernel/linux_time.c` |
| FPU: xv6 kept it off; glibc needs it. FP registers are saved per process in `sched()` | `kernel/fp.S`, `kernel/trap.c`, `kernel/proc.c` |
| tracing: `strace prog` prints every Linux system call | `user/strace.c`, `kernel/linux_syscall.c` |

61 Linux system calls are in the table. Unimplemented ones print
`xsl: <prog> used Linux system call N, not implemented` and return `-ENOSYS`.

Before the Linux layer, the two OSTEP kernel projects were ported to
xv6-riscv as the starting point, and large files were added:

- `getreadcount()` (initial-xv6)
- unmapped page 0 so NULL dereferences trap, `mprotect()`/`munprotect()` (vm-xv6-intro)
- a doubly indirect inode block (max file about 64 MB, a static Linux binary
  is 0.5 to 2 MB but xv6 files were limited to 268 KB)

## Build and run

Ubuntu 24.04 (or WSL):

```
sudo apt install gcc-riscv64-linux-gnu qemu-system-misc qemu-user
bash linux/ports.sh     # optional: builds Lua and BusyBox (static, riscv64)
make qemu
```

In the xv6 shell:

```
$ readcount             getreadcount()
$ nulltest              NULL dereference is caught
$ protect               mprotect / munprotect, inherited by fork
$ largefile             600 KB file (old limit 268 KB)
$ hello                 a Linux program
$ strace hello          every Linux system call it makes
$ args one two          argc, argv, environment, auxv built by exec
$ files                 Linux file calls and errno values
$ memtest               brk, mmap, mprotect; "memtest crash" segfaults
$ proctest              fork, exec, wait, pipe, dup2, kill, WNOHANG
$ lua demo.lua          the Lua interpreter
$ busybox sh            BusyBox shell: try ls | wc -l, cat README | grep xv6,
                        mkdir d; cd d; pwd, echo hi > f; cat f, date, time sleep 1
$ usertests -q          xv6's own tests still pass
```

Quit QEMU with `Ctrl-A x`.

## Testing method

Each Linux program was first run on Ubuntu under `qemu-riscv64 -strace`,
which shows the system calls it makes on real Linux and their results. The
missing ones were implemented and the trace on xv6 (`strace`) compared with
the Linux one.

## Limits

Static programs only (no `ld.so`), no threads (`CLONE_VM` except vfork), no
signal handlers (signals do their default action), no file-backed `mmap`,
no users or permissions, the console has no raw mode (BusyBox is built with
line editing off, see `linux/ports.sh`).
