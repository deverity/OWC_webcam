#include "net/net_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "util/clock.h"
#include "util/log.h"
#include "util/thread.h"

namespace owc::net {

NetServer::~NetServer() { stop(); }

bool NetServer::start(uint16_t controlPort, uint16_t udpPort, uint16_t discoveryPort,
                      uint16_t announcePort, Callbacks* cb, std::string& err) {
  cb_ = cb;
  controlPort_ = controlPort;
  udpPort_ = udpPort;
  announcePort_ = announcePort;
  listenFd_ = createTcpListener(controlPort, err);
  if (listenFd_ < 0) return false;
  udpFd_ = createUdpSocket(udpPort, false, err);
  if (udpFd_ < 0) {
    closeFd(listenFd_);
    return false;
  }
  int snd = kUdpSndBuf;
  setsockopt(udpFd_, SOL_SOCKET, SO_SNDBUF, &snd, sizeof(snd));
  setDscpEf(udpFd_);
  discFd_ = createUdpSocket(discoveryPort, true, err);
  if (discFd_ < 0) {
    // Discovery is a convenience; the server still works without it.
    LOGW("discovery disabled: %s", err.c_str());
    err.clear();
  }
  if (announcePort_) {
    std::string aerr;
    announceFd_ = createUdpSocket(0, true, aerr);  // ephemeral port, SO_BROADCAST
    if (announceFd_ < 0) LOGW("announce disabled: %s", aerr.c_str());
  }
  nextAnnounceMs_ = 0;
  wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  rxBuf_.resize(64 * 1024);
  stop_ = false;
  thread_ = std::thread(&NetServer::loop, this);
  LOGI("listening: tcp %u, udp %u, discovery %u", controlPort, udpPort, discoveryPort);
  return true;
}

void NetServer::stop() {
  stop_ = true;
  if (wakeFd_ >= 0) {
    uint64_t one = 1;
    ssize_t r = ::write(wakeFd_, &one, sizeof(one));
    (void)r;
  }
  if (thread_.joinable()) thread_.join();
  // A client still attached at shutdown is handed back to the worker's
  // shutdown path through onClientDisconnected() inside loop() exit.
  closeFd(listenFd_);
  closeFd(udpFd_);
  closeFd(discFd_);
  closeFd(announceFd_);
  closeFd(wakeFd_);
}

void NetServer::loop() {
  setThreadName("owc-net");
  while (!stop_) {
    pollfd fds[5];
    int n = 0;
    fds[n++] = {wakeFd_, POLLIN, 0};
    fds[n++] = {listenFd_, POLLIN, 0};
    fds[n++] = {udpFd_, POLLIN, 0};
    const int discIdx = discFd_ >= 0 ? n : -1;
    if (discFd_ >= 0) fds[n++] = {discFd_, POLLIN, 0};
    const int cliIdx = client_ ? n : -1;
    if (client_) fds[n++] = {client_->sock->fd(), POLLIN, 0};
    int timeoutMs = 1000;
    if (announceFd_ >= 0)
      timeoutMs = static_cast<int>(std::clamp<int64_t>(nextAnnounceMs_ - monoMs(), 0, 1000));
    int r = ::poll(fds, static_cast<nfds_t>(n), timeoutMs);
    if (r < 0) {
      if (errno == EINTR) continue;
      LOGE_LOCAL("poll: %s", strerror(errno));
      break;
    }
    if (stop_) break;
    if (fds[0].revents) {
      uint64_t v;
      ssize_t rr = ::read(wakeFd_, &v, sizeof(v));
      (void)rr;
    }
    if (cliIdx >= 0 && fds[cliIdx].revents) readClient();
    if (fds[1].revents & POLLIN) acceptClient();
    if (fds[2].revents & POLLIN) readUdpVideo();
    if (discIdx >= 0 && (fds[discIdx].revents & POLLIN)) readDiscovery();
    if (announceFd_ >= 0 && monoMs() >= nextAnnounceMs_) {
      sendAnnounce();
      nextAnnounceMs_ = monoMs() + (client_ ? 5000 : 2000);
    }
  }
  if (client_) dropClient("server stopping");
}

void NetServer::acceptClient() {
  sockaddr_in a{};
  socklen_t al = sizeof(a);
  int fd = ::accept4(listenFd_, reinterpret_cast<sockaddr*>(&a), &al, SOCK_CLOEXEC);
  if (fd < 0) return;
  if (client_) {
    HelloAck busy = cb_->busyAck();
    busy.status = HelloStatus::Busy;
    std::vector<uint8_t> out;
    appendTcpMessage(out, MsgType::HelloAck, encode(busy));
    ssize_t w = ::send(fd, out.data(), out.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    (void)w;
    ::shutdown(fd, SHUT_WR);
    ::close(fd);
    LOGI("rejected %s: busy (active client %s)", addrToString(a).c_str(),
         client_->peerStr.c_str());
    return;
  }
  tuneTcpClient(fd);
  auto c = std::make_shared<ClientConn>();
  c->gen = nextGen_++;
  c->sock = std::make_shared<Socket>(fd);
  c->peer = a;
  c->peerStr = addrToString(a);
  c->lastRxMs = monoMs();
  c->sender = std::make_shared<Sender>(c->sock, udpFd_);
  c->sender->start();
  parser_ = TcpFrameParser();
  client_ = c;
  LOGI("client connected: %s (gen %llu)", c->peerStr.c_str(),
       static_cast<unsigned long long>(c->gen));
  cb_->onClientConnected(std::move(c));
}

void NetServer::dropClient(const char* why) {
  if (!client_) return;
  LOGI("client %s disconnected: %s", client_->peerStr.c_str(), why);
  client_->sock->shutdownBoth();
  const uint64_t gen = client_->gen;
  client_.reset();
  parser_ = TcpFrameParser();
  cb_->onClientDisconnected(gen);
}

void NetServer::readClient() {
  for (int iter = 0; iter < 16; ++iter) {
    ssize_t n = ::recv(client_->sock->fd(), rxBuf_.data(), rxBuf_.size(), MSG_DONTWAIT);
    if (n == 0) {
      dropClient("eof");
      return;
    }
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      dropClient(strerror(errno));
      return;
    }
    const int64_t rxUs = monoUs();
    client_->lastRxMs = rxUs / 1000;
    if (!parser_.feed(rxBuf_.data(), static_cast<size_t>(n))) {
      dropClient("protocol error (oversized message)");
      return;
    }
    TcpFrameParser::Message m;
    while (parser_.next(m)) {
      if (m.type == MsgType::Ping) {
        Ping p;
        if (decode(m.payload.data(), m.payload.size(), p)) client_->sender->pushPong(p.t0_us, rxUs);
        continue;
      }
      cb_->onClientMessage(client_->gen, m.type, std::move(m.payload));
      m.payload.clear();
    }
    if (static_cast<size_t>(n) < rxBuf_.size()) return;
  }
}

void NetServer::readUdpVideo() {
  uint8_t buf[kUdpMaxDatagram];
  for (;;) {
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t n = ::recvfrom(udpFd_, buf, sizeof(buf), MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 0) return;
    UdpHeader h;
    if (!readUdpHeader(buf, static_cast<size_t>(n), h) || h.type != UdpType::Punch) continue;
    if (!client_) continue;
    const uint32_t sid = client_->sessionId.load();
    if (sid == 0 || h.session_id != sid) {
      LOGD("punch with wrong session id %08x from %s", h.session_id, addrToString(from).c_str());
      continue;
    }
    client_->sender->setUdpTarget(from, sid);
    UdpHeader ack;
    ack.type = UdpType::PunchAck;
    ack.session_id = sid;
    uint8_t out[kUdpHeaderSize];
    writeUdpHeader(ack, out);
    ::sendto(udpFd_, out, sizeof(out), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from),
             sizeof(from));
    cb_->onPunch(client_->gen, from);
  }
}

// Interfaces whose directed broadcast we use: Wi-Fi, USB tethering, Ethernet,
// hotspot. Cellular / tunnels / VPN are skipped.
static bool announceInterface(const char* name) {
  static const char* kSkip[] = {"rmnet", "rev_rmnet", "ccmni", "wwan", "clat", "dummy", "tun", "ppp",
                                "ipsec", "v4-"};
  for (const char* p : kSkip)
    if (strncmp(name, p, strlen(p)) == 0) return false;
  return true;
}

void NetServer::sendAnnounce() {
  DiscoveryReply rep = cb_->discoveryInfo();
  rep.control_port = controlPort_;
  rep.udp_port = udpPort_;
  rep.busy = client_ != nullptr;
  rep.announce = true;
  const std::vector<uint8_t> out = encode(rep);

  std::vector<in_addr_t> targets{htonl(INADDR_BROADCAST)};
  ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) == 0) {
    for (ifaddrs* i = ifs; i; i = i->ifa_next) {
      if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_broadaddr) continue;
      if (!(i->ifa_flags & IFF_UP) || !(i->ifa_flags & IFF_BROADCAST) || (i->ifa_flags & IFF_LOOPBACK)) continue;
      if (!announceInterface(i->ifa_name)) continue;
      const in_addr_t b = reinterpret_cast<sockaddr_in*>(i->ifa_broadaddr)->sin_addr.s_addr;
      if (b != 0 && std::find(targets.begin(), targets.end(), b) == targets.end()) targets.push_back(b);
    }
    freeifaddrs(ifs);
  }
  for (in_addr_t t : targets) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(announcePort_);
    a.sin_addr.s_addr = t;
    if (::sendto(announceFd_, out.data(), out.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&a),
                 sizeof(a)) < 0) {
      // ENETUNREACH when no network is up: normal, keep quiet after a few.
      if (++announceErrors_ <= 3) LOGD("announce to %s: %s", addrToString(a).c_str(), strerror(errno));
    }
  }
}

void NetServer::readDiscovery() {
  uint8_t buf[512];
  for (;;) {
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    ssize_t n = ::recvfrom(discFd_, buf, sizeof(buf), MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&from), &fl);
    if (n < 0) return;
    if (!isDiscoveryRequest(buf, static_cast<size_t>(n))) continue;
    DiscoveryReply rep = cb_->discoveryInfo();
    rep.control_port = controlPort_;
    rep.udp_port = udpPort_;
    rep.busy = client_ != nullptr;
    std::vector<uint8_t> out = encode(rep);
    ::sendto(discFd_, out.data(), out.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&from),
             sizeof(from));
  }
}

}  // namespace owc::net
