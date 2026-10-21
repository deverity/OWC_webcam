#include <owc/discovery.h>

#include "util.h"

#include <iphlpapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace owc {

using namespace detail;

std::vector<std::string> Discovery::broadcastAddresses() {
  std::vector<std::string> out;
  ULONG size = 16 * 1024;
  std::vector<uint8_t> buf;
  ULONG rc = ERROR_BUFFER_OVERFLOW;
  for (int i = 0; i < 4 && rc == ERROR_BUFFER_OVERFLOW; ++i) {
    buf.resize(size);
    rc = GetAdaptersAddresses(AF_INET,
                              GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                  GAA_FLAG_SKIP_DNS_SERVER,
                              nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
  }
  if (rc != NO_ERROR) return out;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
      const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      const uint32_t ip = ntohl(sin->sin_addr.s_addr);
      const uint8_t plen = u->OnLinkPrefixLength;
      if (plen == 0 || plen >= 31) continue;
      if ((ip >> 16) == 0xA9FE) continue;  // 169.254/16 link-local (no DHCP)
      const uint32_t mask = plen >= 32 ? 0xFFFFFFFFu : ~((1u << (32 - plen)) - 1u);
      sockaddr_in b{};
      b.sin_family = AF_INET;
      b.sin_addr.s_addr = htonl((ip & mask) | ~mask);
      std::string s = addrToString(b, false);
      if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
    }
  }
  return out;
}

namespace {

SOCKET openSocket(const Discovery::Options& o, std::string& err) {
  if (!winsockInit()) { err = "WSAStartup failed"; return INVALID_SOCKET; }
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) { err = "socket() failed"; return s; }
  BOOL on = TRUE;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
  sockaddr_in a{};
  if (!resolveIPv4(o.bind_address.empty() ? "0.0.0.0" : o.bind_address, 0, a)) {
    err = "bad bind address";
    closesocket(s);
    return INVALID_SOCKET;
  }
  if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    err = "bind failed " + std::to_string(WSAGetLastError());
    closesocket(s);
    return INVALID_SOCKET;
  }
  DWORD tmo = 100;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
  return s;
}

void sendRequests(SOCKET s, const Discovery::Options& o) {
  const std::vector<uint8_t> req = encodeDiscoveryRequest();
  std::vector<std::string> targets;
  if (o.broadcast) {
    targets.push_back("255.255.255.255");
    for (auto& b : Discovery::broadcastAddresses()) targets.push_back(b);
  }
  for (auto& t : o.extra_targets) targets.push_back(t);
  for (auto& t : targets) {
    sockaddr_in to{};
    if (!resolveIPv4(t, o.port, to)) continue;
    sendto(s, reinterpret_cast<const char*>(req.data()), static_cast<int>(req.size()), 0,
           reinterpret_cast<sockaddr*>(&to), sizeof(to));
  }
}

// Receives one reply (up to SO_RCVTIMEO). Returns true and fills d.
bool recvReply(SOCKET s, DiscoveredDevice& d) {
  uint8_t buf[2048];
  sockaddr_in from{};
  int fl = sizeof(from);
  int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                   reinterpret_cast<sockaddr*>(&from), &fl);
  if (n <= 0) return false;
  DiscoveryReply r;
  if (!decode(buf, static_cast<size_t>(n), r)) return false;
  d.ip = addrToString(from, false);
  d.control_port = r.control_port;
  d.udp_port = r.udp_port;
  d.name = r.device_name;
  d.model = r.model;
  d.busy = r.busy;
  d.last_seen_ms = d.first_seen_ms = steadyMs();
  return true;
}

void sortDevices(std::vector<DiscoveredDevice>& v) {
  std::sort(v.begin(), v.end(), [](const DiscoveredDevice& a, const DiscoveredDevice& b) {
    if (a.name != b.name) return a.name < b.name;
    return a.key() < b.key();
  });
}

}  // namespace

struct Discovery::Impl {
  mutable std::mutex mx;
  std::condition_variable cv;
  std::map<std::string, DiscoveredDevice> devs;
  std::string lastError;
  Options opts;
  Callback cb;
  std::thread th;
  bool stopFlag = false;
  bool refresh = false;
  std::atomic<bool> running{false};

  std::vector<DiscoveredDevice> snapshotLocked() const {
    std::vector<DiscoveredDevice> v;
    for (auto& kv : devs) v.push_back(kv.second);
    sortDevices(v);
    return v;
  }

  void run(SOCKET s) {
    setThreadName(L"owc-discovery");
    int64_t nextSend = 0;
    while (true) {
      {
        std::lock_guard<std::mutex> lk(mx);
        if (stopFlag) break;
        if (refresh) { refresh = false; nextSend = 0; }
      }
      const int64_t now = steadyMs();
      bool changed = false;
      if (now >= nextSend) {
        sendRequests(s, opts);
        nextSend = now + opts.interval_ms;
      }
      DiscoveredDevice d;
      if (recvReply(s, d)) {
        std::lock_guard<std::mutex> lk(mx);
        auto it = devs.find(d.key());
        if (it == devs.end()) {
          devs[d.key()] = d;
          changed = true;
        } else {
          d.first_seen_ms = it->second.first_seen_ms;
          changed = it->second.name != d.name || it->second.busy != d.busy ||
                    it->second.model != d.model || it->second.udp_port != d.udp_port;
          it->second = d;
        }
      }
      {
        std::lock_guard<std::mutex> lk(mx);
        for (auto it = devs.begin(); it != devs.end();) {
          if (steadyMs() - it->second.last_seen_ms > static_cast<int64_t>(opts.expiry_ms)) {
            it = devs.erase(it);
            changed = true;
          } else {
            ++it;
          }
        }
      }
      if (changed && cb) {
        std::vector<DiscoveredDevice> v;
        {
          std::lock_guard<std::mutex> lk(mx);
          v = snapshotLocked();
        }
        cb(v);
      }
    }
    closesocket(s);
  }
};

Discovery::Discovery() : impl_(new Impl) {}
Discovery::~Discovery() { stop(); }

bool Discovery::start(const Options& opts, Callback onChange) {
  stop();
  std::string err;
  SOCKET s = openSocket(opts, err);
  std::lock_guard<std::mutex> lk(impl_->mx);
  if (s == INVALID_SOCKET) {
    impl_->lastError = err;
    return false;
  }
  impl_->opts = opts;
  if (impl_->opts.interval_ms < 100) impl_->opts.interval_ms = 100;
  impl_->cb = std::move(onChange);
  impl_->stopFlag = false;
  impl_->devs.clear();
  impl_->running = true;
  impl_->th = std::thread([this, s] { impl_->run(s); });
  return true;
}

void Discovery::stop() {
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    impl_->stopFlag = true;
  }
  if (impl_->th.joinable()) impl_->th.join();
  impl_->running = false;
}

bool Discovery::running() const { return impl_->running; }

void Discovery::refreshNow() {
  std::lock_guard<std::mutex> lk(impl_->mx);
  impl_->refresh = true;
}

std::vector<DiscoveredDevice> Discovery::devices() const {
  std::lock_guard<std::mutex> lk(impl_->mx);
  return impl_->snapshotLocked();
}

std::string Discovery::lastError() const {
  std::lock_guard<std::mutex> lk(impl_->mx);
  return impl_->lastError;
}

std::vector<DiscoveredDevice> Discovery::scanOnce(uint32_t wait_ms, const Options& opts) {
  std::vector<DiscoveredDevice> out;
  std::string err;
  SOCKET s = openSocket(opts, err);
  if (s == INVALID_SOCKET) return out;
  std::map<std::string, DiscoveredDevice> found;
  const int64_t start = steadyMs();
  int sent = 0;
  while (steadyMs() - start < static_cast<int64_t>(wait_ms)) {
    if (sent < 3 && steadyMs() - start >= static_cast<int64_t>(sent) * (wait_ms / 3)) {
      sendRequests(s, opts);
      ++sent;
    }
    DiscoveredDevice d;
    if (recvReply(s, d)) {
      auto it = found.find(d.key());
      if (it != found.end()) d.first_seen_ms = it->second.first_seen_ms;
      found[d.key()] = d;
    }
  }
  closesocket(s);
  for (auto& kv : found) out.push_back(kv.second);
  sortDevices(out);
  return out;
}

}  // namespace owc
