// checks the Linux starting stack that the kernel builds in exec:
// argc, argv, the environment and the auxiliary vector.
#include <stdio.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/utsname.h>

extern char **environ;

int
main(int argc, char *argv[])
{
  struct utsname u;
  char **e;
  int i;

  printf("argc = %d\n", argc);
  for (i = 0; i < argc; i++)
    printf("argv[%d] = %s\n", i, argv[i]);
  for (e = environ; *e; e++)
    printf("env: %s\n", *e);

  printf("auxv: page size = %lu, entry point = 0x%lx\n",
         getauxval(AT_PAGESZ), getauxval(AT_ENTRY));
  printf("pid = %d\n", getpid());

  if (uname(&u) == 0)
    printf("uname: %s %s %s %s\n", u.sysname, u.nodename, u.release, u.machine);
  return 0;
}
