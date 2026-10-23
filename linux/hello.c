// the simplest Linux program. built on Ubuntu with
//   riscv64-linux-gnu-gcc -static
// and copied to xv6 without any change.
#include <stdio.h>

int
main(void)
{
  printf("Hello from Linux! This program was built for Linux and runs unmodified on xv6.\n");
  return 0;
}
