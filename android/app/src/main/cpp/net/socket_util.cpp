#include "net/socket_util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "util/clock.h"

namespace owc::net {

Socket::~Socket() {
  if (fd_ >= 0) ::close(fd_);
}

void Socket::shutdownBoth() {
  if (!shut_.exchange(true) && fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
}

void closeFd(int& fd) {
  if (fd >= 0) ::close(fd);
  fd = -1;
}

int createTcpListener(uint16_t port, std::string& err) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    err = std::string("socket: ") + strerror(errno);
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd, 4) != 0) {
    err = "bind/listen tcp " + std::to_string(port) + ": " + strerror(errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

int createUdpSocket(uint16_t port, bool broadcast, std::string& err) {
  int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    err = std::string("socket: ") + strerror(errno);
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  if (broadcast) setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  a.sin_port = htons(port);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    err = "bind udp " + std::to_string(port) + ": " + strerror(errno);
    ::close(fd);
    return -1;
  }
  return fd;
}

void tuneTcpClient(int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  int snd = kTcpSndBuf;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  int idle = 5, intvl = 2, cnt = 3;
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
  setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
  unsigned int userTimeout = 8000;  // ms of unacked data before the kernel aborts
  setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &userTimeout, sizeof(userTimeout));
}

void setDscpEf(int fd) {
  int tos = kDscpEfTos;
  setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
}

std::string addrToString(const sockaddr_in& a) {
  char ip[INET_ADDRSTRLEN] = {0};
  inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
  return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

SendResult sendAll(int fd, iovec* iov, int iovcnt, int stallMs,
                   const std::atomic<bool>* abort) {
  int64_t lastProgress = monoMs();
  while (iovcnt > 0) {
    if (iov->iov_len == 0) {
      ++iov;
      --iovcnt;
      continue;
    }
    if (abort && abort->load(std::memory_order_relaxed)) return SendResult::Aborted;
    msghdr mh{};
    mh.msg_iov = iov;
    mh.msg_iovlen = static_cast<size_t>(iovcnt);
    ssize_t n = ::sendmsg(fd, &mh, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n > 0) {
      lastProgress = monoMs();
      size_t left = static_cast<size_t>(n);
      while (iovcnt > 0 && left >= iov->iov_len) {
        left -= iov->iov_len;
        ++iov;
        --iovcnt;
      }
      if (iovcnt > 0 && left) {
        iov->iov_base = static_cast<uint8_t*>(iov->iov_base) + left;
        iov->iov_len -= left;
      }
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) return SendResult::Error;
    if (monoMs() - lastProgress > stallMs) return SendResult::Timeout;
    pollfd p{fd, POLLOUT, 0};
    int r = ::poll(&p, 1, 50);
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return SendResult::Error;
  }
  return SendResult::Ok;
}

}  // namespace owc::net
