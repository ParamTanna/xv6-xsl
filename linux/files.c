// Linux file system calls on top of the xv6 file system:
// open, write, read, lseek, stat, unlink, and Linux error codes.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

int
main(void)
{
  char buf[100];
  struct stat st;
  FILE *f;
  int fd, n;

  // a missing file must give errno ENOENT, not just -1
  fd = open("nosuchfile", O_RDONLY);
  printf("open(\"nosuchfile\") = %d, errno = %d (%s)\n", fd, errno, strerror(errno));

  // create a file and write to it
  fd = open("test.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  n = write(fd, "hello from a Linux program\n", 27);
  close(fd);
  printf("created test.txt, wrote %d bytes\n", n);

  // stat it
  stat("test.txt", &st);
  printf("stat: size = %ld, inode = %ld, regular file = %s\n",
         (long)st.st_size, (long)st.st_ino, S_ISREG(st.st_mode) ? "yes" : "no");

  // read part of it back after lseek
  fd = open("test.txt", O_RDONLY);
  lseek(fd, 11, SEEK_SET);
  n = read(fd, buf, sizeof(buf) - 1);
  buf[n] = 0;
  close(fd);
  printf("read from offset 11: %s", buf);

  // O_EXCL on a file that exists must give EEXIST
  fd = open("test.txt", O_CREAT | O_EXCL | O_WRONLY, 0644);
  printf("open with O_EXCL = %d, errno = %d (%s)\n", fd, errno, strerror(errno));

  // delete it
  unlink("test.txt");
  printf("after unlink, stat gives %d (%s)\n", stat("test.txt", &st), strerror(errno));

  // glibc stdio (fopen, fgets) works too: first line of xv6's README
  f = fopen("README", "r");
  if (f && fgets(buf, sizeof(buf), f))
    printf("first line of README: %s", buf);
  if (f)
    fclose(f);
  return 0;
}
