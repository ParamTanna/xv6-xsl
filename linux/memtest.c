// Linux memory system calls: brk (small mallocs), mmap (big mallocs
// and direct mmap), munmap and mprotect.
// "memtest crash" also writes to a read-only page, which must kill it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

int
main(int argc, char *argv[])
{
  char *start, *small[400], *big, *p;
  long sum;
  int i;

  // small mallocs come from the heap, which grows with brk().
  // 400 KB is more than the 132 KB glibc takes at startup.
  start = sbrk(0);
  for (i = 0; i < 400; i++) {
    small[i] = malloc(1000);
    memset(small[i], i, 1000);
  }
  printf("400 small mallocs: program break moved from %p to %p\n", start, sbrk(0));
  for (i = 0; i < 400; i++)
    free(small[i]);

  // glibc gives big allocations (over 128 KB) their own mmap()
  big = malloc(1024 * 1024);
  memset(big, 1, 1024 * 1024);
  sum = 0;
  for (i = 0; i < 1024 * 1024; i++)
    sum += big[i];
  printf("1 MB malloc at %p, sum of bytes = %ld (expected 1048576)\n", big, sum);
  free(big);

  // mmap directly, then make the first page read-only
  p = mmap(0, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  strcpy(p, "written into an mmap page");
  printf("mmap at %p: %s\n", p, p);
  mprotect(p, 4096, PROT_READ);
  printf("after mprotect(PROT_READ) reading still works: %s\n", p);

  if (argc > 1) {
    printf("now writing to the read-only page, the kernel should kill me\n");
    p[0] = 'X';
    printf("still alive, mprotect did not work!\n");
  }

  munmap(p, 2 * 4096);
  printf("munmap done\n");
  return 0;
}
