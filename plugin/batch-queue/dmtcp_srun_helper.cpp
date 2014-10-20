#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/types.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <assert.h>

extern "C" void slurm_srun_register_fds(int in, int out, int err) __attribute((weak));

int quit_pending = 0;
int srun_stdin = -1;
int srun_stdout = -1;
int srun_stderr = -1;
pid_t srun_pid;

//-------------------------------------8<------------------------------------------------//
// FIXME: this is exactly the same code as in src/plugin/ipc/ssh/util_ssh.cpp
// We need to use the same code base in future.
// TODO: put this in some shared util location.

#define MAX(a,b) ((a) < (b) ? (b) : (a))

#define MAX_BUFFER_SIZE (64*1024)

struct Buffer {
  char *buf;
  int  off;
  int  end;
  int  len;
};

static void buffer_init(struct Buffer *buf);
static void buffer_free(struct Buffer *buf);
static void buffer_read(struct Buffer *buf, int fd);
static void buffer_write(struct Buffer *buf, int fd);

static void buffer_init(struct Buffer *buf)
{
  assert(buf != NULL);
  buf->buf = (char*) malloc(MAX_BUFFER_SIZE);
  assert(buf->buf != NULL);
  buf->off = 0;
  buf->end = 0;
  buf->len = MAX_BUFFER_SIZE;
}

static void buffer_free(struct Buffer *buf)
{
  free(buf->buf);
  buf->buf = NULL;
  buf->len = 0;
}

static void buffer_readjust(struct Buffer *buf)
{
  memmove(buf->buf, &buf->buf[buf->off], buf->end - buf->off);
  buf->end -= buf->off;
  buf->off = 0;
}

static bool buffer_ready_for_read(struct Buffer *buf)
{
  assert(buf->buf != NULL && buf->len != 0);
  return buf->end < buf->len - 1;
}

static void buffer_read(struct Buffer *buf, int fd)
{
  assert(buf->buf != NULL && buf->len != 0);

  if (buf->end < buf->len) {
    size_t max = buf->len - buf->end;
    ssize_t rc = read(fd, &buf->buf[buf->end], max);
    if (rc == 0 || (rc == -1 && errno != EINTR)) {
      quit_pending = 1;
      return;
    }
    buf->end += rc;
  }
}

static bool buffer_ready_for_write(struct Buffer *buf)
{
  assert(buf->buf != NULL && buf->len != 0);
  return buf->end > buf->off;
}

static void buffer_write(struct Buffer *buf, int fd)
{
  assert(buf->buf != NULL && buf->len != 0);

  assert(buf->end > buf->off);
  size_t max = buf->end - buf->off;
  ssize_t rc = write(fd,  &buf->buf[buf->off], max);
  if (rc == -1 && errno != EINTR) {
    quit_pending = 1;
    return;
  }
  buf->off += rc;
  if (buf->off > buf->len / 2) {
    buffer_readjust(buf);
  }
}


/* set/unset filedescriptor to non-blocking */
static void set_nonblock(int fd)
{
  int val;
  val = fcntl(fd, F_GETFL, 0);
  if (val < 0) {
    perror("fcntl failed");
  }
  val |= O_NONBLOCK;
  if (fcntl(fd, F_SETFL, val) == -1) {
    perror("fcntl failed");
  }
}

// End of FIXME
//-------------------------------------8<------------------------------------------------//


static struct Buffer stdin_buffer, stdout_buffer, stderr_buffer;

/*
 * Signal handler for signals that cause the program to terminate.  These
 * signals must be trapped to restore terminal modes.
 */
static void signal_handler(int sig)
{
  quit_pending = 1;
  if( sig == SIGCHLD )
    return;
  if (srun_pid != -1) {
    kill(srun_pid, sig);
  }
}

static void create_stdio_fds(int *in, int *out, int *err)
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

pid_t fork_srun(int argc, char **argv, char **envp)
{
  int in[2], out[2], err[2];
  create_stdio_fds(in, out, err);
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

void client_loop()
{
  static struct Buffer stdin_buffer, stdout_buffer, stderr_buffer;

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

  if (signal(SIGTERM, SIG_IGN) != SIG_IGN)
    signal(SIGTERM, signal_handler);

  // Track srun
  signal(SIGCHLD, signal_handler);

  fd_set readset, writeset, errorset;
  int max_fd = 0;

  max_fd = MAX( MAX(srun_stdin, srun_stdout), srun_stderr);

  /* Main loop of the client for the interactive session mode. */
  while (!quit_pending) {
    struct timeval tv = {10, 0};

    // Handle errors on all fds of interest
    FD_ZERO(&errorset);
    FD_ZERO(&readset);
    FD_ZERO(&writeset);

    if (buffer_ready_for_read(&stdin_buffer)) {
      FD_SET(STDIN_FILENO, &readset);
    }
    if (buffer_ready_for_read(&stdout_buffer)) {
      FD_SET(srun_stdout, &readset);
    }
    if (buffer_ready_for_read(&stderr_buffer)) {
      FD_SET(srun_stderr, &readset);
    }

    if (buffer_ready_for_write(&stdin_buffer)) {
      FD_SET(srun_stdin, &writeset);
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
    if (FD_ISSET(srun_stdout, &readset)) {
      buffer_read(&stdout_buffer, srun_stdout);
    }
    if (FD_ISSET(srun_stderr, &readset)) {
      buffer_read(&stderr_buffer, srun_stderr);
    }

    // Write to our stdout/err or stdin of ssh
    if (FD_ISSET(srun_stdin, &writeset)) {
      buffer_write(&stdin_buffer, srun_stdin);
    }
    if (FD_ISSET(STDOUT_FILENO, &writeset)) {
      buffer_write(&stdout_buffer, STDOUT_FILENO);
    }
    if (FD_ISSET(STDERR_FILENO, &writeset)) {
      buffer_write(&stderr_buffer, STDERR_FILENO);
    }
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

  assert(slurm_srun_register_fds != NULL);

  srun_pid = fork_srun(argc, argv, envp);
  slurm_srun_register_fds(srun_stdin, srun_stdout, srun_stderr);

  client_loop();
  wait(&status);
  return status;
}
