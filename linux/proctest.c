// Linux process system calls on xv6: fork (clone), exec (execve),
// waitpid (wait4) and the exit status encoding, pipe (pipe2),
// dup2 (dup3), kill, and WNOHANG.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

int
main(void)
{
  int fd[2], status, rc;

  printf("parent: pid %d, my parent is %d\n", getpid(), getppid());

  // 1. fork and exit status
  rc = fork();
  if (rc == 0) {
    printf("child: pid %d, my parent is %d, exiting with 42\n", getpid(), getppid());
    exit(42);
  }
  waitpid(rc, &status, 0);
  printf("parent: child %d exited normally = %d, exit code = %d\n",
         rc, WIFEXITED(status), WEXITSTATUS(status));

  // 2. pipe, dup2 and exec: BusyBox's wc counts what we send it
  pipe(fd);
  rc = fork();
  if (rc == 0) {
    dup2(fd[0], 0); // stdin is now the read end of the pipe
    close(fd[0]);
    close(fd[1]);
    execl("/busybox", "wc", "-c", (char *)0);
    perror("exec");
    exit(1);
  }
  close(fd[0]);
  write(fd[1], "hello through a pipe\n", 21);
  close(fd[1]); // now wc sees end of file
  printf("parent: sent 21 bytes into the pipe, wc says: ");
  fflush(stdout);
  waitpid(rc, &status, 0);

  // 3. kill: the child sleeps, the parent kills it with SIGTERM
  rc = fork();
  if (rc == 0) {
    sleep(100);
    exit(0);
  }
  kill(rc, SIGTERM);
  waitpid(rc, &status, 0);
  printf("parent: killed child: killed by a signal = %d, signal = %d (SIGTERM is %d)\n",
         WIFSIGNALED(status), WTERMSIG(status), SIGTERM);

  // 4. WNOHANG: don't wait if the child hasn't finished yet
  rc = fork();
  if (rc == 0) {
    sleep(1);
    exit(7);
  }
  printf("parent: waitpid with WNOHANG right away = %d (child still running)\n",
         waitpid(rc, &status, WNOHANG));
  waitpid(rc, &status, 0);
  printf("parent: after a real wait, exit code = %d\n", WEXITSTATUS(status));
  return 0;
}
