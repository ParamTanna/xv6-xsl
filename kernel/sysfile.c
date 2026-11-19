//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "linux.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  argint(n, &fd);
  if (fd < 0 || fd >= NOFILE || (f = myproc()->ofile[fd]) == 0)
    return -1;
  if (pfd)
    *pfd = fd;
  if (pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *p = myproc();

  for (fd = 0; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      p->cloexec[fd] = 0; // XSL: a new fd is never close-on-exec
      return fd;
    }
  }
  return -1;
}

uint64
sys_dup(void)
{
  struct file *f;
  int fd;

  if (argfd(0, 0, &f) < 0)
    return -1;
  if ((fd = fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

// how many times read() has been called since boot.
// updated with an atomic add since many cpus can be in read() at once.
uint64 readcount;

uint64
sys_getreadcount(void)
{
  return readcount;
}

uint64
sys_read(void)
{
  struct file *f;
  int n;
  uint64 p;

  __sync_fetch_and_add(&readcount, 1);
  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return fileread(f, p, n);
}

uint64
sys_write(void)
{
  struct file *f;
  int n;
  uint64 p;

  argaddr(1, &p);
  argint(2, &n);
  if (argfd(0, 0, &f) < 0)
    return -1;

  return filewrite(f, p, n);
}

uint64
sys_close(void)
{
  int fd;
  struct file *f;

  if (argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

uint64
sys_fstat(void)
{
  struct file *f;
  uint64 st; // user pointer to struct stat

  argaddr(1, &st);
  if (argfd(0, 0, &f) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
uint64
sys_link(void)
{
  char name[DIRSIZ], new[MAXPATH], old[MAXPATH];
  struct inode *dp, *ip;

  if (argstr(0, old, MAXPATH) < 0 || argstr(1, new, MAXPATH) < 0)
    return -1;

  begin_op();
  if ((ip = namei(old)) == 0) {
    end_op();
    return -1;
  }

  ilock(ip);
  if (ip->type == T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->nlink >= NLINK_MAX) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if ((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  // dp may have been unlinked while we resolved it; linking into an
  // orphaned directory leaks ip (itrunc discards the record without
  // dropping ip->nlink).  create() has the same guard.
  if (dp->nlink == 0) {
    iunlockput(dp);
    goto bad;
  }
  if (dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0) {
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de)) {
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
      return 0;
  }
  return 1;
}

// remove the directory entry path. shared by xv6's unlink() and
// Linux's unlinkat(), so it returns a Linux error code (-ENOENT ...).
// isdir: 1 = path must be a directory (Linux rmdir),
//        0 = path must not be a directory (Linux unlink),
//       -1 = either (xv6 unlink).
static int
unlinkpath(char *path, int isdir)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ];
  uint off;
  int err = -ENOENT;

  begin_op();
  if ((dp = nameiparent(path, name)) == 0) {
    end_op();
    return -ENOENT;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if (namecmp(name, ".") == 0 || namecmp(name, "..") == 0) {
    err = -EINVAL;
    goto bad;
  }

  if ((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if (ip->nlink < 1)
    panic("unlink: nlink < 1");
  if (isdir == 1 && ip->type != T_DIR)
    err = -ENOTDIR;
  else if (isdir == 0 && ip->type == T_DIR)
    err = -EISDIR;
  else if (ip->type == T_DIR && !isdirempty(ip))
    err = -ENOTEMPTY;
  else
    err = 0;
  if (err < 0) {
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if (writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if (ip->type == T_DIR) {
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return err;
}

uint64
sys_unlink(void)
{
  char path[MAXPATH];

  if (argstr(0, path, MAXPATH) < 0)
    return -1;
  if (unlinkpath(path, -1) < 0)
    return -1;
  return 0;
}

static struct inode *
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if ((dp = nameiparent(path, name)) == 0)
    return 0;

  ilock(dp);

  if (dp->nlink == 0) {
    iunlockput(dp);
    return 0;
  }

  // a new directory's ".." would push dp->nlink past its maximum
  if (type == T_DIR && dp->nlink >= NLINK_MAX) {
    iunlockput(dp);
    return 0;
  }

  if ((ip = dirlookup(dp, name, 0)) != 0) {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && (ip->type == T_FILE || ip->type == T_DEVICE))
      return ip;
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0) {
    iunlockput(dp);
    return 0;
  }

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR) { // Create . and .. entries.
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      goto fail;
  }

  if (dirlink(dp, name, ip->inum) < 0)
    goto fail;

  if (type == T_DIR) {
    // now that success is guaranteed:
    dp->nlink++; // for ".."
    iupdate(dp);
  }

  iunlockput(dp);

  return ip;

fail:
  // something went wrong. de-allocate ip.
  ip->nlink = 0;
  iupdate(ip);
  iunlockput(ip);
  iunlockput(dp);
  return 0;
}

uint64
sys_open(void)
{
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  int n;

  argint(1, &omode);
  if ((n = argstr(0, path, MAXPATH)) < 0)
    return -1;

  begin_op();

  if (omode & O_CREATE) {
    ip = create(path, T_FILE, 0, 0);
    if (ip == 0) {
      end_op();
      return -1;
    }
  } else {
    if ((ip = namei(path)) == 0) {
      end_op();
      return -1;
    }
    ilock(ip);
    if (ip->type == T_DIR && omode != O_RDONLY) {
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -1;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);

  if ((omode & O_TRUNC) && ip->type == T_FILE) {
    itrunc(ip);
  }

  iunlock(ip);
  end_op();

  return fd;
}

uint64
sys_mkdir(void)
{
  char path[MAXPATH];
  struct inode *ip;

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_mknod(void)
{
  struct inode *ip;
  char path[MAXPATH];
  int major, minor;

  begin_op();
  argint(1, &major);
  argint(2, &minor);
  if ((argstr(0, path, MAXPATH)) < 0 ||
      (ip = create(path, T_DEVICE, major, minor)) == 0) {
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

uint64
sys_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  begin_op();
  if (argstr(0, path, MAXPATH) < 0 || (ip = namei(path)) == 0) {
    end_op();
    return -1;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

uint64
sys_exec(void)
{
  char path[MAXPATH], *argv[MAXARG];
  int i;
  uint64 uargv, uarg;

  argaddr(1, &uargv);
  if (argstr(0, path, MAXPATH) < 0) {
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for (i = 0;; i++) {
    if (i >= NELEM(argv)) {
      goto bad;
    }
    if (fetchaddr(uargv + sizeof(uint64) * i, (uint64 *)&uarg) < 0) {
      goto bad;
    }
    if (uarg == 0) {
      argv[i] = 0;
      break;
    }
    argv[i] = kalloc();
    if (argv[i] == 0)
      goto bad;
    if (fetchstr(uarg, argv[i], PGSIZE) < 0)
      goto bad;
  }

  int ret = kexec(path, argv, 0);

  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);

  return ret;

bad:
  for (i = 0; i < NELEM(argv) && argv[i] != 0; i++)
    kfree(argv[i]);
  return -1;
}

uint64
sys_pipe(void)
{
  uint64 fdarray; // user pointer to array of two integers
  struct file *rf, *wf;
  int fd0, fd1;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  if (pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
    if (fd0 >= 0)
      p->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  if (copyout(p->pagetable, p->sz, fdarray, (char *)&fd0, sizeof(fd0)) < 0 ||
      copyout(p->pagetable, p->sz, fdarray + sizeof(fd0), (char *)&fd1,
              sizeof(fd1)) < 0) {
    p->ofile[fd0] = 0;
    p->ofile[fd1] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  return 0;
}

//
// XSL: file system calls for Linux programs.
// they work on the same xv6 files, inodes and file descriptors as
// the calls above, but take Linux arguments and flag values, and
// return -errno on failure instead of -1.
//

// xv6 returns -1 for any failure. turn that into a Linux -e.
static uint64
lerr(uint64 r, int e)
{
  if ((int64)r < 0)
    return -e;
  return r;
}

// read, write and close take the same arguments on Linux and xv6,
// so after checking the fd we can call the xv6 versions.
uint64
linux_read(void)
{
  struct file *f;

  if (argfd(0, 0, &f) < 0 || f->readable == 0)
    return -EBADF;
  return lerr(sys_read(), EIO);
}

uint64
linux_write(void)
{
  struct file *f;

  if (argfd(0, 0, &f) < 0 || f->writable == 0)
    return -EBADF;
  return lerr(sys_write(), EIO);
}

uint64
linux_close(void)
{
  return lerr(sys_close(), EBADF);
}

// openat(dirfd, path, flags, mode). like sys_open(), but Linux
// uses different bits for the same flags (O_CREAT is 0x40 on Linux,
// O_CREATE is 0x200 on xv6) and wants a precise error code.
uint64
linux_openat(void)
{
  char path[MAXPATH];
  int dirfd, flags, acc, fd;
  struct file *f;
  struct inode *ip;

  argint(0, &dirfd);
  argint(2, &flags);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  // only paths relative to the current directory, or absolute ones
  if (dirfd != AT_FDCWD && path[0] != '/')
    return -EINVAL;
  acc = flags & L_O_ACCMODE;

  begin_op();
  if (flags & L_O_CREAT) {
    // O_EXCL: fail if the file is already there
    if ((flags & L_O_EXCL) && (ip = namei(path)) != 0) {
      iput(ip);
      end_op();
      return -EEXIST;
    }
    if ((ip = create(path, T_FILE, 0, 0)) == 0) {
      end_op();
      return -ENOENT; // no such directory, or it is a directory
    }
  } else {
    if ((ip = namei(path)) == 0) {
      end_op();
      return -ENOENT;
    }
    ilock(ip);
  }

  if (ip->type == T_DIR && acc != 0) {
    iunlockput(ip);
    end_op();
    return -EISDIR; // directories can only be opened for reading
  }
  if ((flags & L_O_DIRECTORY) && ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -ENOTDIR;
  }
  if (ip->type == T_DEVICE && (ip->major < 0 || ip->major >= NDEV)) {
    iunlockput(ip);
    end_op();
    return -EIO;
  }

  if ((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0) {
    if (f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return f ? -EMFILE : -ENFILE;
  }

  if (ip->type == T_DEVICE) {
    f->type = FD_DEVICE;
    f->major = ip->major;
  } else {
    f->type = FD_INODE;
    f->off = 0;
  }
  f->ip = ip;
  f->readable = (acc != L_O_WRONLY);
  f->writable = (acc == L_O_WRONLY || acc == L_O_RDWR);
  f->append = (flags & L_O_APPEND) != 0;
  myproc()->cloexec[fd] = (flags & L_O_CLOEXEC) != 0;

  if ((flags & L_O_TRUNC) && ip->type == T_FILE && f->writable)
    itrunc(ip);

  iunlock(ip);
  end_op();
  return fd;
}

// lseek(fd, offset, whence). xv6 has no lseek, but each open file
// already has an offset (f->off) that read and write use.
uint64
linux_lseek(void)
{
  struct file *f;
  int64 off, base;
  int whence;

  argaddr(1, (uint64 *)&off);
  argint(2, &whence);
  if (argfd(0, 0, &f) < 0)
    return -EBADF;
  if (f->type != FD_INODE)
    return -ESPIPE; // the console and pipes can't seek

  if (whence == SEEK_SET) {
    base = 0;
  } else if (whence == SEEK_CUR) {
    base = f->off;
  } else if (whence == SEEK_END) {
    ilock(f->ip);
    base = f->ip->size;
    iunlock(f->ip);
  } else {
    return -EINVAL;
  }
  if (base + off < 0 || base + off > MAXFILE * BSIZE)
    return -EINVAL;
  f->off = base + off;
  return f->off;
}

// fill a Linux struct stat from an xv6 inode. caller holds ip->lock.
static void
linux_stati(struct inode *ip, struct linux_stat *st)
{
  memset(st, 0, sizeof(*st));
  st->st_dev = ip->dev;
  st->st_ino = ip->inum;
  st->st_nlink = ip->nlink;
  st->st_size = ip->size;
  st->st_blksize = BSIZE;
  st->st_blocks = (ip->size + 511) / 512; // in 512-byte units
  // xv6 has no permissions, so everything is readable and writable
  if (ip->type == T_DIR) {
    st->st_mode = L_S_IFDIR | 0755;
  } else if (ip->type == T_DEVICE) {
    st->st_mode = L_S_IFCHR | 0666;
    st->st_rdev = (ip->major << 8) | ip->minor;
  } else {
    st->st_mode = L_S_IFREG | 0755;
  }
}

// copy the Linux stat of open file f to user address addr.
static uint64
linux_fdstat(struct file *f, uint64 addr)
{
  struct linux_stat st;
  struct proc *p = myproc();

  if (f->type == FD_PIPE) {
    memset(&st, 0, sizeof(st));
    st.st_mode = L_S_IFIFO | 0600;
  } else {
    ilock(f->ip);
    linux_stati(f->ip, &st);
    iunlock(f->ip);
  }
  if (copyout(p->pagetable, p->sz, addr, (char *)&st, sizeof(st)) < 0)
    return -EFAULT;
  return 0;
}

// fstat(fd, statbuf)
uint64
linux_fstat(void)
{
  struct file *f;
  uint64 addr;

  argaddr(1, &addr);
  if (argfd(0, 0, &f) < 0)
    return -EBADF;
  return linux_fdstat(f, addr);
}

// newfstatat(dirfd, path, statbuf, flags): stat by name.
// glibc's stat() uses this.
uint64
linux_newfstatat(void)
{
  char path[MAXPATH];
  int dirfd, flags;
  uint64 addr;
  struct file *f;
  struct inode *ip;
  struct linux_stat st;
  struct proc *p = myproc();

  argint(0, &dirfd);
  argaddr(2, &addr);
  argint(3, &flags);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;

  // AT_EMPTY_PATH with path "": stat the file dirfd itself
  if (path[0] == 0 && (flags & AT_EMPTY_PATH)) {
    if (argfd(0, 0, &f) < 0)
      return -EBADF;
    return linux_fdstat(f, addr);
  }
  if (dirfd != AT_FDCWD && path[0] != '/')
    return -EINVAL;

  begin_op();
  if ((ip = namei(path)) == 0) {
    end_op();
    return -ENOENT;
  }
  ilock(ip);
  linux_stati(ip, &st);
  iunlockput(ip);
  end_op();

  if (copyout(p->pagetable, p->sz, addr, (char *)&st, sizeof(st)) < 0)
    return -EFAULT;
  return 0;
}

// unlinkat(dirfd, path, flags): unlink, or rmdir if AT_REMOVEDIR.
uint64
linux_unlinkat(void)
{
  char path[MAXPATH];
  int dirfd, flags;

  argint(0, &dirfd);
  argint(2, &flags);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  if (dirfd != AT_FDCWD && path[0] != '/')
    return -EINVAL;
  return unlinkpath(path, (flags & AT_REMOVEDIR) ? 1 : 0);
}

// ioctl(fd, request, arg). only two requests, and only on the console:
//   TCGETS: glibc's isatty() asks for the terminal settings. if that
//     works, stdout is a terminal and printf flushes after every line,
//     otherwise it waits for a full buffer.
//   TIOCGWINSZ: the window size; ls uses it to lay out its columns.
// for any other file the answer is "not a terminal".
uint64
linux_ioctl(void)
{
  struct file *f;
  uint64 req, arg;
  struct linux_termios t;
  struct linux_winsize w;
  struct proc *p = myproc();

  argaddr(1, &req);
  argaddr(2, &arg);
  if (argfd(0, 0, &f) < 0)
    return -EBADF;
  if (f->type != FD_DEVICE || f->major != CONSOLE)
    return -ENOTTY;

  if (req == TCGETS) {
    // xv6's console works a line at a time and echoes what you type
    memset(&t, 0, sizeof(t));
    t.c_lflag = L_ICANON | L_ECHO;
    if (copyout(p->pagetable, p->sz, arg, (char *)&t, sizeof(t)) < 0)
      return -EFAULT;
    return 0;
  }
  if (req == TIOCGWINSZ) {
    // QEMU's serial console doesn't know the real window, use the
    // classic terminal size
    memset(&w, 0, sizeof(w));
    w.ws_row = 24;
    w.ws_col = 80;
    if (copyout(p->pagetable, p->sz, arg, (char *)&w, sizeof(w)) < 0)
      return -EFAULT;
    return 0;
  }
  return -ENOTTY;
}

// readlinkat: xv6 has no symbolic links (and no /proc, which is what
// glibc asks about at startup), so there is never a link to read.
uint64
linux_readlinkat(void)
{
  return -ENOENT;
}

// sendfile(out_fd, in_fd, offset, count) copies between two files
// inside the kernel. xv6's read and write paths only copy to and from
// user memory, so we don't offer it; BusyBox's cat then falls back to
// plain read() and write(), which is what EINVAL tells it to do.
uint64
linux_sendfile(void)
{
  return -EINVAL;
}

//
// XSL: file descriptor calls, used by the shell for pipes and
// redirection (ls | wc, echo hi > file).
//

// put f in the lowest free fd that is >= min (for F_DUPFD).
static int
fdalloc_from(struct file *f, int min)
{
  int fd;
  struct proc *p = myproc();

  for (fd = min; fd < NOFILE; fd++) {
    if (p->ofile[fd] == 0) {
      p->ofile[fd] = f;
      p->cloexec[fd] = 0;
      return fd;
    }
  }
  return -1;
}

// dup(fd): same arguments as xv6's dup.
uint64
linux_dup(void)
{
  struct file *f;

  if (argfd(0, 0, &f) < 0)
    return -EBADF;
  return lerr(sys_dup(), EMFILE);
}

// dup3(oldfd, newfd, flags): make newfd refer to the same open file
// as oldfd, closing whatever newfd was before. glibc's dup2() uses it.
// this is how the shell points stdout (fd 1) at a pipe or a file.
uint64
linux_dup3(void)
{
  struct file *f;
  int oldfd, newfd, flags;
  struct proc *p = myproc();

  argint(1, &newfd);
  argint(2, &flags);
  if (argfd(0, &oldfd, &f) < 0)
    return -EBADF;
  if (newfd < 0 || newfd >= NOFILE)
    return -EBADF;
  if (newfd == oldfd || (flags & ~L_O_CLOEXEC))
    return -EINVAL;

  if (p->ofile[newfd])
    fileclose(p->ofile[newfd]);
  p->ofile[newfd] = filedup(f);
  p->cloexec[newfd] = (flags & L_O_CLOEXEC) != 0;
  return newfd;
}

// fcntl(fd, cmd, arg)
uint64
linux_fcntl(void)
{
  struct file *f;
  int fd, cmd, arg, newfd, fl;
  struct proc *p = myproc();

  argint(1, &cmd);
  argint(2, &arg);
  if (argfd(0, &fd, &f) < 0)
    return -EBADF;

  switch (cmd) {
  case F_DUPFD:         // copy to the lowest free fd >= arg
  case F_DUPFD_CLOEXEC: // the shell keeps its own copy of fds this way
    if (arg < 0 || arg >= NOFILE)
      return -EINVAL;
    if ((newfd = fdalloc_from(f, arg)) < 0)
      return -EMFILE;
    filedup(f);
    p->cloexec[newfd] = (cmd == F_DUPFD_CLOEXEC);
    return newfd;
  case F_GETFD: // fd flags: only close-on-exec exists
    return p->cloexec[fd] ? FD_CLOEXEC : 0;
  case F_SETFD:
    p->cloexec[fd] = (arg & FD_CLOEXEC) != 0;
    return 0;
  case F_GETFL: // open file flags: access mode and append
    if (f->readable && f->writable)
      fl = L_O_RDWR;
    else if (f->writable)
      fl = L_O_WRONLY;
    else
      fl = 0;
    if (f->append)
      fl |= L_O_APPEND;
    return fl;
  case F_SETFL: // only O_APPEND can be changed here
    f->append = (arg & L_O_APPEND) != 0;
    return 0;
  }
  return -EINVAL;
}

// pipe2(fds, flags): xv6's pipe plus the O_CLOEXEC flag.
uint64
linux_pipe2(void)
{
  uint64 fdarray;
  int flags, fd[2];
  struct file *rf, *wf;
  struct proc *p = myproc();

  argaddr(0, &fdarray);
  argint(1, &flags);
  if (flags & ~L_O_CLOEXEC)
    return -EINVAL; // O_NONBLOCK and O_DIRECT are not supported
  if (pipealloc(&rf, &wf) < 0)
    return -ENFILE;
  fd[0] = -1;
  if ((fd[0] = fdalloc(rf)) < 0 || (fd[1] = fdalloc(wf)) < 0) {
    if (fd[0] >= 0)
      p->ofile[fd[0]] = 0;
    fileclose(rf);
    fileclose(wf);
    return -EMFILE;
  }
  if (copyout(p->pagetable, p->sz, fdarray, (char *)fd, sizeof(fd)) < 0) {
    p->ofile[fd[0]] = 0;
    p->ofile[fd[1]] = 0;
    fileclose(rf);
    fileclose(wf);
    return -EFAULT;
  }
  p->cloexec[fd[0]] = p->cloexec[fd[1]] = (flags & L_O_CLOEXEC) != 0;
  return 0;
}

// readv/writev(fd, iov, iovcnt): read or write a list of buffers,
// one after the other, as if they were one big buffer.
static uint64
linux_rwv(int write)
{
  struct file *f;
  uint64 iov;
  struct linux_iovec v;
  int iovcnt, i, n, total = 0;
  struct proc *p = myproc();

  argaddr(1, &iov);
  argint(2, &iovcnt);
  if (argfd(0, 0, &f) < 0 || (write ? !f->writable : !f->readable))
    return -EBADF;
  if (iovcnt < 0 || iovcnt > 1024)
    return -EINVAL;

  for (i = 0; i < iovcnt; i++) {
    if (copyin(p->pagetable, p->sz, (char *)&v, iov + i * sizeof(v), sizeof(v)) < 0)
      return -EFAULT;
    if (v.len == 0)
      continue;
    n = write ? filewrite(f, v.base, v.len) : fileread(f, v.base, v.len);
    if (n < 0)
      return total > 0 ? total : -EIO;
    total += n;
    if (n < v.len) // short read (end of file) or write: stop here
      break;
  }
  return total;
}

uint64
linux_readv(void)
{
  return linux_rwv(0);
}

uint64
linux_writev(void)
{
  return linux_rwv(1);
}

//
// XSL: directories.
//

// getdents64(fd, buf, count): read directory entries.
// an xv6 directory is a file of 16-byte struct dirent (inum, name).
// Linux wants a list of variable sized struct linux_dirent64 records,
// so we convert them one by one. f->off remembers where we are, so
// the next call continues after the last entry we returned.
uint64
linux_getdents64(void)
{
  struct file *f;
  struct dirent de;
  uint64 buf;
  int count, n, len, reclen;
  char rec[48]; // a record: 19-byte header + name (max 14) + 0, padded
  struct linux_dirent64 *d = (struct linux_dirent64 *)rec;
  struct proc *p = myproc();

  argaddr(1, &buf);
  argint(2, &count);
  if (argfd(0, 0, &f) < 0)
    return -EBADF;
  if (f->type != FD_INODE)
    return -ENOTDIR;

  ilock(f->ip);
  if (f->ip->type != T_DIR) {
    iunlock(f->ip);
    return -ENOTDIR;
  }
  n = 0;
  while (f->off + sizeof(de) <= f->ip->size) {
    if (readi(f->ip, 0, (uint64)&de, f->off, sizeof(de)) != sizeof(de))
      break;
    if (de.inum == 0) { // unused slot (a deleted file)
      f->off += sizeof(de);
      continue;
    }
    // xv6 names use all 14 bytes with no 0 at the end
    for (len = 0; len < DIRSIZ && de.name[len]; len++)
      ;
    reclen = (sizeof(struct linux_dirent64) + len + 1 + 7) & ~7;
    if (n + reclen > count)
      break; // buffer full, the program will call again

    memset(rec, 0, sizeof(rec));
    d->d_ino = de.inum;
    d->d_off = f->off + sizeof(de); // where the next entry starts
    d->d_reclen = reclen;
    d->d_type = DT_UNKNOWN;
    memmove(rec + sizeof(struct linux_dirent64), de.name, len);
    if (copyout(p->pagetable, p->sz, buf + n, rec, reclen) < 0) {
      iunlock(f->ip);
      return -EFAULT;
    }
    n += reclen;
    f->off += sizeof(de);
  }
  if (n == 0 && f->off + sizeof(de) <= f->ip->size) {
    iunlock(f->ip);
    return -EINVAL; // buffer too small for even one entry
  }
  iunlock(f->ip);
  return n; // 0 means end of directory
}

// getcwd(buf, size): xv6 keeps the current directory as an inode,
// not as a path, so we rebuild the path by walking up through "..":
// in the parent we look for the entry with our inode number, that is
// our name. repeat until the root. the path is built from the end.
uint64
linux_getcwd(void)
{
  char path[MAXPATH];
  int pos, len, found;
  uint off, inum;
  uint64 buf, size;
  struct dirent de;
  struct inode *ip, *pp;
  struct proc *p = myproc();

  argaddr(0, &buf);
  argaddr(1, &size);

  pos = MAXPATH - 1;
  path[pos] = 0;

  begin_op();
  ip = idup(p->cwd);
  for (;;) {
    ilock(ip);
    if (ip->inum == ROOTINO) {
      iunlockput(ip);
      break;
    }
    inum = ip->inum;
    pp = dirlookup(ip, "..", 0); // the parent
    iunlockput(ip);
    if (pp == 0) {
      end_op();
      return -ENOENT;
    }

    // find our name in the parent
    ilock(pp);
    found = 0;
    for (off = 0; off < pp->size; off += sizeof(de)) {
      if (readi(pp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
        break;
      if (de.inum == inum && namecmp(de.name, ".") != 0 &&
          namecmp(de.name, "..") != 0) {
        found = 1;
        break;
      }
    }
    iunlock(pp);
    if (!found) { // our directory was removed
      iput(pp);
      end_op();
      return -ENOENT;
    }

    // put "/name" in front of what we have
    for (len = 0; len < DIRSIZ && de.name[len]; len++)
      ;
    if (pos - len - 1 < 0) {
      iput(pp);
      end_op();
      return -ERANGE;
    }
    pos -= len;
    memmove(path + pos, de.name, len);
    path[--pos] = '/';
    ip = pp;
  }
  end_op();

  if (path[pos] == 0) // we are at the root
    path[--pos] = '/';
  len = MAXPATH - 1 - pos;
  if (len + 1 > size)
    return -ERANGE;
  if (copyout(p->pagetable, p->sz, buf, path + pos, len + 1) < 0)
    return -EFAULT;
  return len + 1; // the system call returns the length with the 0
}

// chdir(path): like xv6's chdir, with Linux error codes.
uint64
linux_chdir(void)
{
  char path[MAXPATH];
  struct inode *ip;
  struct proc *p = myproc();

  if (argstr(0, path, MAXPATH) < 0)
    return -EFAULT;
  begin_op();
  if ((ip = namei(path)) == 0) {
    end_op();
    return -ENOENT;
  }
  ilock(ip);
  if (ip->type != T_DIR) {
    iunlockput(ip);
    end_op();
    return -ENOTDIR;
  }
  iunlock(ip);
  iput(p->cwd);
  end_op();
  p->cwd = ip;
  return 0;
}

// mkdirat(dirfd, path, mode). xv6 has no permissions, mode is ignored.
uint64
linux_mkdirat(void)
{
  char path[MAXPATH];
  int dirfd;
  struct inode *ip;

  argint(0, &dirfd);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  if (dirfd != AT_FDCWD && path[0] != '/')
    return -EINVAL;

  begin_op();
  if ((ip = namei(path)) != 0) {
    iput(ip);
    end_op();
    return -EEXIST;
  }
  if ((ip = create(path, T_DIR, 0, 0)) == 0) {
    end_op();
    return -ENOENT; // the parent directory doesn't exist
  }
  iunlockput(ip);
  end_op();
  return 0;
}

// faccessat(dirfd, path, mode): may we read/write/run path?
// everyone is root on xv6, so the answer is yes if the file exists.
uint64
linux_faccessat(void)
{
  char path[MAXPATH];
  int dirfd;
  struct inode *ip;

  argint(0, &dirfd);
  if (argstr(1, path, MAXPATH) < 0)
    return -EFAULT;
  if (dirfd != AT_FDCWD && path[0] != '/')
    return -EINVAL;

  begin_op();
  ip = namei(path);
  if (ip)
    iput(ip);
  end_op();
  return ip ? 0 : -ENOENT;
}
