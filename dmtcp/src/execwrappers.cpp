/****************************************************************************
 *   Copyright (C) 2006-2008 by Jason Ansel, Kapil Arya, and Gene Cooperman *
 *   jansel@csail.mit.edu, kapil@ccs.neu.edu, gene@ccs.neu.edu              *
 *                                                                          *
 *   This file is part of the dmtcp/src module of DMTCP (DMTCP:dmtcp/src).  *
 *                                                                          *
 *  DMTCP:dmtcp/src is free software: you can redistribute it and/or        *
 *  modify it under the terms of the GNU Lesser General Public License as   *
 *  published by the Free Software Foundation, either version 3 of the      *
 *  License, or (at your option) any later version.                         *
 *                                                                          *
 *  DMTCP:dmtcp/src is distributed in the hope that it will be useful,      *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 *  GNU Lesser General Public License for more details.                     *
 *                                                                          *
 *  You should have received a copy of the GNU Lesser General Public        *
 *  License along with DMTCP:dmtcp/src.  If not, see                        *
 *  <http://www.gnu.org/licenses/>.                                         *
 ****************************************************************************/

#include <stdarg.h>
#include <stdlib.h>
#include "syscallwrappers.h"
#include  "../jalib/jassert.h"
#include "uniquepid.h"
#include "dmtcpworker.h"
#include "sockettable.h"
#include "protectedfds.h"
#include "constants.h"
// extern "C" int fexecve(int fd, char *const argv[], char *const envp[])
// {
//     _real_fexecve(fd,argv,envp);
// }
//
// extern "C" int execve(const char *filename, char *const argv[],
//                 char *const envp[])
// {
//     _real_execve(filename,argv,envp);
// }

#include "protectedfds.h"
#include "sockettable.h"
#include "connectionmanager.h"
#include "connectionidentifier.h"
#include "syslogcheckpointer.h"
#include  "../jalib/jconvert.h"
#include "constants.h"
#include <vector>
#include <list>
#include <string>

#define INITIAL_ARGV_MAX 32

static void protectLD_PRELOAD();

extern "C" int close ( int fd )
{
  if ( dmtcp::ProtectedFDs::isProtected ( fd ) )
  {
    JTRACE ( "blocked attempt to close protected fd" ) ( fd );
    errno = EBADF;
    return -1;
  }

  int rv = _real_close ( fd );

  // #ifdef DEBUG
  //     if(rv==0)
  //     {
  //         dmtcp::string closeDevice = dmtcp::KernelDeviceToConnection::Instance().fdToDevice( fd );
  //         if(closeDevice != "") JTRACE("close()")(fd)(closeDevice);
  //     }
  // #endif
  //     else
  //     {
  // #ifdef DEBUG
  //         if(dmtcp::SocketTable::Instance()[fd].state() != dmtcp::SocketEntry::T_INVALID)
  //         {
  //             dmtcp::SocketEntry& e = dmtcp::SocketTable::Instance()[fd];
  //             JTRACE("CLOSE()")(fd)(e.remoteId().id)(e.state());
  //         }
  // #endif
  //         dmtcp::SocketTable::Instance().resetFd(fd);
  //     }
  return rv;
}

static pid_t fork_work()
{
  protectLD_PRELOAD();

  /* Little bit cheating here: child_time should be same for both parent and
   * child, thus we compute it before forking the child. */
  time_t child_time = time ( NULL );
  pid_t child_pid = _real_fork();
  if (child_pid < 0) {
    return child_pid;
  }

  long child_host = dmtcp::UniquePid::ThisProcess().hostid();

  dmtcp::UniquePid parent = dmtcp::UniquePid::ThisProcess();

  if ( child_pid == 0 )
  {
    child_pid = _real_getpid();
    dmtcp::UniquePid child = dmtcp::UniquePid ( child_host, child_pid, child_time );
#ifdef DEBUG
    //child should get new logfile
    dmtcp::ostringstream o;
    o << getenv(ENV_VAR_TMPDIR) << "/jassertlog." << child.toString();
    JASSERT_SET_LOGFILE (o.str());
    //JASSERT_SET_LOGFILE ( jalib::XToString(getenv(ENV_VAR_TMPDIR))
    //                      + "/jassertlog." + jalib::XToString ( child_pid ) );
#endif


    JTRACE ( "fork()ed [CHILD]" ) ( child ) ( getenv ( "LD_PRELOAD" ) );

    //fix the mutex
    _dmtcp_remutex_on_fork();

    //update ThisProcess()
    dmtcp::UniquePid::resetOnFork ( child );

#ifdef PID_VIRTUALIZATION
    dmtcp::VirtualPidTable::Instance().resetOnFork( );
#endif

    dmtcp::SyslogCheckpointer::resetOnFork();

    //rewrite socket table
    //         dmtcp::SocketTable::Instance().onForkUpdate(parent,child);

    //make new connection to coordinator
    dmtcp::DmtcpWorker::resetOnFork();

    JTRACE ( "fork() done [CHILD]" ) ( child ) ( getenv ( "LD_PRELOAD" ) );

    return 0;
  }
  else
  {
    dmtcp::UniquePid child = dmtcp::UniquePid ( child_host, child_pid, child_time );

#ifdef PID_VIRTUALIZATION
    dmtcp::VirtualPidTable::Instance().insert ( child_pid, child );
#endif

    JTRACE ( "fork()ed [PARENT] done" ) ( child ) ( getenv ( "LD_PRELOAD" ) );;

    //         _dmtcp_lock();

    //rewrite socket table
    //         dmtcp::SocketTable::Instance().onForkUpdate(parent,child);

    //         _dmtcp_unlock();

    //         JTRACE("fork() done [PARENT]")(child);

    return child_pid;
  }
}

extern "C" pid_t fork()
{
  /* Acquire the wrapperExeution lock to prevent checkpoint to happen while
   * processing this system call.
   */
  WRAPPER_EXECUTION_LOCK_LOCK();

  int retVal = fork_work();

  if (retVal != 0) {
    WRAPPER_EXECUTION_LOCK_UNLOCK();
  }

  return retVal;
}


extern "C" pid_t vfork()
{
  JTRACE ( "vfork wrapper calling fork" );
  // This might not preserve the full semantics of vfork. 
  // Used for checkpointing gdb.
  return fork();
}

/* epoll is currently not supported by DMTCP */
extern "C" int epoll_create(int size)
{
  JWARNING (false) .Text("epoll is currently not supported by DMTCP.");
  errno = EPERM;
  return -1;
}

static int ptsname_r_work ( int fd, char * buf, size_t buflen )
{
  JTRACE ( "Calling ptsname_r" );
  char device[1024];
  const char *ptr;

  int rv = _real_ptsname_r ( fd, device, sizeof ( device ) );
  if ( rv != 0 )
  {
    JTRACE ( "ptsname_r failed" );
    return rv;
  }

  ptr = dmtcp::UniquePid::ptsSymlinkFilename ( device );

  if ( strlen ( ptr ) >=buflen )
  {
    JWARNING ( false ) ( ptr ) ( strlen ( ptr ) ) ( buflen )
      .Text ( "fake ptsname() too long for user buffer" );
    errno = ERANGE;
    return -1;
  }

  if ( dmtcp::PtsToSymlink::Instance().exists(device) == true )
  {
    dmtcp::string name = dmtcp::PtsToSymlink::Instance().getFilename(device);
    strcpy ( buf, name.c_str() );
    return rv;
  }

  JASSERT ( symlink ( device, ptr ) == 0 ) ( device ) ( ptr ) ( JASSERT_ERRNO )
    .Text ( "symlink() failed" );

  strcpy ( buf, ptr );

  //  dmtcp::PtyConnection::PtyType type = dmtcp::PtyConnection::PTY_MASTER;
  //  dmtcp::PtyConnection *master = new dmtcp::PtyConnection ( device, ptr, type );
  //  dmtcp::KernelDeviceToConnection::Instance().create ( fd, master );

  dmtcp::PtsToSymlink::Instance().add ( device, buf );

  return rv;
}

extern "C" char *ptsname ( int fd )
{
  /* No need to acquire Wrapper Protection lock since it will be done in ptsname_r */
  JTRACE ( "ptsname() promoted to ptsname_r()" );
  static char tmpbuf[1024];

  if ( ptsname_r ( fd, tmpbuf, sizeof ( tmpbuf ) ) != 0 )
  {
    return NULL;
  }

  return tmpbuf;
}

extern "C" int ptsname_r ( int fd, char * buf, size_t buflen )
{
  WRAPPER_EXECUTION_LOCK_LOCK();

  int retVal = ptsname_r_work(fd, buf, buflen);

  WRAPPER_EXECUTION_LOCK_UNLOCK();

  return retVal;
}

extern "C" int socketpair ( int d, int type, int protocol, int sv[2] )
{
  WRAPPER_EXECUTION_LOCK_LOCK();

  JASSERT ( sv != NULL );
  int rv = _real_socketpair ( d,type,protocol,sv );
  JTRACE ( "socketpair()" ) ( sv[0] ) ( sv[1] );

  dmtcp::TcpConnection *a, *b;

  a = new dmtcp::TcpConnection ( d, type, protocol );
  a->onConnect();
  b = new dmtcp::TcpConnection ( *a, a->id() );

  dmtcp::KernelDeviceToConnection::Instance().create ( sv[0] , a );
  dmtcp::KernelDeviceToConnection::Instance().create ( sv[1] , b );

  WRAPPER_EXECUTION_LOCK_UNLOCK();

  return rv;
}

extern "C" int pipe ( int fds[2] )
{
  JTRACE ( "promoting pipe() to socketpair()" );
  //just promote pipes to socketpairs
  return socketpair ( AF_UNIX, SOCK_STREAM, 0, fds );
}

static void dmtcpPrepareForExec()
{
  protectLD_PRELOAD();
  dmtcp::string serialFile = dmtcp::UniquePid::dmtcpTableFilename();
  jalib::JBinarySerializeWriter wr ( serialFile );
  dmtcp::UniquePid::serialize ( wr );
  dmtcp::KernelDeviceToConnection::Instance().serialize ( wr );
#ifdef PID_VIRTUALIZATION
  dmtcp::VirtualPidTable::Instance().prepareForExec();
  dmtcp::VirtualPidTable::Instance().serialize ( wr );
#endif
  setenv ( ENV_VAR_SERIALFILE_INITIAL, serialFile.c_str(), 1 );
  JTRACE ( "Prepared for Exec" );
}

static void protectLD_PRELOAD()
{
  const char* actual = getenv ( "LD_PRELOAD" );
  const char* expctd = getenv ( ENV_VAR_HIJACK_LIB );

  if ( actual!=0 && expctd!=0 ){
    JASSERT ( strcmp ( actual,expctd ) ==0 )( actual ) ( expctd )
      .Text ( "eeek! Someone stomped on LD_PRELOAD" );
  }
}

static const char* ourImportantEnvs[] =
{
  "LD_PRELOAD",
  ENV_VARS_ALL //expands to a long list
};
#define ourImportantEnvsCnt ((sizeof(ourImportantEnvs))/(sizeof(const char*)))

static bool isImportantEnv ( dmtcp::string str )
{
  for ( size_t i=0; i<str.size(); ++i )
    if ( str[i] == '=' )
    {
      str[i] = '\0';
      str = str.c_str();
      break;
    }

  for ( size_t i=0; i<ourImportantEnvsCnt; ++i )
  {
    if ( str == ourImportantEnvs[i] )
      return true;
  }
  return false;
}

static char** patchUserEnv ( char *const envp[] )
{
  static dmtcp::vector<char*> envVect;
  static dmtcp::list<dmtcp::string> strStorage;
  envVect.clear();
  strStorage.clear();

#ifdef DEBUG
  const static bool dbg = true;
#else
  const static bool dbg = false;
#endif

  JTRACE ( "patching user envp..." ) ( getenv ( "LD_PRELOAD" ) );

  //pack up our ENV into the new ENV
  for ( size_t i=0; i<ourImportantEnvsCnt; ++i )
  {
    const char* v = getenv ( ourImportantEnvs[i] );
    if ( v != NULL )
    {
      strStorage.push_back ( dmtcp::string ( ourImportantEnvs[i] ) + '=' + v );
      envVect.push_back ( &strStorage.back() [0] );
      if(dbg) JASSERT_STDERR << "     addenv[dmtcp]:" << strStorage.back() << '\n';
    }
  }

  for ( ;*envp != NULL; ++envp )
  {
    if ( isImportantEnv ( *envp ) )
    {
      if(dbg) JASSERT_STDERR << "     skipping: " << *envp << '\n';
      continue;
    }
    strStorage.push_back ( *envp );
    envVect.push_back ( &strStorage.back() [0] );
    if(dbg) JASSERT_STDERR << "     addenv[user]:" << strStorage.back() << '\n';
  }

  envVect.push_back ( NULL );

  return &envVect[0];
}

extern "C" int execve ( const char *filename, char *const argv[], char *const envp[] )
{
  //TODO: Right now we assume the user hasn't clobbered our setup of envp
  //(like LD_PRELOAD), we should really go check to make sure it hasn't
  //been destroyed....
  JTRACE ( "exec() wrapper" );
  /* Acquire the wrapperExeution lock to prevent checkpoint to happen while
   * processing this system call.
   */
  WRAPPER_EXECUTION_LOCK_LOCK();

  dmtcpPrepareForExec();
  int retVal = _real_execve ( filename, argv, patchUserEnv ( envp ) );

  WRAPPER_EXECUTION_LOCK_UNLOCK();

  return retVal;
}

extern "C" int fexecve ( int fd, char *const argv[], char *const envp[] )
{
  //TODO: Right now we assume the user hasn't clobbered our setup of envp
  //(like LD_PRELOAD), we should really go check to make sure it hasn't
  //been destroyed....
  JTRACE ( "exec() wrapper" );
  /* Acquire the wrapperExeution lock to prevent checkpoint to happen while
   * processing this system call.
   */
  WRAPPER_EXECUTION_LOCK_LOCK();

  dmtcpPrepareForExec();
  int retVal = _real_fexecve ( fd, argv, patchUserEnv ( envp ) );

  WRAPPER_EXECUTION_LOCK_UNLOCK();

  return retVal;
}

extern "C" int execv ( const char *path, char *const argv[] )
{
  JTRACE ( "exec() wrapper" );
  /* Acquire the wrapperExeution lock to prevent checkpoint to happen while
   * processing this system call.
   */
  WRAPPER_EXECUTION_LOCK_LOCK();

  dmtcpPrepareForExec();
  int retVal = _real_execv ( path, argv );

  WRAPPER_EXECUTION_LOCK_UNLOCK();

  return retVal;
}

extern "C" int execvp ( const char *file, char *const argv[] )
{
  JTRACE ( "exec() wrapper" );
  /* Acquire the wrapperExeution lock to prevent checkpoint to happen while
   * processing this system call.
   */
  WRAPPER_EXECUTION_LOCK_LOCK();

  dmtcpPrepareForExec();
  int retVal = _real_execvp ( file, argv );

  WRAPPER_EXECUTION_LOCK_UNLOCK();

  return retVal;
}

extern "C" int execl ( const char *path, const char *arg, ... )
{
  size_t argv_max = INITIAL_ARGV_MAX;
  const char *initial_argv[INITIAL_ARGV_MAX];
  const char **argv = initial_argv;
  va_list args;

  argv[0] = arg;

  va_start (args, arg);
  unsigned int i = 0;
  while (argv[i++] != NULL)
  {
    if (i == argv_max)
    {
      argv_max *= 2;
      const char **nptr = (const char**) realloc (argv == initial_argv ? NULL : argv,
          argv_max * sizeof (const char *));
      if (nptr == NULL)
      {
        if (argv != initial_argv)
          free (argv);
        return -1;
      }
      if (argv == initial_argv)
        /* We have to copy the already filled-in data ourselves.  */
        memcpy (nptr, argv, i * sizeof (const char *));

      argv = nptr;
    }

    argv[i] = va_arg (args, const char *);
  }
  va_end (args);

  int ret = execv (path, (char *const *) argv);
  if (argv != initial_argv)
    free (argv);

  return ret;
}


extern "C" int execlp ( const char *file, const char *arg, ... )
{
  size_t argv_max = INITIAL_ARGV_MAX;
  const char *initial_argv[INITIAL_ARGV_MAX];
  const char **argv = initial_argv;
  va_list args;

  argv[0] = arg;

  va_start (args, arg);
  unsigned int i = 0;
  while (argv[i++] != NULL)
  {
    if (i == argv_max)
    {
      argv_max *= 2;
      const char **nptr = (const char**) realloc (argv == initial_argv ? NULL : argv,
          argv_max * sizeof (const char *));
      if (nptr == NULL)
      {
        if (argv != initial_argv)
          free (argv);
        return -1;
      }
      if (argv == initial_argv)
        /* We have to copy the already filled-in data ourselves.  */
        memcpy (nptr, argv, i * sizeof (const char *));

      argv = nptr;
    }

    argv[i] = va_arg (args, const char *);
  }
  va_end (args);

  int ret = execvp (file, (char *const *) argv);
  if (argv != initial_argv)
    free (argv);

  return ret;
}


extern "C" int execle(const char *path, const char *arg, ...)
{
  size_t argv_max = INITIAL_ARGV_MAX;
  const char *initial_argv[INITIAL_ARGV_MAX];
  const char **argv = initial_argv;
  va_list args;
  argv[0] = arg;

  va_start (args, arg);
  unsigned int i = 0;
  while (argv[i++] != NULL)
  {
    if (i == argv_max)
    {
      argv_max *= 2;
      const char **nptr = (const char**) realloc (argv == initial_argv ? NULL : argv,
          argv_max * sizeof (const char *));
      if (nptr == NULL)
      {
        if (argv != initial_argv)
          free (argv);
        return -1;
      }
      if (argv == initial_argv)
        /* We have to copy the already filled-in data ourselves.  */
        memcpy (nptr, argv, i * sizeof (const char *));

      argv = nptr;
    }

    argv[i] = va_arg (args, const char *);
  }

  const char *const *envp = va_arg (args, const char *const *);
  va_end (args);

  int ret = execve (path, (char *const *) argv, (char *const *) envp);
  if (argv != initial_argv)
    free (argv);

  return ret;
}


/*
 * The following code including the the macros, do_system() and system() has
 * been taken from  glibc
 */
#define  SHELL_PATH  "/bin/sh"  /* Path of the shell.  */
#define  SHELL_NAME  "sh"    /* Name to give it.  */


#ifdef _LIBC_REENTRANT
static struct sigaction intr, quit;
static int sa_refcntr;
__libc_lock_define_initialized (static, lock);

# define DO_LOCK() __libc_lock_lock (lock)
# define DO_UNLOCK() __libc_lock_unlock (lock)
# define INIT_LOCK() ({ __libc_lock_init (lock); sa_refcntr = 0; })
# define ADD_REF() sa_refcntr++
# define SUB_REF() --sa_refcntr
#else
# define DO_LOCK()
# define DO_UNLOCK()
# define INIT_LOCK()
# define ADD_REF() 0
# define SUB_REF() 0
#endif


/* Execute LINE as a shell command, returning its status.  */
static int
do_system (const char *line)
{
  int status, save;
  pid_t pid;
  struct sigaction sa;
#ifndef _LIBC_REENTRANT
  struct sigaction intr, quit;
#endif
  sigset_t omask;

  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  sigemptyset (&sa.sa_mask);

  DO_LOCK ();
  if (ADD_REF () == 0)
    {
      if (sigaction (SIGINT, &sa, &intr) < 0)
  {
    SUB_REF ();
    goto out;
  }
      if (sigaction (SIGQUIT, &sa, &quit) < 0)
  {
    save = errno;
    SUB_REF ();
    goto out_restore_sigint;
  }
    }
  DO_UNLOCK ();

  /* We reuse the bitmap in the 'sa' structure.  */
  sigaddset (&sa.sa_mask, SIGCHLD);
  save = errno;
  if (sigprocmask (SIG_BLOCK, &sa.sa_mask, &omask) < 0)
    {
#ifndef _LIBC
      if (errno == ENOSYS)
  errno = save;
      else
#endif
  {
    DO_LOCK ();
    if (SUB_REF () == 0)
      {
        save = errno;
        (void) sigaction (SIGQUIT, &quit, (struct sigaction *) NULL);
      out_restore_sigint:
        (void) sigaction (SIGINT, &intr, (struct sigaction *) NULL);
        errno = save;
        //set_errno (save);
      }
  out:
    DO_UNLOCK ();
    return -1;
  }
    }

#ifdef CLEANUP_HANDLER
  CLEANUP_HANDLER;
#endif

#ifdef FORK
  pid = fork ();
#else
  pid = fork ();
#endif
  if (pid == (pid_t) 0)
    {
      /* Child side.  */
      const char *new_argv[4];
      new_argv[0] = SHELL_NAME;
      new_argv[1] = "-c";
      new_argv[2] = line;
      new_argv[3] = NULL;

      /* Restore the signals.  */
      (void) sigaction (SIGINT, &intr, (struct sigaction *) NULL);
      (void) sigaction (SIGQUIT, &quit, (struct sigaction *) NULL);
      (void) sigprocmask (SIG_SETMASK, &omask, (sigset_t *) NULL);
      INIT_LOCK ();

      /* Exec the shell.  */
      (void) execve (SHELL_PATH, (char *const *) new_argv, __environ);
      _exit (127);
    }
  else if (pid < (pid_t) 0)
    /* The fork failed.  */
    status = -1;
  else
    /* Parent side.  */
  {
    /* Note the system() is a cancellation point.  But since we call
       waitpid() which itself is a cancellation point we do not
       have to do anything here.  */
    do {
      if (TEMP_FAILURE_RETRY (waitpid (pid, &status, 0)) != pid)
        status = -1;
    }
    while (WIFEXITED(status) == 0);
  }

#ifdef CLEANUP_HANDLER
  CLEANUP_RESET;
#endif

  save = errno;
  DO_LOCK ();
  if ((SUB_REF () == 0
       && (sigaction (SIGINT, &intr, (struct sigaction *) NULL)
     | sigaction (SIGQUIT, &quit, (struct sigaction *) NULL)) != 0)
      || sigprocmask (SIG_SETMASK, &omask, (sigset_t *) NULL) != 0)
    {
#ifndef _LIBC
      /* glibc cannot be used on systems without waitpid.  */
      if (errno == ENOSYS)
        errno = save;
      else
#endif
  status = -1;
    }
  DO_UNLOCK ();

  return status;
}

extern "C" int
system (const char *line)
{
  JTRACE ( "before system(), checkpointing may not work" )
    ( line ) ( getenv ( ENV_VAR_HIJACK_LIB ) ) ( getenv ( "LD_PRELOAD" ) );
  protectLD_PRELOAD();

  if (line == NULL)
    /* Check that we have a command processor available.  It might
       not be available after a chroot(), for example.  */
    return do_system ("exit 0") == 0;

  int result = do_system (line);

  JTRACE ( "after system()" );

  return result;
}
