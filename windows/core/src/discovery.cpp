#include <owc/discovery.h>

#include "util.h"

#include <iphlpapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace owc {

using namespace detail;

namespace {

constexpr size_t kMaxKnown = 32;
constexpr int kSweepBurst = 64;  // datagrams per ~1 ms

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string ipToString(uint32_t hostOrder) {
  char b[16];
  snprintf(b, sizeof(b), "%u.%u.%u.%u", (hostOrder >> 24) & 255u, (hostOrder >> 16) & 255u,
           (hostOrder >> 8) & 255u, hostOrder & 255u);
  return b;
}

bool parseIPv4(const std::string& s, uint32_t& hostOrder) {
  in_addr a{};
  if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
  hostOrder = ntohl(a.s_addr);
  return true;
}

bool parseCidr(const std::string& cidr, uint32_t& net, uint8_t& prefix) {
  const size_t slash = cidr.find('/');
  if (slash == std::string::npos) return false;
  uint32_t ip = 0;
  if (!parseIPv4(cidr.substr(0, slash), ip)) return false;
  const int p = atoi(cidr.c_str() + slash + 1);
  if (p < 0 || p > 32) return false;
  prefix = static_cast<uint8_t>(p);
  const uint32_t mask = prefix == 0 ? 0u : ~((prefix >= 32 ? 0u : (1u << (32 - prefix))) - 1u);
  net = ip & mask;
  return true;
}

template <class F>
void forEachAdapter(F&& f) {
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
  if (rc != NO_ERROR) return;
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
      const auto* sin = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
      f(*a, ntohl(sin->sin_addr.s_addr), static_cast<uint8_t>(u->OnLinkPrefixLength));
    }
  }
}

void disableConnReset(SOCKET s) {
  BOOL f = FALSE;
  DWORD br = 0;
  WSAIoctl(s, SIO_UDP_CONNRESET, &f, sizeof(f), nullptr, 0, &br, nullptr, nullptr);
}

SOCKET openRequestSocket(const Discovery::Options& o, std::string& err) {
  if (!winsockInit()) { err = "WSAStartup failed"; return INVALID_SOCKET; }
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) { err = "socket() failed"; return s; }
  BOOL on = TRUE;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof(on));
  disableConnReset(s);
  int sb = 1 << 20;
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sb), sizeof(sb));
  sockaddr_in a{};
  if (!resolveIPv4(o.bind_address.empty() ? "0.0.0.0" : o.bind_address, 0, a) ||
      bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    err = "bind failed " + std::to_string(WSAGetLastError());
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

// Passive listener for kind=3 announcements. INVALID_SOCKET (with err) if the
// port can't be bound; SO_REUSEADDR lets several OWC processes share it.
SOCKET openAnnounceSocket(const Discovery::Options& o, std::string& err) {
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) { err = "announce socket() failed"; return s; }
  BOOL on = TRUE;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
  disableConnReset(s);
  sockaddr_in a{};
  if (!resolveIPv4(o.announce_bind_address.empty() ? "0.0.0.0" : o.announce_bind_address,
                   o.announce_port, a) ||
      bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    err = "announce listener: cannot bind UDP " + std::to_string(o.announce_port) + " (" +
          std::to_string(WSAGetLastError()) + ")";
    closesocket(s);
    return INVALID_SOCKET;
  }
  return s;
}

void sortDevices(std::vector<DiscoveredDevice>& v) {
  std::sort(v.begin(), v.end(), [](const DiscoveredDevice& a, const DiscoveredDevice& b) {
    if (a.name != b.name) return a.name < b.name;
    return a.key() < b.key();
  });
}

// Sockets + send/receive logic shared by continuous mode and scanOnce().
// Sockets are NON-BLOCKING: sends that hit WSAEWOULDBLOCK/WSAENOBUFS (a
// 254-host burst can fill the send queue while ARP resolves) wait for
// writability and retry instead of being silently dropped; every failure is
// counted (Counters::send_errors / last_send_error).
struct Engine {
  Discovery::Options o;
  SOCKET req = INVALID_SOCKET, ann = INVALID_SOCKET;
  std::string annErr;
  std::vector<uint8_t> request = encodeDiscoveryRequest();
  Discovery::Counters c;

  bool open(std::string& err) {
    req = openRequestSocket(o, err);
    if (req == INVALID_SOCKET) return false;
    setNonBlocking(req, true);
    int rb = 1 << 18;
    setsockopt(req, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rb), sizeof(rb));
    if (o.listen_announcements) {
      ann = openAnnounceSocket(o, annErr);
      if (ann != INVALID_SOCKET) setNonBlocking(ann, true);
    }
    return true;
  }
  void close() {
    closeSocket(req);
    closeSocket(ann);
  }

  bool sendTo(uint32_t ipHostOrder, uint16_t port) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(ipHostOrder);
    for (int attempt = 0; attempt < 6; ++attempt) {
      const int n = sendto(req, reinterpret_cast<const char*>(request.data()),
                           static_cast<int>(request.size()), 0,
                           reinterpret_cast<sockaddr*>(&to), sizeof(to));
      if (n == static_cast<int>(request.size())) return true;
      const int e = WSAGetLastError();
      c.last_send_error = e;
      if (e != WSAEWOULDBLOCK && e != WSAENOBUFS) break;
      fd_set w;
      FD_ZERO(&w);
      FD_SET(req, &w);
      timeval tv{0, 20000};
      select(0, nullptr, &w, nullptr, &tv);
    }
    ++c.send_errors;
    return false;
  }

  void sendRequests(const std::vector<std::string>& known) {
    std::vector<std::string> targets;
    if (o.broadcast) {
      targets.push_back("255.255.255.255");
      for (auto& b : Discovery::broadcastAddresses()) targets.push_back(b);
    }
    targets.insert(targets.end(), o.extra_targets.begin(), o.extra_targets.end());
    targets.insert(targets.end(), known.begin(), known.end());
    std::set<std::string> done;
    for (auto& t : targets) {
      uint32_t ip = 0;
      if (!done.insert(t).second || !parseIPv4(t, ip)) continue;
      if (sendTo(ip, o.port)) ++c.requests_sent;
    }
  }

  // Unicast to every host of the sweep subnets, paced; replies that arrive
  // meanwhile are drained into `out` so a full receive buffer can't drop them.
  void sweep(std::vector<DiscoveredDevice>& out) {
    const int64_t t0 = steadyMs();
    std::vector<std::string> cidrs = o.sweep_subnets;
    for (auto& sn : Discovery::sweepSubnets(o.sweep_min_prefix)) cidrs.push_back(sn.cidr);
    std::set<std::string> uniq(cidrs.begin(), cidrs.end());
    c.sweep_subnets = static_cast<uint32_t>(uniq.size());
    int burst = 0;
    for (auto& cidr : uniq) {
      for (auto& h : Discovery::subnetHosts(cidr)) {
        uint32_t ip = 0;
        if (!parseIPv4(h, ip)) continue;
        if (sendTo(ip, o.port)) ++c.sweep_sent;
        if (++burst >= kSweepBurst) {
          burst = 0;
          poll(1, out);  // ~1 ms pacing that also drains replies
        }
      }
    }
    ++c.sweep_rounds;
    c.last_sweep_ms = steadyMs() - t0;
  }

  // Waits up to timeoutMs, then drains EVERY pending datagram of both sockets.
  // Returns the number of devices appended to `out`.
  int poll(int timeoutMs, std::vector<DiscoveredDevice>& out) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(req, &r);
    if (ann != INVALID_SOCKET) FD_SET(ann, &r);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    if (select(0, &r, nullptr, nullptr, &tv) <= 0) return 0;
    int got = 0;
    for (SOCKET s : {req, ann}) {
      if (s == INVALID_SOCKET) continue;
      for (int guard = 0; guard < 1024; ++guard) {
        uint8_t buf[2048];
        sockaddr_in from{};
        int fl = sizeof(from);
        const int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &fl);
        if (n < 0) {
          const int e = WSAGetLastError();
          if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  // ICMP / junk: keep draining
          break;  // WSAEWOULDBLOCK: drained
        }
        DiscoveryReply rep;
        if (!decode(buf, static_cast<size_t>(n), rep)) {
          ++c.invalid;
          continue;
        }
        DiscoveredDevice d;
        d.ip = addrToString(from, false);
        d.control_port = rep.control_port;
        d.udp_port = rep.udp_port;
        d.name = rep.device_name;
        d.model = rep.model;
        d.busy = rep.busy;
        d.via_announce = rep.announce;
        d.last_seen_ms = d.first_seen_ms = steadyMs();
        if (rep.announce) ++c.announces;
        else ++c.replies;
        out.push_back(d);
        ++got;
      }
    }
    return got;
  }
};

}  // namespace

// ================================================================ helpers
std::vector<std::string> Discovery::broadcastAddresses() {
  std::vector<std::string> out;
  forEachAdapter([&](const IP_ADAPTER_ADDRESSES&, uint32_t ip, uint8_t plen) {
    if (plen == 0 || plen >= 31) return;
    if ((ip >> 16) == 0xA9FE) return;  // 169.254/16 link-local (no DHCP)
    const uint32_t mask = ~((1u << (32 - plen)) - 1u);
    const std::string s = ipToString((ip & mask) | ~mask);
    if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  });
  return out;
}

bool Discovery::isPrivateIPv4(uint32_t ip) {
  return (ip >> 24) == 10 || (ip >> 20) == 0xAC1 /* 172.16/12 */ || (ip >> 16) == 0xC0A8;
}

bool Discovery::isVirtualAdapter(const std::string& friendlyName, const std::string& description) {
  static const char* const kPatterns[] = {
      "vethernet", "wsl", "hyper-v", "virtualbox", "vmware", "vpn", "tap-", "tap ",
      "wireguard", "tailscale", "zerotier", "hamachi", "docker", "npcap", "loopback",
      "virtual", "tunnel", "anyconnect", "fortinet", "openvpn", "nordlynx"};
  const std::string n = lower(friendlyName) + " | " + lower(description);
  for (const char* p : kPatterns)
    if (n.find(p) != std::string::npos) return true;
  return false;
}

std::vector<Discovery::Subnet> Discovery::sweepSubnets(uint8_t minPrefix) {
  std::vector<Subnet> out;
  forEachAdapter([&](const IP_ADAPTER_ADDRESSES& a, uint32_t ip, uint8_t plen) {
    if (a.IfType == IF_TYPE_TUNNEL || a.IfType == IF_TYPE_PPP) return;
    if (plen < minPrefix || plen > 30 || !isPrivateIPv4(ip)) return;
    const std::string fn = a.FriendlyName ? wideToUtf8(a.FriendlyName) : std::string();
    const std::string de = a.Description ? wideToUtf8(a.Description) : std::string();
    if (isVirtualAdapter(fn, de)) return;
    const uint32_t mask = ~((1u << (32 - plen)) - 1u);
    Subnet s;
    s.adapter = fn;
    s.address = ipToString(ip);
    s.cidr = ipToString(ip & mask) + "/" + std::to_string(plen);
    for (auto& x : out)
      if (x.cidr == s.cidr) return;
    out.push_back(s);
  });
  return out;
}

std::vector<std::string> Discovery::subnetHosts(const std::string& cidr) {
  std::vector<std::string> out;
  uint32_t net = 0;
  uint8_t prefix = 0;
  if (!parseCidr(cidr, net, prefix) || prefix < 22) return out;
  if (prefix >= 31) {
    const uint32_t count = prefix == 32 ? 1u : 2u;
    for (uint32_t i = 0; i < count; ++i) out.push_back(ipToString(net + i));
    return out;
  }
  const uint32_t size = 1u << (32 - prefix);
  out.reserve(size - 2);
  for (uint32_t i = 1; i + 1 < size; ++i) out.push_back(ipToString(net + i));
  return out;
}

std::string Discovery::defaultKnownFile() {
  PWSTR p = nullptr;
  std::string out;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p)) && p)
    out = wideToUtf8(std::wstring(p) + L"\\OWC\\known_devices.txt");
  if (p) CoTaskMemFree(p);
  return out;
}

std::vector<std::string> Discovery::loadKnownAddresses(const std::string& file) {
  std::vector<std::string> out;
  const std::string path = file.empty() ? defaultKnownFile() : file;
  if (path.empty()) return out;
  FILE* f = nullptr;
  if (_wfopen_s(&f, utf8ToWide(path).c_str(), L"rb") != 0 || !f) return out;
  char line[128];
  while (fgets(line, sizeof(line), f)) {
    std::string s(line);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    uint32_t ip = 0;
    if (parseIPv4(s, ip) && std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
  }
  fclose(f);
  if (out.size() > kMaxKnown) out.erase(out.begin(), out.end() - kMaxKnown);
  return out;
}

bool Discovery::saveKnownAddresses(const std::vector<std::string>& ips, const std::string& file) {
  const std::string path = file.empty() ? defaultKnownFile() : file;
  if (path.empty()) return false;
  const std::wstring w = utf8ToWide(path);
  const size_t slash = w.find_last_of(L"\\/");
  if (slash != std::wstring::npos) CreateDirectoryW(w.substr(0, slash).c_str(), nullptr);
  FILE* f = nullptr;
  if (_wfopen_s(&f, w.c_str(), L"wb") != 0 || !f) return false;
  const size_t first = ips.size() > kMaxKnown ? ips.size() - kMaxKnown : 0;
  for (size_t i = first; i < ips.size(); ++i) fprintf(f, "%s\n", ips[i].c_str());
  fclose(f);
  return true;
}

// ================================================================ continuous
struct Discovery::Impl {
  mutable std::mutex mx;
  std::map<std::string, DiscoveredDevice> devs;
  std::vector<std::string> known;  // ordered, most recent last
  std::string lastError;
  Callback cb;
  std::thread th;
  bool stopFlag = false;
  bool refresh = false;
  std::atomic<bool> running{false};
  std::atomic<bool> announceOn{false};
  Counters counters;

  std::vector<DiscoveredDevice> snapshotLocked() const {
    std::vector<DiscoveredDevice> v;
    for (auto& kv : devs) v.push_back(kv.second);
    sortDevices(v);
    return v;
  }

  // Returns true if the known list changed.
  bool rememberLocked(const std::string& ip) {
    auto it = std::find(known.begin(), known.end(), ip);
    if (it != known.end()) return false;
    known.push_back(ip);
    if (known.size() > kMaxKnown) known.erase(known.begin());
    return true;
  }

  void run(Engine* e) {
    setThreadName(L"owc-discovery");
    int64_t nextReq = 0, nextSweep = 0;
    while (true) {
      std::vector<std::string> knownCopy;
      {
        std::lock_guard<std::mutex> lk(mx);
        if (stopFlag) break;
        if (refresh) {
          refresh = false;
          nextReq = nextSweep = 0;
        }
        knownCopy = known;
      }
      const int64_t now = steadyMs();
      if (now >= nextReq) {
        e->sendRequests(knownCopy);
        nextReq = now + e->o.interval_ms;
      }
      std::vector<DiscoveredDevice> got;
      if (e->o.subnet_sweep && now >= nextSweep) {
        e->sweep(got);
        nextSweep = steadyMs() + e->o.sweep_interval_ms;
      }
      bool changed = false, knownChanged = false;
      e->poll(100, got);
      {
        std::lock_guard<std::mutex> lk(mx);
        counters = e->c;
        for (DiscoveredDevice& d : got) {
          auto it = devs.find(d.key());
          if (it == devs.end()) {
            devs[d.key()] = d;
            changed = true;
          } else {
            d.first_seen_ms = it->second.first_seen_ms;
            changed |= it->second.name != d.name || it->second.busy != d.busy ||
                       it->second.model != d.model || it->second.udp_port != d.udp_port;
            it->second = d;
          }
          knownChanged |= rememberLocked(d.ip);
        }
      }
      {
        std::lock_guard<std::mutex> lk(mx);
        for (auto it = devs.begin(); it != devs.end();) {
          if (steadyMs() - it->second.last_seen_ms > static_cast<int64_t>(e->o.expiry_ms)) {
            it = devs.erase(it);
            changed = true;
          } else {
            ++it;
          }
        }
      }
      if (knownChanged && e->o.persist_known) {
        std::vector<std::string> k;
        {
          std::lock_guard<std::mutex> lk(mx);
          k = known;
        }
        saveKnownAddresses(k, e->o.known_file);
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
    e->close();
    delete e;
  }
};

Discovery::Discovery() : impl_(new Impl) {}
Discovery::~Discovery() { stop(); }

bool Discovery::start(const Options& opts, Callback onChange) {
  stop();
  auto* e = new Engine;
  e->o = opts;
  if (e->o.interval_ms < 100) e->o.interval_ms = 100;
  if (e->o.sweep_interval_ms < 1000) e->o.sweep_interval_ms = 1000;
  std::string err;
  if (!e->open(err)) {
    delete e;
    std::lock_guard<std::mutex> lk(impl_->mx);
    impl_->lastError = err;
    return false;
  }
  std::lock_guard<std::mutex> lk(impl_->mx);
  impl_->lastError = e->annErr;  // non-fatal: announce listener unavailable
  impl_->announceOn = e->ann != INVALID_SOCKET;
  impl_->known.clear();
  if (opts.persist_known)
    for (auto& ip : loadKnownAddresses(opts.known_file)) impl_->rememberLocked(ip);
  for (auto& ip : opts.known_addresses) impl_->rememberLocked(ip);
  impl_->cb = std::move(onChange);
  impl_->stopFlag = false;
  impl_->devs.clear();
  impl_->counters = Counters();
  impl_->running = true;
  impl_->th = std::thread([this, e] { impl_->run(e); });
  return true;
}

void Discovery::stop() {
  {
    std::lock_guard<std::mutex> lk(impl_->mx);
    impl_->stopFlag = true;
  }
  if (impl_->th.joinable()) impl_->th.join();
  impl_->running = false;
  impl_->announceOn = false;
}

bool Discovery::running() const { return impl_->running; }
bool Discovery::announceListening() const { return impl_->announceOn; }

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

Discovery::Counters Discovery::counters() const {
  std::lock_guard<std::mutex> lk(impl_->mx);
  return impl_->counters;
}

std::vector<std::string> Discovery::knownAddresses() const {
  std::lock_guard<std::mutex> lk(impl_->mx);
  return impl_->known;
}

std::vector<DiscoveredDevice> Discovery::scanOnce(uint32_t wait_ms, const Options& opts,
                                                Counters* countersOut) {
  std::vector<DiscoveredDevice> out;
  Engine e;
  e.o = opts;
  std::string err;
  if (!e.open(err)) return out;
  std::vector<std::string> known = opts.known_addresses;
  if (opts.persist_known)
    for (auto& ip : loadKnownAddresses(opts.known_file)) known.push_back(ip);
  std::map<std::string, DiscoveredDevice> found;
  auto merge = [&](std::vector<DiscoveredDevice>& v) {
    for (DiscoveredDevice& d : v) {
      auto it = found.find(d.key());
      if (it != found.end()) d.first_seen_ms = it->second.first_seen_ms;
      found[d.key()] = d;
    }
    v.clear();
  };
  const int64_t start = steadyMs();
  int sent = 0;
  bool swept = !opts.subnet_sweep;
  std::vector<DiscoveredDevice> got;
  while (steadyMs() - start < static_cast<int64_t>(wait_ms)) {
    if (sent < 3 && steadyMs() - start >= static_cast<int64_t>(sent) * (wait_ms / 3)) {
      e.sendRequests(known);
      ++sent;
    }
    if (!swept) {
      e.sweep(got);
      swept = true;
    }
    e.poll(50, got);
    merge(got);
  }
  e.close();
  if (countersOut) *countersOut = e.c;
  for (auto& kv : found) out.push_back(kv.second);
  sortDevices(out);
  return out;
}

}  // namespace owc
