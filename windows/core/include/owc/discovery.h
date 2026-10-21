// LAN discovery of OWC phones. Four complementary mechanisms, all on by
// default, all feeding one device list:
//
//  1. Broadcast DiscoveryRequest to 255.255.255.255:<port> and to the directed
//     broadcast of every up IPv4 interface, every interval_ms (2 s).
//  2. Passive listener on UDP <announce_port> (47803) for phone announcements
//     (DiscoveryReply kind=3, broadcast by the phone every ~2 s while idle).
//     NEEDED because many phones drop INCOMING broadcasts while the screen is
//     off (Android APF), so (1) gets no answer; their outgoing broadcasts still
//     work. Requires an inbound Windows Firewall allow rule for UDP 47803 on
//     Private networks: see owc/firewall.h. If the port can't be bound the
//     listener is simply off (announceListening() == false) and the other
//     mechanisms keep working.
//  3. Unicast sweep: DiscoveryRequest to EVERY host of each private, physical
//     IPv4 subnet with prefix >= /22 (WSL/Hyper-V vEthernet, VPN/TAP, loopback
//     and other virtual adapters are skipped), every sweep_interval_ms (6 s),
//     paced at <= 64 datagrams per ms. Unicasts are delivered even when the
//     phone filters broadcasts.
//  4. Unicast to "known" addresses (every phone ever seen) every interval_ms.
//     With persist_known (default) the list is loaded from / saved to
//     defaultKnownFile() = %APPDATA%\OWC\known_devices.txt (one IPv4 per line,
//     max 32). The UI may also manage it via Options::known_addresses.
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

// Kind of LOCAL network adapter a phone was seen on.
//   UsbTether: the phone's "USB tethering" network (RNDIS / NCM adapter) - the
//              no-adb USB path for normal users (typically 192.168.42.x/24).
enum class LinkType : uint8_t { Unknown = 0, Wifi, UsbTether, Ethernet, Other };
const char* toString(LinkType);

struct DiscoveredDevice {
  std::string ip;                       // IPv4 of the phone (reply/announce source)
  uint16_t control_port = kDefaultControlPort;
  uint16_t udp_port = kDefaultVideoUdpPort;
  std::string name, model;
  bool busy = false;                    // phone already has a PC connected
  bool via_announce = false;            // last seen through an announcement (kind=3)
  LinkType link = LinkType::Unknown;    // local adapter the reply/announce arrived on
  std::string local_adapter;            // its friendly name (e.g. "Ethernet 5", "Wi-Fi")
  int64_t first_seen_ms = 0;            // steady clock (GetTickCount64) ms
  int64_t last_seen_ms = 0;
  // Convenient unique key: "ip:control_port".
  std::string key() const { return ip + ":" + std::to_string(control_port); }
};

class Discovery {
 public:
  struct Options {
    uint16_t port = kDefaultDiscoveryPort;  // phone discovery port (requests go here)
    uint32_t interval_ms = 2000;            // broadcast + known-address request period
    uint32_t expiry_ms = 7000;              // drop devices silent this long
    bool broadcast = true;                  // send to 255.255.255.255 + directed bcasts
    std::vector<std::string> extra_targets; // unicast IPv4 targets every interval (e.g. "127.0.0.1")
    std::string bind_address = "0.0.0.0";   // local bind for the request socket

    // (2) passive announcement listener
    bool listen_announcements = true;
    uint16_t announce_port = kDefaultAnnouncePort;  // 47803
    std::string announce_bind_address = "0.0.0.0";

    // (3) unicast subnet sweep
    bool subnet_sweep = true;
    uint32_t sweep_interval_ms = 6000;
    uint8_t sweep_min_prefix = 22;          // only subnets with <= 1022 hosts
    std::vector<std::string> sweep_subnets; // extra "a.b.c.d/nn" CIDRs to sweep (tests, manual)

    // (4) known addresses
    std::vector<std::string> known_addresses;  // always unicast (in addition to the file)
    bool persist_known = true;                 // load/save known_file
    std::string known_file;                    // "" = defaultKnownFile()
  };
  using Callback = std::function<void(const std::vector<DiscoveredDevice>&)>;

  // Per-mechanism counters (diagnostics / UI "details" panel).
  struct Counters {
    uint64_t requests_sent = 0;   // broadcast + extra + known-address datagrams
    uint64_t sweep_rounds = 0;
    uint64_t sweep_sent = 0;      // unicast sweep datagrams sent
    uint32_t sweep_subnets = 0;   // subnets in the last sweep
    int64_t last_sweep_ms = 0;    // duration of the last sweep
    uint64_t send_errors = 0;     // sendto failures after retries
    int last_send_error = 0;      // WSA error code of the last failure (0 = none)
    uint64_t replies = 0;         // DiscoveryReply (kind=2) received
    uint64_t announces = 0;       // announcements (kind=3) received
    uint64_t invalid = 0;         // undecodable datagrams
  };

  Discovery();
  ~Discovery();  // stops
  Discovery(const Discovery&) = delete;
  Discovery& operator=(const Discovery&) = delete;

  // Starts continuous discovery (restarts if running). False on socket error
  // of the request socket (the announce listener failing is not an error).
  bool start(const Options& opts = Options(), Callback onChange = nullptr);
  void stop();
  bool running() const;
  void refreshNow();  // send requests + sweep immediately (continuous mode)
  // Snapshot sorted by name then ip.
  std::vector<DiscoveredDevice> devices() const;
  std::string lastError() const;
  // True while the passive announce listener is bound (continuous mode).
  bool announceListening() const;
  Counters counters() const;
  // Current known-address list (loaded + options + every phone seen).
  std::vector<std::string> knownAddresses() const;

  // One-shot blocking scan using all enabled mechanisms (requests are sent 3
  // times, wait_ms/3 apart; one sweep). Independent of continuous mode. Does
  // not write the known file.
  // Dozing phones can take ~0.3-1 s to answer (Wi-Fi power save): prefer
  // wait_ms >= 2000 or continuous mode.
  static std::vector<DiscoveredDevice> scanOnce(uint32_t wait_ms = 2000,
                                                const Options& opts = Options(),
                                                Counters* counters = nullptr);

  // ---- helpers (exposed for the UI and tests)
  // ---- link classification / USB tethering
  // A local IPv4 address of this PC with its adapter.
  struct LocalAdapter {
    std::string name;            // friendly name
    std::string description;     // driver description ("Remote NDIS based Internet Sharing Device")
    uint32_t if_type = 0;        // IANA ifType (6 Ethernet, 71 Wi-Fi, ...)
    std::string ipv4;            // this PC's address on it
    uint8_t prefix = 0;          // on-link prefix length
    bool has_default_gateway = false;
    uint32_t ipv4_metric = 0;    // effective interface metric (lower = preferred route)
    LinkType link = LinkType::Other;
  };
  // Up adapters with IPv4 (loopback excluded), with gateways and metrics.
  static std::vector<LocalAdapter> localAdapters();
  // Classifies an adapter: UsbTether when the name/description mentions Remote
  // NDIS / RNDIS / NCM / "Android USB Ethernet" (checked first, whatever the
  // ifType), Wifi for ifType 71, Ethernet for ifType 6 (non-virtual), else Other.
  static LinkType classifyAdapter(uint32_t ifType, const std::string& friendlyName,
                                  const std::string& description);
  // Which local adapter's subnet contains `ip` (Unknown if none, e.g. loopback).
  static LinkType linkFor(const std::string& ip, const std::vector<LocalAdapter>& adapters,
                          std::string* adapterName = nullptr);

  struct UsbTetherAdapter {
    std::string name, description, ipv4;
    uint8_t prefix = 0;
    bool has_default_gateway = false;
    uint32_t ipv4_metric = 0;
    // True when this adapter has a default gateway and its metric is <= every
    // other adapter's that also has one: Windows may route INTERNET traffic over
    // the phone's mobile data. UI: warn, and suggest raising the adapter metric
    // (or turning off the phone's mobile data).
    bool may_carry_internet = false;
  };
  static std::vector<UsbTetherAdapter> usbTetherAdapters();
  static std::vector<UsbTetherAdapter> usbTetherAdapters(const std::vector<LocalAdapter>& adapters);

  // The same phone (same name + model) seen on several links: keep one entry,
  // preferring UsbTether > Ethernet > Wifi > Other/Unknown (lower latency, no
  // Wi-Fi contention). Order otherwise preserved.
  static std::vector<DiscoveredDevice> preferBestLink(const std::vector<DiscoveredDevice>& devices);

  // IPv4 directed-broadcast addresses of all up, non-loopback interfaces.
  static std::vector<std::string> broadcastAddresses();
  struct Subnet {
    std::string adapter;     // friendly name
    std::string address;     // this PC's address on it
    std::string cidr;        // "192.168.0.0/24"
  };
  // Subnets the sweep covers: up, physical (USB tether included), private,
  // prefix >= minPrefix.
  static std::vector<Subnet> sweepSubnets(uint8_t minPrefix = 22);
  // Host addresses of a CIDR (network/broadcast excluded; /31-/32 = itself).
  // Empty for malformed input or prefix < 22 (more than 1022 hosts).
  static std::vector<std::string> subnetHosts(const std::string& cidr);
  static bool isPrivateIPv4(uint32_t hostOrderIp);
  // Heuristic on adapter friendly name / description (case-insensitive):
  // vEthernet, WSL, Hyper-V, VPN, TAP, WireGuard, VirtualBox, VMware, ...
  static bool isVirtualAdapter(const std::string& friendlyName, const std::string& description);
  static std::string defaultKnownFile();   // %APPDATA%\OWC\known_devices.txt (UTF-8 path)
  static std::vector<std::string> loadKnownAddresses(const std::string& file = std::string());
  static bool saveKnownAddresses(const std::vector<std::string>& ips,
                                 const std::string& file = std::string());

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace owc
