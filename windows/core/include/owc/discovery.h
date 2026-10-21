// LAN discovery of OWC phones.
//
// Sends protocol DiscoveryRequest datagrams to 255.255.255.255:<port>, to the
// directed broadcast address of every up IPv4 interface (GetAdaptersAddresses)
// and to any extra unicast targets; phones answer with DiscoveryReply to the
// source address. Continuous mode repeats every interval_ms and expires
// devices not seen for expiry_ms.
//
// THREADING: all methods are thread-safe and non-blocking except scanOnce()
// (blocks for wait_ms). Continuous mode runs one internal thread
// ("owc-discovery"); the optional callback is invoked on that thread whenever
// the device list changes (added / updated name/busy / expired). Don't call
// stop() or the destructor from inside the callback.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <owc/protocol.h>

namespace owc {

struct DiscoveredDevice {
  std::string ip;                       // IPv4 of the phone (reply source)
  uint16_t control_port = kDefaultControlPort;
  uint16_t udp_port = kDefaultVideoUdpPort;
  std::string name, model;
  bool busy = false;                    // phone already has a PC connected
  int64_t first_seen_ms = 0;            // steady clock (GetTickCount64) ms
  int64_t last_seen_ms = 0;
  // Convenient unique key: "ip:control_port".
  std::string key() const { return ip + ":" + std::to_string(control_port); }
};

class Discovery {
 public:
  struct Options {
    uint16_t port = kDefaultDiscoveryPort;  // phone discovery port
    uint32_t interval_ms = 2000;            // continuous mode request period
    uint32_t expiry_ms = 7000;              // drop devices silent this long
    bool broadcast = true;                  // send to 255.255.255.255 + directed bcasts
    std::vector<std::string> extra_targets; // unicast IPv4 targets (e.g. "127.0.0.1")
    std::string bind_address = "0.0.0.0";   // local bind for the socket
  };
  using Callback = std::function<void(const std::vector<DiscoveredDevice>&)>;

  Discovery();
  ~Discovery();  // stops
  Discovery(const Discovery&) = delete;
  Discovery& operator=(const Discovery&) = delete;

  // Starts continuous discovery (restarts if running). False on socket error.
  bool start(const Options& opts = Options(), Callback onChange = nullptr);
  void stop();
  bool running() const;
  void refreshNow();  // send a request burst immediately (continuous mode)
  // Snapshot sorted by name then ip.
  std::vector<DiscoveredDevice> devices() const;
  std::string lastError() const;

  // One-shot blocking scan: sends requests (3 times, wait_ms/3 apart) and
  // collects replies for wait_ms. Independent of continuous mode.
  static std::vector<DiscoveredDevice> scanOnce(uint32_t wait_ms = 1500,
                                                const Options& opts = Options());

  // IPv4 directed-broadcast addresses of all up, non-loopback interfaces.
  static std::vector<std::string> broadcastAddresses();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace owc
