#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <sys/wait.h>

int srun_stdin = -1;
int srun_stdout = -1;
int srun_stderr = -1;

extern "C" void slurmHelperRegisterFds(int in, int out, int err) __attribute((weak));

static void createStdioFds(int *in, int *out, int *err)
{
  struct stat buf;
  if (fstat(STDIN_FILENO,  &buf)  == -1) {
    int fd = open("/dev/null", O_RDWR);
    if (fd != STDIN_FILENO) {
      dup2(fd, STDIN_FILENO);
      close(fd);
    }
  }
  if (fstat(STDOUT_FILENO,  &buf)  == -1) {
    int fd = open("/dev/null", O_RDWR);
    if (fd != STDOUT_FILENO) {
      dup2(fd, STDOUT_FILENO);
      close(fd);
    }
  }
  if (fstat(STDERR_FILENO,  &buf)  == -1) {
    int fd = open("/dev/null", O_RDWR);
    if (fd != STDERR_FILENO) {
      dup2(fd, STDERR_FILENO);
      close(fd);
    }
  }

  // Close all open file descriptors
  int maxfd = sysconf(_SC_OPEN_MAX);
  for (int i = 3; i < maxfd; i++) {
    close(i);
  }

  if (pipe(in) != 0) {
    perror("Error creating pipe: ");
  }
  if (pipe(out) != 0) {
    perror("Error creating pipe: ");
  }
  if (pipe(err) != 0) {
    perror("Error creating pipe: ");
  }
}

pid_t forkSrun(int argc, char **argv, char **envp)
{
  int in[2], out[2], err[2];
  createStdioFds(in, out, err);
  unsetenv("LD_PRELOAD");
  pid_t pid = fork();
  if( pid == 0 ) {
    close(in[1]);
    close(out[0]);
    close(err[0]);
    dup2(in[0], STDIN_FILENO);
    dup2(out[1], STDOUT_FILENO);
    dup2(err[1], STDERR_FILENO);

    unsetenv("LD_PRELOAD");
    execvpe(argv[1], &argv[1], envp);
    printf("%s:%d DMTCP Error detected. Failed to exec.", __FILE__, __LINE__);
    abort();
  }

  close(in[0]);
  close(out[1]);
  close(err[1]);

  srun_stdin = in[1];
  srun_stdout = out[0];
  srun_stderr = err[0];

  return pid;
}

void client_loop(int ssh_stdin, int ssh_stdout, int ssh_stderr, int sock)
{
  remoteSock = sock;
  /* Initialize buffers. */
  buffer_init(&stdin_buffer);
  buffer_init(&stdout_buffer);
  buffer_init(&stderr_buffer);

  /* enable nonblocking unless tty */
  set_nonblock(fileno(stdin));
  set_nonblock(fileno(stdout));
  set_nonblock(fileno(stderr));

  /*
   * Set signal handlers, (e.g. to restore non-blocking mode)
   * but don't overwrite SIG_IGN, matches behaviour from rsh(1)
   */
  if (signal(SIGHUP, SIG_IGN) != SIG_IGN)
    signal(SIGHUP, signal_handler);
  if (signal(SIGINT, SIG_IGN) != SIG_IGN)
    signal(SIGINT, signal_handler);
  if (signal(SIGQUIT, SIG_IGN) != SIG_IGN)
    signal(SIGQUIT, signal_handler);
  if (signal(SIGTERM, SIG_IGN) != SIG_IGN)
    signal(SIGTERM, signal_handler);
  //signal(SIGWINCH, window_change_handler);

  fd_set readset, writeset, errorset;
  int max_fd = 0;

  max_fd = MAX(ssh_stdin, ssh_stdout);
  max_fd = MAX(max_fd, ssh_stderr);

  /* Main loop of the client for the interactive session mode. */
  while (!quit_pending) {
    struct timeval tv = {10, 0};
    FD_ZERO(&readset);
    FD_ZERO(&writeset);
    FD_ZERO(&errorset);
    FD_SET(remoteSock, &errorset);

    if (buffer_ready_for_read(&stdin_buffer)) {
      FD_SET(STDIN_FILENO, &readset);
    }
    if (buffer_ready_for_read(&stdout_buffer)) {
      FD_SET(ssh_stdout, &readset);
    }
    if (buffer_ready_for_read(&stderr_buffer)) {
      FD_SET(ssh_stderr, &readset);
    }

    if (buffer_ready_for_write(&stdin_buffer)) {
      FD_SET(ssh_stdin, &writeset);
    }
    if (buffer_ready_for_write(&stdout_buffer)) {
      FD_SET(STDOUT_FILENO, &writeset);
    }
    if (buffer_ready_for_write(&stderr_buffer)) {
      FD_SET(STDERR_FILENO, &writeset);
    }

    int ret = select(max_fd, &readset, &writeset, &errorset, &tv);
    if (ret == -1 && errno == EINTR) {
      continue;
    }
    if (ret == -1) {
      perror("select failed");
      return;
    }

    if (quit_pending)
      break;

    //Read from our STDIN or stdout/err of ssh
    if (FD_ISSET(STDIN_FILENO, &readset)) {
      buffer_read(&stdin_buffer, STDIN_FILENO);
    }
    if (FD_ISSET(ssh_stdout, &readset)) {
      buffer_read(&stdout_buffer, ssh_stdout);
    }
    if (FD_ISSET(ssh_stderr, &readset)) {
      buffer_read(&stderr_buffer, ssh_stderr);
    }

    // Write to our stdout/err or stdin of ssh
    if (FD_ISSET(ssh_stdin, &writeset)) {
      buffer_write(&stdin_buffer, ssh_stdin);
    }
    if (FD_ISSET(STDOUT_FILENO, &writeset)) {
      buffer_write(&stdout_buffer, STDOUT_FILENO);
    }
    if (FD_ISSET(STDERR_FILENO, &writeset)) {
      buffer_write(&stderr_buffer, STDERR_FILENO);
    }

    if (FD_ISSET(remoteSock, &errorset)) {
      break;
    }

    if (quit_pending)
      break;
  }

  /* Write pending data to our stdout/stderr */
  if (buffer_ready_for_write(&stdout_buffer)) {
    buffer_write(&stdout_buffer, STDOUT_FILENO);
  }
  if (buffer_ready_for_write(&stderr_buffer)) {
    buffer_write(&stderr_buffer, STDERR_FILENO);
  }

  /* Clear and free any buffers. */
  buffer_free(&stdin_buffer);
  buffer_free(&stdout_buffer);
  buffer_free(&stderr_buffer);
}


int main(int argc, char **argv, char **envp)
{
  int status;

  assert(slurmHelperRegisterFds != NULL);

  pid_t pid = forkSrun(argc, argv, envp);
  slurmHelperRegisterFds(srun_stdin, srun_stdout, srun_stderr);

  while(1){
    sleep(1);
  }
  wait(&status);
  return status;
}
