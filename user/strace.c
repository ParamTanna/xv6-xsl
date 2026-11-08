// strace: run a program and have the kernel print every Linux
// system call it makes, with its arguments and result.
//   $ strace hello
#include "kernel/types.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(2, "usage: strace program [args...]\n");
    exit(1);
  }
  trace(1); // stays on across exec
  exec(argv[1], argv + 1);
  fprintf(2, "strace: cannot run %s\n", argv[1]);
  exit(1);
}
