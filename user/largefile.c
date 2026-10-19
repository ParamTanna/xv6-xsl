// write a file bigger than the old 268 KB limit and read it back.
// it needs the doubly indirect block to work.
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "kernel/fs.h"
#include "user/user.h"

// 600 KB. the old limit was 12 + 256 = 268 blocks, so blocks
// 267 to 599 need the doubly indirect block.
#define NBLK 600

int
main(int argc, char *argv[])
{
  char buf[BSIZE];
  int fd, i, *ip;

  fd = open("large.file", O_CREATE | O_WRONLY | O_TRUNC);
  if (fd < 0) {
    printf("largefile: cannot open large.file for writing\n");
    exit(1);
  }
  for (i = 0; i < NBLK; i++) {
    ip = (int *)buf;
    *ip = i; // block number at the start of each block
    if (write(fd, buf, BSIZE) != BSIZE) {
      printf("largefile: write failed at block %d\n", i);
      exit(1);
    }
    if (i % 100 == 0)
      printf(".");
  }
  close(fd);
  printf("\nwrote %d blocks\n", NBLK);

  fd = open("large.file", O_RDONLY);
  for (i = 0; i < NBLK; i++) {
    if (read(fd, buf, BSIZE) != BSIZE) {
      printf("largefile: read failed at block %d\n", i);
      exit(1);
    }
    ip = (int *)buf;
    if (*ip != i) {
      printf("largefile: block %d has wrong data %d\n", i, *ip);
      exit(1);
    }
  }
  close(fd);
  unlink("large.file");
  printf("read back %d blocks correctly, largefile ok\n", NBLK);
  exit(0);
}
