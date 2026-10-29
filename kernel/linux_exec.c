//
// XSL: loading a Linux program in exec().
//
// kexec() in exec.c reads the ELF header. if linux_iself() says the
// file is a Linux program, kexec() calls linux_exec() instead of
// loading it the xv6 way. the differences from xv6's loader:
//   - segments go wherever the ELF file says (0x10000 for Linux),
//     and need not start on a page boundary
//   - the stack is LINUX_STACK_PAGES pages high up in memory
//   - the stack starts with argc, argv, envp and the auxiliary
//     vector, which is what glibc's _start reads
//   - the heap (brk) starts right after the program
//   - the process is marked ABI_LINUX, so its system calls go to
//     the Linux table (linux_syscall.c)
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "elf.h"
#include "linux.h"

// xv6 has no environment variables, so every Linux program
// gets this small environment from the kernel.
static char *linux_envp[] = {"HOME=/", "PATH=/", "TERM=vt100", 0};

// is this ELF file a Linux program?
// glibc puts a note called the "GNU ABI tag" into every program it
// links (it comes from crt1.o). the note says the program was built
// for Linux. FreeBSD's Linux layer recognises Linux programs the
// same way. xv6's own programs don't have this note.
int
linux_iself(struct inode *ip, struct elfhdr *elf)
{
  struct proghdr ph;
  uint off, pos, end;
  uint hdr[3]; // namesz, descsz, type
  char name[4];
  uint os;
  int i;

  for (i = 0, off = elf->phoff; i < elf->phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      return 0;
    if (ph.type != ELF_PROG_NOTE)
      continue;

    // a note segment is a list of notes, each one is
    //   namesz, descsz, type, name (padded to 4 bytes), desc (padded)
    pos = ph.off;
    end = ph.off + ph.filesz;
    while (pos + sizeof(hdr) <= end) {
      if (readi(ip, 0, (uint64)hdr, pos, sizeof(hdr)) != sizeof(hdr))
        return 0;
      if (hdr[2] == NT_GNU_ABI_TAG && hdr[0] == 4) {
        // name is "GNU\0", the first word of desc is the OS
        readi(ip, 0, (uint64)name, pos + 12, 4);
        readi(ip, 0, (uint64)&os, pos + 16, 4);
        if (memcmp(name, "GNU", 4) == 0 && os == ELF_NOTE_OS_LINUX)
          return 1;
      }
      pos += sizeof(hdr) + ((hdr[0] + 3) & ~3) + ((hdr[1] + 3) & ~3);
    }
  }
  return 0;
}

// map one zeroed page at va.
static int
mapzero(pagetable_t pagetable, uint64 va, int perm)
{
  char *mem;

  if ((mem = kalloc()) == 0)
    return -1;
  memset(mem, 0, PGSIZE);
  if (mappages(pagetable, va, PGSIZE, (uint64)mem, perm) != 0) {
    kfree(mem);
    return -1;
  }
  return 0;
}

// load one PT_LOAD segment: map its pages, then copy in its bytes
// from the file. unlike xv6 programs, a Linux segment need not start
// on a page boundary (glibc's data segment starts at e.g. 0x7e380).
static int
loadseg(pagetable_t pagetable, struct inode *ip, struct proghdr *ph)
{
  uint64 va, pa, n;
  pte_t *pte;
  int perm = PTE_R | PTE_U;

  if (ph->flags & ELF_PROG_FLAG_WRITE)
    perm |= PTE_W;
  if (ph->flags & ELF_PROG_FLAG_EXEC)
    perm |= PTE_X;

  // 1. zeroed pages for the whole segment. the part after filesz
  //    is the bss, which must be zero.
  for (va = PGROUNDDOWN(ph->vaddr); va < ph->vaddr + ph->memsz; va += PGSIZE) {
    pte = walk(pagetable, va, 0);
    if (pte && (*pte & PTE_V)) {
      // first page is shared with the end of the previous segment
      *pte |= perm;
      continue;
    }
    if (mapzero(pagetable, va, perm) < 0)
      return -1;
  }

  // 2. copy filesz bytes from the file, at most one page at a time
  for (va = ph->vaddr; va < ph->vaddr + ph->filesz; va += n) {
    pa = PTE2PA(*walk(pagetable, va, 0)) + va % PGSIZE;
    n = PGSIZE - va % PGSIZE;
    if (n > ph->vaddr + ph->filesz - va)
      n = ph->vaddr + ph->filesz - va;
    if (readi(ip, 0, pa, ph->off + (va - ph->vaddr), n) != n)
      return -1;
  }
  return 0;
}

// copy a string onto the new stack, return its user address or 0.
static uint64
pushstr(pagetable_t pagetable, uint64 *sp, char *s)
{
  *sp -= strlen(s) + 1;
  if (copyout(pagetable, LINUX_STACK_TOP, *sp, s, strlen(s) + 1) < 0)
    return 0;
  return *sp;
}

// build the stack a Linux program starts with:
//
//   sp ->  argc
//          argv[0] ... argv[argc-1], 0
//          envp[0] ... envp[n-1], 0
//          auxv: (type, value) pairs, ending with (AT_NULL, 0)
//          ...
//          the strings and the 16 random bytes these point to
//          LINUX_STACK_TOP
//
// the auxiliary vector tells glibc things it can't find out itself,
// e.g. where the program headers are (to set up thread-local
// storage) and 16 random bytes (for its stack protector canary).
// returns the new sp, or 0 on error.
static uint64
linux_stack(pagetable_t pagetable, char **argv, char **envp,
            struct elfhdr *elf, uint64 phdr)
{
  uint64 sp = LINUX_STACK_TOP;
  uint64 rnd[2], random;
  uint64 *v;
  int argc, envc, n, i;

  for (argc = 0; argv[argc]; argc++)
    ;
  for (envc = 0; envp[envc]; envc++)
    ;
  if (argc > MAXARG || envc > MAXARG)
    return 0;

  // the words (argc, argv, envp, auxv) are built in a kernel page
  // first and copied out in one go at the end.
  //   v[0]                    argc
  //   v[1 .. argc]            argv pointers, then 0
  //   v[argc+2 ..]            envp pointers, then 0
  if ((v = (uint64 *)kalloc()) == 0)
    return 0;

  // 1. strings at the top of the stack; their addresses go into v
  v[0] = argc;
  for (i = 0; i < argc; i++) {
    if ((v[1 + i] = pushstr(pagetable, &sp, argv[i])) == 0)
      goto bad;
  }
  v[1 + argc] = 0;
  for (i = 0; i < envc; i++) {
    if ((v[2 + argc + i] = pushstr(pagetable, &sp, envp[i])) == 0)
      goto bad;
  }
  v[2 + argc + envc] = 0;
  n = 3 + argc + envc;

  // 2. 16 random bytes for AT_RANDOM
  rnd[0] = linux_random();
  rnd[1] = linux_random();
  sp -= 16;
  sp -= sp % 16;
  if (copyout(pagetable, LINUX_STACK_TOP, sp, (char *)rnd, 16) < 0)
    goto bad;
  random = sp;

  // 3. the auxiliary vector
  v[n++] = AT_PHDR;   v[n++] = phdr;
  v[n++] = AT_PHENT;  v[n++] = sizeof(struct proghdr);
  v[n++] = AT_PHNUM;  v[n++] = elf->phnum;
  v[n++] = AT_PAGESZ; v[n++] = PGSIZE;
  v[n++] = AT_ENTRY;  v[n++] = elf->entry;
  v[n++] = AT_UID;    v[n++] = 0; // everyone is root on xv6
  v[n++] = AT_EUID;   v[n++] = 0;
  v[n++] = AT_GID;    v[n++] = 0;
  v[n++] = AT_EGID;   v[n++] = 0;
  v[n++] = AT_SECURE; v[n++] = 0;
  v[n++] = AT_HWCAP;  v[n++] = LINUX_HWCAP;
  v[n++] = AT_RANDOM; v[n++] = random;
  v[n++] = AT_NULL;   v[n++] = 0;

  sp -= n * sizeof(uint64);
  sp -= sp % 16; // riscv sp must be 16-byte aligned
  if (sp < LINUX_STACK_TOP - LINUX_STACK_PAGES * PGSIZE ||
      copyout(pagetable, LINUX_STACK_TOP, sp, (char *)v, n * sizeof(uint64)) < 0)
    goto bad;
  kfree(v);
  return sp;

bad:
  kfree(v);
  return 0;
}

// replace the current process with the Linux program ip.
// called by kexec() with ip locked and inside a transaction;
// we finish both. envp is 0 when exec came from an xv6 program,
// then the program gets linux_envp. returns 0 on success, -1 on failure.
int
linux_exec(char *path, struct inode *ip, struct elfhdr *elf, char **argv,
           char **envp)
{
  struct proc *p = myproc();
  pagetable_t pagetable = 0, oldpagetable;
  struct proghdr ph;
  uint64 phdr = 0, end = 0, sp, oldsz;
  char *s, *last;
  int i, off;

  // only static, 64-bit RISC-V executables
  if (elf->elf[0] != ELF_CLASS64 || elf->machine != EM_RISCV ||
      elf->type != ELF_ET_EXEC)
    goto bad;

  if ((pagetable = proc_pagetable(p)) == 0)
    goto bad;

  for (i = 0, off = elf->phoff; i < elf->phnum; i++, off += sizeof(ph)) {
    if (readi(ip, 0, (uint64)&ph, off, sizeof(ph)) != sizeof(ph))
      goto bad;
    if (ph.type == ELF_PROG_INTERP)
      goto bad; // dynamically linked, would need ld.so
    if (ph.type != ELF_PROG_LOAD)
      continue;
    if (ph.memsz < ph.filesz || ph.vaddr + ph.memsz < ph.vaddr)
      goto bad;
    if (ph.vaddr < PGSIZE || ph.vaddr + ph.memsz > LINUX_MMAP_BASE)
      goto bad;
    if (loadseg(pagetable, ip, &ph) < 0)
      goto bad;
    // the program headers sit inside the first segment. glibc needs
    // their address (AT_PHDR) to find its TLS segment.
    if (elf->phoff >= ph.off && elf->phoff < ph.off + ph.filesz)
      phdr = ph.vaddr + (elf->phoff - ph.off);
    if (ph.vaddr + ph.memsz > end)
      end = ph.vaddr + ph.memsz;
  }
  iunlockput(ip);
  end_op();
  ip = 0;

  // the stack: LINUX_STACK_PAGES pages ending at LINUX_STACK_TOP
  if (uvmalloc(pagetable, LINUX_STACK_TOP - LINUX_STACK_PAGES * PGSIZE,
               LINUX_STACK_TOP, PTE_W) == 0)
    goto bad;
  if (envp == 0)
    envp = linux_envp;
  if ((sp = linux_stack(pagetable, argv, envp, elf, phdr)) == 0)
    goto bad;

  for (last = s = path; *s; s++)
    if (*s == '/')
      last = s + 1;
  safestrcpy(p->name, last, sizeof(p->name));

  // commit to the new program
  oldpagetable = p->pagetable;
  oldsz = p->sz;
  p->pagetable = pagetable;
  p->sz = LINUX_STACK_TOP;
  p->abi = ABI_LINUX;
  p->brkbase = PGROUNDUP(end); // heap starts on the page after the bss
  p->brk = p->brkbase;
  p->mmapnext = LINUX_MMAP_BASE;

  // Linux starts a program with all registers zero except pc and sp.
  // glibc's _start needs a0 = 0 ("no exit function from ld.so").
  // a0 is also the return value of exec, which is 0.
  memset(p->trapframe, 0, sizeof(*p->trapframe));
  p->trapframe->epc = elf->entry;
  p->trapframe->sp = sp;
  proc_freepagetable(oldpagetable, oldsz);
  return 0;

bad:
  if (pagetable)
    proc_freepagetable(pagetable, LINUX_STACK_TOP);
  if (ip) {
    iunlockput(ip);
    end_op();
  }
  return -1;
}
