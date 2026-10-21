// Socket helpers (IPv4). All sends use MSG_NOSIGNAL; no SIGPIPE handling needed.
#pragma once

#include <netinet/in.h>
#include <sys/uio.h>

#include <atomic>
#include <cstdint>
#include <string>

namespace owc::net {

// TCP send buffer for the client socket. Requested 64 KiB; Linux doubles it
// (~128 KiB effective). At 8 Mbps that is <= ~130 ms of in-kernel backlog in
// the worst case, while still > the LAN bandwidth-delay product (e.g. 50 Mbps
// x 10 ms RTT = 62 KB). Anything that does not fit stays in OUR bounded queue,
// where the latency policy can drop it. Setting it disables autotuning, which
// is exactly what we want (autotuning grows to MBs = seconds of lag on Wi-Fi).
constexpr int kTcpSndBuf = 64 * 1024;
// UDP: big enough to absorb a keyframe burst (sendto blocks when full).
constexpr int kUdpSndBuf = 1024 * 1024;
constexpr int kDscpEfTos = 0xB8;  // DSCP EF (46) << 2

// A socket shared by the net thread (reader) and the sender thread (writer).
// shutdown() is safe from any thread; close happens in the destructor (last ref).
class Socket {
 public:
  explicit Socket(int fd) : fd_(fd) {}
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  int fd() const { return fd_; }
  void shutdownBoth();
  bool isShutdown() const { return shut_.load(); }

 private:
  int fd_;
  std::atomic<bool> shut_{false};
};

void closeFd(int& fd);
int createTcpListener(uint16_t port, std::string& err);
int createUdpSocket(uint16_t port, bool broadcast, std::string& err);
// TCP_NODELAY, SO_SNDBUF, keepalive (5s idle, 2s x 3), TCP_USER_TIMEOUT 8s.
void tuneTcpClient(int fd);
void setDscpEf(int fd);
std::string addrToString(const sockaddr_in& a);

enum class SendResult { Ok, Error, Timeout, Aborted };
// Writes all iov bytes using non-blocking sendmsg + poll(POLLOUT). Gives up
// after `stallMs` without progress, or when *abort becomes true (checked
// every <=50 ms).
SendResult sendAll(int fd, iovec* iov, int iovcnt, int stallMs, const std::atomic<bool>* abort);

}  // namespace owc::net
