//
// XSL: memory system calls for Linux programs:
// brk, mmap, munmap and mprotect.
//
// pages are allocated as soon as they are asked for (no lazy
// allocation), using xv6's own uvmalloc(), uvmdealloc() and uvmunmap().
// so any page that is not mapped really is outside the program's
// memory, and touching it kills the program.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "linux.h"

// brk(addr): move the end of the heap to addr.
// glibc's malloc uses this for small allocations.
uint64
linux_brk(void)
{
  uint64 addr;
  struct proc *p = myproc();

  argaddr(0, &addr);

  // brk(0) just asks where the heap ends. a request outside the
  // heap area fails, which Linux reports by returning the old end.
  if (addr < p->brkbase || addr >= LINUX_MMAP_BASE)
    return p->brk;

  if (addr > p->brk) {
    // grow: map zeroed pages from the old end up to the new end
    if (uvmalloc(p->pagetable, p->brk, addr, PTE_W) == 0)
      return p->brk;
  } else {
    // shrink: free the pages above the new end
    uvmdealloc(p->pagetable, p->brk, addr);
  }
  p->brk = addr;
  return addr;
}

// mmap(addr, len, prot, flags, fd, offset).
// we only support what glibc's malloc needs: anonymous memory
// (not backed by a file) at an address the kernel picks.
// each call takes the next free range above LINUX_MMAP_BASE.
uint64
linux_mmap(void)
{
  uint64 addr, len;
  int prot, flags, perm;
  struct proc *p = myproc();

  argaddr(1, &len);
  argint(2, &prot);
  argint(3, &flags);

  if ((flags & MAP_ANONYMOUS) == 0 || (flags & MAP_FIXED) != 0)
    return -EINVAL; // file mappings and fixed addresses not supported
  if (len == 0 || len > LINUX_STACK_TOP || prot == 0)
    return -EINVAL;
  len = PGROUNDUP(len);

  addr = p->mmapnext;
  if (addr + len > LINUX_STACK_TOP - LINUX_STACK_PAGES * PGSIZE)
    return -ENOMEM; // would run into the stack

  perm = 0;
  if (prot & PROT_WRITE)
    perm |= PTE_W;
  if (prot & PROT_EXEC)
    perm |= PTE_X;
  if (uvmalloc(p->pagetable, addr, addr + len, perm) == 0)
    return -ENOMEM;

  p->mmapnext = addr + len;
  return addr;
}

// munmap(addr, len): free memory that came from mmap.
// addresses are not reused, which is fine for a small program.
uint64
linux_munmap(void)
{
  uint64 addr, len;
  struct proc *p = myproc();

  argaddr(0, &addr);
  argaddr(1, &len);
  len = PGROUNDUP(len);
  if (addr % PGSIZE != 0 || len == 0 || addr < LINUX_MMAP_BASE ||
      addr >= p->mmapnext || len > p->mmapnext - addr)
    return -EINVAL;

  uvmunmap(p->pagetable, addr, len / PGSIZE, 1); // skips pages already freed
  return 0;
}

// mprotect(addr, len, prot): change the permission bits in the
// page table entries. glibc uses it at startup to make some data
// read-only after it has been set up.
uint64
linux_mprotect(void)
{
  uint64 addr, len, a;
  int prot, perm;
  pte_t *pte;
  struct proc *p = myproc();

  argaddr(0, &addr);
  argaddr(1, &len);
  argint(2, &prot);
  len = PGROUNDUP(len);
  if (addr % PGSIZE != 0 || addr >= LINUX_STACK_TOP ||
      len > LINUX_STACK_TOP - addr)
    return -EINVAL;
  if (prot == 0)
    return -EINVAL; // PROT_NONE not supported

  // R is always on: a RISC-V PTE with R, W and X all off means
  // "pointer to the next page table level", not "no access".
  perm = PTE_R;
  if (prot & PROT_WRITE)
    perm |= PTE_W;
  if (prot & PROT_EXEC)
    perm |= PTE_X;

  // check every page first, so a failure changes nothing
  for (a = addr; a < addr + len; a += PGSIZE) {
    pte = walk(p->pagetable, a, 0);
    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -ENOMEM;
  }
  for (a = addr; a < addr + len; a += PGSIZE) {
    pte = walk(p->pagetable, a, 0);
    *pte = (*pte & ~(PTE_R | PTE_W | PTE_X)) | perm;
  }
  sfence_vma(); // the TLB may still hold the old permissions
  return 0;
}
