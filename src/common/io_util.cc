/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 */

#include "io_util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <fmt/format.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/poll.h>

#ifdef __linux__
#include <sys/sendfile.h>
#endif

#include "event_util.h"
#include "fd_util.h"
#include "scope_exit.h"

#ifndef POLLIN
#define POLLIN 0x0001   /* There is data to read */
#define POLLPRI 0x0002  /* There is urgent data to read */
#define POLLOUT 0x0004  /* Writing now will not block */
#define POLLERR 0x0008  /* Error condition */
#define POLLHUP 0x0010  /* Hung up */
#define POLLNVAL 0x0020 /* Invalid request: fd not open */
#endif

#define AE_READABLE 1  // NOLINT
#define AE_WRITABLE 2  // NOLINT
#define AE_ERROR 4     // NOLINT
#define AE_HUP 8       // NOLINT

namespace Util {
Status SockConnect(const std::string &host, uint32_t port, int *fd) {
  addrinfo hints, *servinfo = nullptr, *p = nullptr;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  if (int rv = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &servinfo); rv != 0) {
    return {Status::NotOK, gai_strerror(rv)};
  }

  auto exit = MakeScopeExit([servinfo] { freeaddrinfo(servinfo); });

  for (p = servinfo; p != nullptr; p = p->ai_next) {
    auto cfd = UniqueFD(socket(p->ai_family, p->ai_socktype, p->ai_protocol));
    if (!cfd) continue;
    if (connect(*cfd, p->ai_addr, p->ai_addrlen) == -1) {
      continue;
    }
    Status s = SockSetTcpKeepalive(*cfd, 120);
    if (s.IsOK()) {
      s = SockSetTcpNoDelay(*cfd, 1);
    }
    if (!s.IsOK()) {
      continue;
    }

    *fd = cfd.Release();
    return Status::OK();
  }

  return Status::FromErrno();
}

Status SockSetTcpNoDelay(int fd, int val) {
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1) {
    return Status::FromErrno();
  }
  return Status::OK();
}

Status SockSetTcpKeepalive(int fd, int interval) {
  int val = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1) {
    return Status::FromErrno();
  }

#ifdef __linux__
  // Default settings are more or less garbage, with the keepalive time
  // set to 7200 by default on Linux. Modify settings to make the feature
  // actually useful.

  // Send first probe after interval.
  val = interval;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
    return {Status::NotOK, fmt::format("setsockopt TCP_KEEPIDLE: {}", strerror(errno))};
  }

  // Send next probes after the specified interval. Note that we set the
  // delay as interval / 3, as we send three probes before detecting
  // an error (see the next setsockopt call).
  val = interval / 3;
  if (val == 0) val = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
    return {Status::NotOK, fmt::format("setsockopt TCP_KEEPINTVL: {}", strerror(errno))};
  }

  // Consider the socket in error state after three we send three ACK
  // probes without getting a reply.
  val = 3;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
    return {Status::NotOK, fmt::format("setsockopt TCP_KEEPCNT: {}", strerror(errno))};
  }
#else
  ((void)interval);  // Avoid unused var warning for non Linux systems.
#endif

  return Status::OK();
}

Status SockConnect(const std::string &host, uint32_t port, int *fd, uint64_t conn_timeout, uint64_t timeout) {
  if (conn_timeout == 0) {
    auto s = SockConnect(host, port, fd);
    if (!s) return s;
  } else {
    *fd = socket(AF_INET, SOCK_STREAM, 0);
    if (*fd == NullFD) return Status::FromErrno();
  }

  auto exit = MakeScopeExit([fd] {
    close(*fd);
    *fd = NullFD;
  });

  if (conn_timeout != 0) {
    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(host.c_str());
    sin.sin_port = htons(port);

    fcntl(*fd, F_SETFL, O_NONBLOCK);
    if (connect(*fd, reinterpret_cast<sockaddr *>(&sin), sizeof(sin))) {
      return Status::FromErrno();
    }

    auto retmask = Util::aeWait(*fd, AE_WRITABLE, conn_timeout);
    if ((retmask & AE_WRITABLE) == 0 || (retmask & AE_ERROR) != 0 || (retmask & AE_HUP) != 0) {
      return Status::FromErrno();
    }

    int socket_arg = 0;
    // Set to blocking mode again...
    if ((socket_arg = fcntl(*fd, F_GETFL, NULL)) < 0) {
      return Status::FromErrno();
    }
    socket_arg &= (~O_NONBLOCK);
    if (fcntl(*fd, F_SETFL, socket_arg) < 0) {
      return Status::FromErrno();
    }
    auto s = SockSetTcpNoDelay(*fd, 1);
    if (!s) return s;
  }

  if (timeout > 0) {
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    if (setsockopt(*fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char *>(&tv), sizeof(tv)) < 0) {
      return Status(Status::NotOK, std::string("setsockopt failed: ") + strerror(errno));
    }
  }

  exit.Disable();
  return Status::OK();
}

// NOTE: fd should be blocking here
Status SockSend(int fd, const std::string &data) { return Write(fd, data); }

// Implements SockSendFileCore to transfer data between file descriptors and
// avoid transferring data to and from user space.
//
// The function prototype is just like sendfile(2) on Linux. in_fd is a file
// descriptor opened for reading and out_fd is a descriptor opened for writing.
// offset specifies where to start reading data from in_fd. count is the number
// of bytes to copy between the file descriptors.
//
// The return value is the number of bytes written to out_fd, if the transfer
// was successful. On error, -1 is returned, and errno is set appropriately.
ssize_t SockSendFileCore(int out_fd, int in_fd, off_t offset, size_t count) {
#if defined(__linux__)
  return sendfile(out_fd, in_fd, &offset, count);

#elif defined(__APPLE__)
  off_t len = count;
  if (sendfile(in_fd, out_fd, offset, &len, NULL, 0) == -1)
    return -1;
  else
    return (ssize_t)len;

#endif
  errno = ENOSYS;
  return -1;
}

// Send file by sendfile actually according to different operation systems,
// please note that, the out socket fd should be in blocking mode.
Status SockSendFile(int out_fd, int in_fd, size_t size) {
  ssize_t nwritten = 0;
  off_t offset = 0;
  while (size != 0) {
    size_t n = size <= 16 * 1024 ? size : 16 * 1024;
    nwritten = SockSendFileCore(out_fd, in_fd, offset, n);
    if (nwritten == -1) {
      if (errno == EINTR)
        continue;
      else
        return Status(Status::NotOK, strerror(errno));
    }
    size -= nwritten;
    offset += nwritten;
  }
  return Status::OK();
}

Status SockSetBlocking(int fd, int blocking) {
  int flags = 0;
  // Old flags
  if ((flags = fcntl(fd, F_GETFL)) == -1) {
    return Status(Status::NotOK, std::string("fcntl(F_GETFL): ") + strerror(errno));
  }

  // New flags
  if (blocking)
    flags &= ~O_NONBLOCK;
  else
    flags |= O_NONBLOCK;

  if (fcntl(fd, F_SETFL, flags) == -1) {
    return Status(Status::NotOK, std::string("fcntl(F_SETFL,O_BLOCK): ") + strerror(errno));
  }
  return Status::OK();
}

Status SockReadLine(int fd, std::string *data) {
  UniqueEvbuf evbuf;
  if (evbuffer_read(evbuf.get(), fd, -1) <= 0) {
    return Status(Status::NotOK, std::string("read response err: ") + strerror(errno));
  }
  UniqueEvbufReadln line(evbuf.get(), EVBUFFER_EOL_CRLF_STRICT);
  if (!line) {
    return Status(Status::NotOK, std::string("read response err(empty): ") + strerror(errno));
  }
  *data = std::string(line.get(), line.length);
  return Status::OK();
}

int GetPeerAddr(int fd, std::string *addr, uint32_t *port) {
  addr->clear();

  sockaddr_storage sa{};
  socklen_t sa_len = sizeof(sa);
  if (getpeername(fd, reinterpret_cast<sockaddr *>(&sa), &sa_len) < 0) {
    return -1;
  }
  if (sa.ss_family == AF_INET6) {
    char buf[INET6_ADDRSTRLEN];
    auto sa6 = reinterpret_cast<sockaddr_in6 *>(&sa);
    inet_ntop(AF_INET6, reinterpret_cast<void *>(&sa6->sin6_addr), buf, INET_ADDRSTRLEN);
    addr->append(buf);
    *port = ntohs(sa6->sin6_port);
  } else {
    auto sa4 = reinterpret_cast<sockaddr_in *>(&sa);
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, reinterpret_cast<void *>(&sa4->sin_addr), buf, INET_ADDRSTRLEN);
    addr->append(buf);
    *port = ntohs(sa4->sin_port);
  }
  return 0;
}

int GetLocalPort(int fd) {
  sockaddr_in6 address;
  socklen_t len = sizeof(address);
  if (getsockname(fd, (struct sockaddr *)&address, &len) == -1) {
    return 0;
  }

  if (address.sin6_family == AF_INET) {
    return ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
  } else if (address.sin6_family == AF_INET6) {
    return ntohs(address.sin6_port);
  }

  return 0;
}

bool IsPortInUse(int port) {
  int fd = NullFD;
  Status s = SockConnect("0.0.0.0", static_cast<uint32_t>(port), &fd);
  if (fd != NullFD) close(fd);
  return s.IsOK();
}

/* Wait for milliseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(int fd, int mask, uint64_t timeout) {
  pollfd pfd;
  int retmask = 0, retval = 0;

  memset(&pfd, 0, sizeof(pfd));
  pfd.fd = fd;
  if (mask & AE_READABLE) pfd.events |= POLLIN;
  if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

  if ((retval = poll(&pfd, 1, timeout)) == 1) {
    if (pfd.revents & POLLIN) retmask |= AE_READABLE;
    if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
    if (pfd.revents & POLLERR) retmask |= AE_ERROR;
    if (pfd.revents & POLLHUP) retmask |= AE_HUP;
    return retmask;
  } else {
    return retval;
  }
}

template <auto syscall, typename... Args>
Status WriteImpl(int fd, std::string_view data, Args &&...args) {
  ssize_t n = 0;
  while (n < static_cast<ssize_t>(data.size())) {
    ssize_t nwritten = syscall(fd, data.data() + n, data.size() - n, std::forward<Args>(args)...);
    if (nwritten == -1) {
      return Status::FromErrno();
    }
    n += nwritten;
  }
  return Status::OK();
}

Status Write(int fd, const std::string &data) { return WriteImpl<write>(fd, data); }

Status Pwrite(int fd, const std::string &data, off_t offset) { return WriteImpl<pwrite>(fd, data, offset); }

}  // namespace Util
