// test for getreadcount(): the count should go up by
// exactly one for every read() call, even failed ones.
#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  char buf[16];
  int before, after, fd, i;

  before = getreadcount();

  fd = open("README", O_RDONLY);
  for (i = 0; i < 5; i++)
    read(fd, buf, sizeof(buf));
  close(fd);
  read(-1, buf, 1); // bad fd, still counts

  after = getreadcount();
  printf("reads before %d, after %d, difference %d (expected 6)\n",
         before, after, after - before);
  exit(0);
}
