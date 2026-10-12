// test for mprotect() and munprotect().
#include "kernel/types.h"
#include "kernel/riscv.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  char *page;
  int pid, status;

  // get a fresh page-aligned page from sbrk.
  sbrk(PGSIZE - ((uint64)sbrk(0) % PGSIZE));
  page = sbrk(PGSIZE);
  page[0] = 'a';

  if (mprotect(page, 1) < 0) {
    printf("mprotect failed\n");
    exit(1);
  }
  printf("page %p is now read-only, reading it gives '%c'\n", page, page[0]);

  // bad arguments
  if (mprotect(page + 1, 1) == 0)
    printf("unaligned address was accepted!\n");
  if (mprotect(page, 1000) == 0)
    printf("range past the end was accepted!\n");
  if (mprotect(0, 1) == 0)
    printf("page 0 was accepted!\n");

  // the kernel must not write into it on our behalf either.
  if (pipe((int *)page) < 0)
    printf("kernel refused to write into the read-only page, good\n");

  // read-only is inherited by fork, so the child dies on write.
  pid = fork();
  if (pid == 0) {
    printf("child: writing to the read-only page\n");
    page[0] = 'b';
    printf("child: write worked, protection is broken!\n");
    exit(0);
  }
  wait(&status);
  if (status == -1)
    printf("child was killed by the write, good\n");

  munprotect(page, 1);
  page[0] = 'c';
  printf("after munprotect the write works: '%c'\n", page[0]);
  exit(0);
}
