/****************************************************************************
 *   Copyright (C) 2006-2010 by Jason Ansel, Kapil Arya, and Gene Cooperman *
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

#include <stdlib.h>
#include <string.h>
#include <string>
#include <sstream>
#include <fcntl.h>
#include <sys/syscall.h>
#include "constants.h"
#include "util.h"
#include "pidwrappers.h"
#include "../jalib/jconvert.h"
#include "../jalib/jfilesystem.h"
#include "virtualpidtable.h"
#include "dmtcpplugin.h"

#define INITIAL_VIRTUAL_TID 1
#define MAX_VIRTUAL_TID 999
static int _numTids = 1;

dmtcp::VirtualPidTable::VirtualPidTable()
  : VirtualIdTable<pid_t> ("Pid")
{
  //_do_lock_tbl();
  //_idMapTable[getpid()] = _real_getpid();
  //_idMapTable[getppid()] = _real_getppid();
  //_do_unlock_tbl();
}

static dmtcp::VirtualPidTable *virtPidTableInst = NULL;
dmtcp::VirtualPidTable& dmtcp::VirtualPidTable::instance()
{
  if (virtPidTableInst == NULL) {
    virtPidTableInst = new VirtualPidTable();
  }
  return *virtPidTableInst;
}

void dmtcp::VirtualPidTable::postRestart()
{
  VirtualIdTable<pid_t>::postRestart();
  _do_lock_tbl();
  _idMapTable[getpid()] = _real_getpid();
  _do_unlock_tbl();
}

void dmtcp::VirtualPidTable::refresh()
{
  pid_t pid = getpid();
  id_iterator i;
  id_iterator next;
  pid_t _real_pid = _real_getpid();

  _do_lock_tbl();
  for (i = _idMapTable.begin(), next = i; i != _idMapTable.end(); i = next) {
    next++;
    if (isIdCreatedByCurrentProcess(i->second)
        && _real_tgkill(_real_pid, i->second, 0) == -1) {
      _idMapTable.erase(i);
    }
  }
  _do_unlock_tbl();
  printMaps();
}

pid_t dmtcp::VirtualPidTable::getNewVirtualTid()
{
  pid_t tid = VirtualIdTable<pid_t>::getNewVirtualId();

  if (tid == -1) {
    refresh();
  }

  tid = VirtualIdTable<pid_t>::getNewVirtualId();

  JASSERT(tid != -1) (_idMapTable.size())
    .Text("Exceeded maximum number of threads allowed");

  return tid;
}

void dmtcp::VirtualPidTable::resetOnFork()
{
  VirtualIdTable<pid_t>::resetOnFork();
  _numTids = 1;
  _idMapTable[getpid()] = _real_getpid();
  refresh();
  printMaps();
}

//to allow linking without ptrace plugin
extern "C" int dmtcp_is_ptracing() __attribute__ ((weak));
pid_t dmtcp::VirtualPidTable::realToVirtual(pid_t realPid)
{
  if (realIdExists(realPid)) {
    return VirtualIdTable<pid_t>::realToVirtual(realPid);
  }

  _do_lock_tbl();
  if (dmtcp_is_ptracing != 0 && dmtcp_is_ptracing()) {
    pid_t virtualPid = readVirtualTidFromFileForPtrace(gettid());
    if (virtualPid != -1) {
      _do_unlock_tbl();
      updateMapping(virtualPid, realPid);
      return virtualPid;
    }
  }

  //JWARNING(false) (realPid)
    //.Text("No virtual pid/tid found for the given real pid");
  _do_unlock_tbl();
  return realPid;
}

void dmtcp::VirtualPidTable::writeVirtualTidToFileForPtrace(pid_t pid)
{
  if (!dmtcp_is_ptracing || !dmtcp_is_ptracing()) {
    return;
  }
  pid_t tracerPid = dmtcp::Util::getTracerPid();
  if (tracerPid != 0) {
    dmtcp::ostringstream o;
    char buf[80];
    o << dmtcp_get_tmpdir() << "/virtualPidOfNewlyCreatedThread_"
      << dmtcp_get_computation_id_str() << "_" << tracerPid;

    sprintf(buf, "%d", pid);
    int fd = open(o.str().c_str(), O_CREAT|O_WRONLY|O_TRUNC, 0600);
    JASSERT(fd >= 0) (o.str()) (JASSERT_ERRNO);
    dmtcp::Util::writeAll(fd, buf, strlen(buf) + 1);
    JTRACE("Writing virtual Pid/Tid to file") (pid) (o.str());
    close(fd);
  }
}

pid_t dmtcp::VirtualPidTable::readVirtualTidFromFileForPtrace(pid_t tid)
{
  dmtcp::ostringstream o;
  char buf[80];
  pid_t pid;
  int fd;
  ssize_t bytesRead;

  if (!dmtcp_is_ptracing || !dmtcp_is_ptracing()) {
    return -1;
  }
  if (tid == -1) {
    tid = dmtcp::Util::getTracerPid();
    if (tid == 0) {
      return -1;
    }
  }

  o << dmtcp::UniquePid::getTmpDir() << "/virtualPidOfNewlyCreatedThread_"
    << dmtcp::UniquePid::ComputationId() << "_" << tid;

  fd = _real_open(o.str().c_str(), O_RDONLY, 0);
  if (fd < 0) {
    return -1;
  }
  bytesRead = dmtcp::Util::readAll(fd, buf, sizeof(buf));
  close(fd);
  unlink(o.str().c_str());

  if (bytesRead <= 0) {
    return -1;
  }

  sscanf(buf, "%d", &pid);
  JTRACE("Read virtual Pid/Tid from file") (pid) (o.str());
  return pid;
}
