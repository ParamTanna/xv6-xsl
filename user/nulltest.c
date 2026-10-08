// test for the unmapped page 0: a null pointer read or write
// must kill the process instead of returning garbage.
#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pid, status;
  char buf[4];

  printf("main is at %p (should be at 0x1000 or above)\n", main);

  // the kernel must refuse to copy from address 0 too.
  if (write(1, (char *)0, 4) != 4)
    printf("write from address 0 wrote nothing, good\n");

  pid = fork();
  if (pid == 0) {
    int *p = 0;
    printf("child: reading *NULL, this should kill me\n");
    buf[0] = *p;
    printf("child: still alive, page 0 is mapped! value %d\n", buf[0]);
    exit(0);
  }
  wait(&status);
  if (status == -1)
    printf("child was killed by the null read, good\n");
  else
    printf("child exited normally, page 0 is mapped!\n");
  exit(0);
}
