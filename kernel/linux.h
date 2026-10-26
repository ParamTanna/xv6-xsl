// XSL: definitions for running Linux programs on xv6.
// the numbers and struct layouts are the ones riscv64 Linux uses
// (include/uapi/asm-generic in the Linux source), because the
// Linux program was compiled against them.

// process personality, kept in proc->abi
#define ABI_XV6   0
#define ABI_LINUX 1

// user address space of a Linux process:
//   0x10000            the program (text, data, bss) where the ELF file says
//   right after it     the heap, grown by brk()
//   LINUX_MMAP_BASE    mmap() regions, handed out upwards
//   LINUX_STACK_TOP    the stack, growing down
// p->sz is LINUX_STACK_TOP, but only the pages in use are mapped.
#define LINUX_MMAP_BASE    0x10000000L
#define LINUX_STACK_TOP    0x40000000L
#define LINUX_STACK_PAGES  64 // 256 KB, allocated at exec

// Linux system call numbers we implement (asm-generic/unistd.h)
// files and directories
#define LSYS_getcwd          17
#define LSYS_dup             23
#define LSYS_dup3            24
#define LSYS_fcntl           25
#define LSYS_ioctl           29
#define LSYS_mkdirat         34
#define LSYS_unlinkat        35
#define LSYS_faccessat       48
#define LSYS_chdir           49
#define LSYS_openat          56
#define LSYS_close           57
#define LSYS_pipe2           59
#define LSYS_getdents64      61
#define LSYS_lseek           62
#define LSYS_sendfile        71
#define LSYS_read            63
#define LSYS_write           64
#define LSYS_readv           65
#define LSYS_writev          66
#define LSYS_readlinkat      78
#define LSYS_newfstatat      79
#define LSYS_fstat           80
#define LSYS_faccessat2      439
// processes
#define LSYS_exit            93
#define LSYS_exit_group      94
#define LSYS_set_tid_address 96
#define LSYS_set_robust_list 99
#define LSYS_sched_yield     124
#define LSYS_kill            129
#define LSYS_tkill           130
#define LSYS_tgkill          131
#define LSYS_rt_sigaction    134
#define LSYS_rt_sigprocmask  135
#define LSYS_setgid          144
#define LSYS_setuid          146
#define LSYS_setpgid         154
#define LSYS_getpgid         155
#define LSYS_getsid          156
#define LSYS_setsid          157
#define LSYS_umask           166
#define LSYS_prctl           167
#define LSYS_getpid          172
#define LSYS_getppid         173
#define LSYS_getuid          174
#define LSYS_geteuid         175
#define LSYS_getgid          176
#define LSYS_getegid         177
#define LSYS_gettid          178
#define LSYS_clone           220
#define LSYS_execve          221
#define LSYS_wait4           260
// time
#define LSYS_nanosleep       101
#define LSYS_clock_gettime   113
#define LSYS_clock_nanosleep 115
// memory
#define LSYS_brk             214
#define LSYS_munmap          215
#define LSYS_mmap            222
#define LSYS_mprotect        226
// system information
#define LSYS_uname           160
#define LSYS_prlimit64       261
#define LSYS_getrandom       278

// errno values. Linux returns -errno on failure, xv6 returns -1.
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EFAULT       14
#define EEXIST       17
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ENOTTY       25
#define ESPIPE       29
#define ERANGE       34
#define ENOSYS       38
#define ENOTEMPTY    39

// openat() flags (octal, as in the Linux headers)
#define L_O_ACCMODE   00000003
#define L_O_WRONLY    00000001
#define L_O_RDWR      00000002
#define L_O_CREAT     00000100
#define L_O_EXCL      00000200
#define L_O_TRUNC     00001000
#define L_O_APPEND    00002000
#define L_O_DIRECTORY 00200000
#define L_O_CLOEXEC   02000000 // close this fd when the process execs

#define AT_FDCWD      -100   // "relative to the current directory"
#define AT_REMOVEDIR  0x200  // unlinkat: remove a directory
#define AT_EMPTY_PATH 0x1000 // newfstatat: stat the fd itself

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

// fcntl commands
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC      1

// getdents64: we don't fill in d_type, so programs stat() the entry
#define DT_UNKNOWN 0

// clone flags (the low byte is the signal to send the parent at exit)
#define CLONE_VM            0x00000100
#define CLONE_VFORK         0x00004000
#define CLONE_PARENT_SETTID 0x00100000
#define CLONE_CHILD_SETTID  0x01000000

// wait4 options
#define WNOHANG 1

// signal numbers we look at
#define SIGILL   4
#define SIGKILL  9
#define SIGSEGV  11
#define SIGSTOP  19
#define SIGCHLD  17
#define SIGCONT  18
#define SIGURG   23
#define SIGWINCH 28

// prctl
#define PR_SET_NAME 15
#define PR_GET_NAME 16

// clocks
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7
#define TIMER_ABSTIME            1

// the cpu's time counter (r_time()) counts at 10 MHz on QEMU's virt
// machine, and xv6's timer interrupt comes every 1000000 counts (0.1 s)
#define TIMER_HZ    10000000
#define TICK_CYCLES 1000000

// file type bits of st_mode
#define L_S_IFIFO 0010000
#define L_S_IFCHR 0020000
#define L_S_IFDIR 0040000
#define L_S_IFREG 0100000

// mmap / mprotect
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define PROT_EXEC     0x4
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

// terminal ioctls: get settings (glibc's isatty() uses it),
// and get the window size (ls uses it to lay out columns)
#define TCGETS     0x5401
#define TIOCGWINSZ 0x5413
#define L_ICANON 0000002 // c_lflag: line at a time
#define L_ECHO   0000010 // c_lflag: echo typed characters

#define RLIMIT_STACK 3

// auxiliary vector entries (the "aux vector" after envp on the stack)
#define AT_NULL   0
#define AT_PHDR   3  // where the program headers are in memory
#define AT_PHENT  4  // size of one program header
#define AT_PHNUM  5  // number of program headers
#define AT_PAGESZ 6
#define AT_ENTRY  9  // program entry point
#define AT_UID    11
#define AT_EUID   12
#define AT_GID    13
#define AT_EGID   14
#define AT_HWCAP  16 // cpu features
#define AT_CLKTCK 17
#define AT_SECURE 23
#define AT_RANDOM 25 // address of 16 random bytes

// AT_HWCAP for rv64gc: one bit per ISA letter (I, M, A, F, D, C)
#define LINUX_HWCAP ((1 << ('I' - 'A')) | (1 << ('M' - 'A')) | \
                     (1 << ('A' - 'A')) | (1 << ('F' - 'A')) | \
                     (1 << ('D' - 'A')) | (1 << ('C' - 'A')))

// ELF things not in elf.h
#define ELF_PROG_INTERP 3 // dynamically linked program
#define ELF_PROG_NOTE   4
#define ELF_ET_EXEC     2
#define ELF_CLASS64     2
#define EM_RISCV        243
#define NT_GNU_ABI_TAG  1
#define ELF_NOTE_OS_LINUX 0

// struct stat as riscv64 Linux lays it out (128 bytes)
struct linux_stat {
  uint64 st_dev;
  uint64 st_ino;
  uint st_mode;
  uint st_nlink;
  uint st_uid;
  uint st_gid;
  uint64 st_rdev;
  uint64 pad1;
  int64 st_size;
  int st_blksize;
  int pad2;
  int64 st_blocks;
  int64 st_atime;
  uint64 st_atime_nsec;
  int64 st_mtime;
  uint64 st_mtime_nsec;
  int64 st_ctime;
  uint64 st_ctime_nsec;
  uint unused4;
  uint unused5;
};

struct linux_utsname {
  char sysname[65];
  char nodename[65];
  char release[65];
  char version[65];
  char machine[65];
  char domainname[65];
};

// the kernel's struct termios (glibc's own one is bigger)
struct linux_termios {
  uint c_iflag;
  uint c_oflag;
  uint c_cflag;
  uint c_lflag;
  uchar c_line;
  uchar c_cc[19];
};

struct linux_rlimit {
  uint64 cur;
  uint64 max;
};

struct linux_winsize {
  ushort ws_row;
  ushort ws_col;
  ushort ws_xpixel;
  ushort ws_ypixel;
};

struct linux_timespec {
  int64 tv_sec;
  int64 tv_nsec;
};

// one buffer for readv/writev
struct linux_iovec {
  uint64 base;
  uint64 len;
};

// header of one entry returned by getdents64; the name follows it,
// with a 0 at the end, and the whole entry is padded to 8 bytes
struct linux_dirent64 {
  uint64 d_ino;
  int64 d_off;    // directory offset of the next entry
  ushort d_reclen; // size of this entry
  uchar d_type;
} __attribute__((packed));
