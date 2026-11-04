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

// ioctl(fd, request, arg). only on the console:
//   TCGETS: glibc's isatty() asks for the terminal settings. if that
//     works, stdout is a terminal and printf flushes after every line,
//     otherwise it waits for a full buffer.
// for any other file the answer is "not a terminal".
uint64
linux_ioctl(void)
{
  struct file *f;
  uint64 req, arg;
  struct linux_termios t;
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
  return -ENOTTY;
}

// readlinkat: xv6 has no symbolic links (and no /proc, which is what
// glibc asks about at startup), so there is never a link to read.
uint64
linux_readlinkat(void)
{
  return -ENOENT;
}
