// owc_core_probe: manual environment check / live session tool.
//
//   owc_core_probe                      READ-ONLY: adb path/devices/forwards,
//                                       LAN discovery (2 s), decoder availability
//   owc_core_probe --connect host:port [--transport tcp|udp] [--seconds N]
//                  [--set id=value]... [--decoder auto|hw|sw] [--shm]
//                  [--no-patch] [--jitter MS]
//       Runs an owc::Client session and prints logs + stats every second.
//       --set values: plain number = Int32; prefix B = Bool, L = Int64,
//       F = Float, E = Enum (e.g. --set 18=E0 --set 2=1920 --set 29=F2.5).
//       --shm publishes to the shared-memory bus (off by default so the probe
//       never fights the OWC app for it). Exit code 0 if frames were decoded.
#include "decoder.h"
#include "util.h"

#include <owc/core.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace owc;

namespace {

void printCounters(const Discovery::Counters& c);

int readOnlyProbe() {
  Adb adb;
  std::printf("adb: %s\n", adb.adbPath().empty() ? "(not found)" : adb.adbPath().c_str());
  if (adb.available()) {
    std::printf("adb version: %s\n", adb.version().c_str());
    std::vector<AdbDevice> devs;
    std::string err;
    if (adb.listDevices(devs, &err)) {
      for (auto& d : devs)
        std::printf("  device %s state=%s model=%s product=%s transport_id=%d\n", d.serial.c_str(),
                    d.state.c_str(), d.model.c_str(), d.product.c_str(), d.transport_id);
      if (devs.empty()) std::printf("  (no devices)\n");
    } else {
      std::printf("  devices failed: %s\n", err.c_str());
    }
    std::vector<AdbForward> fw;
    if (adb.listForwards(fw, &err)) {
      for (auto& f : fw) std::printf("  forward %s %s -> %s\n", f.serial.c_str(), f.local.c_str(), f.remote.c_str());
      if (fw.empty()) std::printf("  (no forwards)\n");
    }
  }
  std::printf("discovery (2 s)...\n");
  Discovery::Counters dc;
  const auto found = Discovery::scanOnce(2000, Discovery::Options(), &dc);
  printCounters(dc);
  for (auto& d : found)
    std::printf("  %s:%u udp %u '%s' (%s)%s\n", d.ip.c_str(), d.control_port, d.udp_port,
                d.name.c_str(), d.model.c_str(), d.busy ? " BUSY" : "");
  detail::MfThreadScope mf;
  for (Codec c : {Codec::H264, Codec::Hevc}) {
    for (DecoderMode m : {DecoderMode::Hardware, DecoderMode::Software}) {
      detail::MfDecoder dec;
      detail::DecoderConfig cfg;
      cfg.codec = c;
      cfg.width = 1920;
      cfg.height = 1080;
      cfg.mode = m;
      std::string err;
      const bool ok = dec.open(cfg, err);
      std::printf("decoder %s %s: %s\n", c == Codec::H264 ? "H.264" : "HEVC", toString(m),
                  ok ? dec.name().c_str() : err.c_str());
    }
  }
  return 0;
}

bool parseValue(const char* s, ParamValue& v) {
  char* end = nullptr;
  switch (s[0]) {
    case 'B': case 'b': v = ParamValue::Bool(atoi(s + 1) != 0); return true;
    case 'L': case 'l': v = ParamValue::Long(_strtoi64(s + 1, &end, 10)); return true;
    case 'F': case 'f': v = ParamValue::Real(static_cast<float>(atof(s + 1))); return true;
    case 'E': case 'e': v = ParamValue::EnumV(atoi(s + 1)); return true;
    default: v = ParamValue::Int(static_cast<int32_t>(strtol(s, &end, 10))); return end && *end == 0;
  }
}

struct Events : ClientEvents {
  std::atomic<uint64_t> frames{0};
  std::atomic<uint32_t> lastW{0}, lastH{0}, lastMatrix{0};
  void onStateChanged(ClientState s, DisconnectReason r, const std::string& m) override {
    std::printf("[state] %s %s %s\n", toString(s), r != DisconnectReason::None ? toString(r) : "",
                m.c_str());
  }
  void onConnected(const DeviceInfo& d) override {
    std::printf("[connected] %s (%s) API %u codecs 0x%x session %u udp %u\n", d.device_name.c_str(),
                d.model.c_str(), d.android_api, d.codec_mask, d.session_id, d.udp_port);
  }
  void onCapabilities(const Capabilities& c) override {
    std::printf("[caps] %zu camera(s), %zu params, active %u\n", c.cameras.size(), c.params.size(),
                c.active_camera);
  }
  void onParamState(const ParamState& p) override {
    std::printf("[param] %s status %d value i=%lld f=%g\n", owc::toString(p.id),
                static_cast<int>(p.status), static_cast<long long>(p.value.i), p.value.f);
  }
  void onVideoFormat(const VideoFormat& f) override {
    std::printf("[format] epoch %u %s %ux%u@%u rot %d mirror %d csd %zu B\n", f.epoch,
                f.codec == Codec::H264 ? "H.264" : "HEVC", f.width, f.height, f.fps,
                f.rotation_deg, f.mirror ? 1 : 0, f.csd.size());
  }
  void onFrame(const DecodedFrame& f) override {
    ++frames;
    lastW = f.width;
    lastH = f.height;
    lastMatrix = f.color_matrix;
  }
};

int connectMode(int argc, char** argv) {
  ConnectParams cp;
  ClientOptions o;
  o.publish_to_shm = false;
  o.auto_reconnect = false;
  int seconds = 10;
  std::vector<std::pair<ParamId, ParamValue>> sets;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--connect") {
      std::string hp = next();
      const size_t c = hp.rfind(':');
      cp.host = hp.substr(0, c);
      cp.port = c == std::string::npos ? kDefaultControlPort
                                       : static_cast<uint16_t>(atoi(hp.c_str() + c + 1));
    } else if (a == "--transport") {
      cp.transport = std::string(next()) == "udp" ? VideoTransport::Udp : VideoTransport::Tcp;
    } else if (a == "--seconds") {
      seconds = atoi(next());
    } else if (a == "--set") {
      const std::string kv = next();
      const size_t e = kv.find('=');
      ParamValue v;
      if (e == std::string::npos || !parseValue(kv.c_str() + e + 1, v)) {
        std::fprintf(stderr, "bad --set %s\n", kv.c_str());
        return 2;
      }
      sets.emplace_back(static_cast<ParamId>(atoi(kv.c_str())), v);
    } else if (a == "--decoder") {
      const std::string d = next();
      o.decoder_mode = d == "hw" ? DecoderMode::Hardware : d == "sw" ? DecoderMode::Software : DecoderMode::Auto;
    } else if (a == "--shm") {
      o.publish_to_shm = true;
    } else if (a == "--no-patch") {
      o.patch_sps = false;
    } else if (a == "--jitter") {
      o.jitter_buffer_ms = static_cast<uint32_t>(atoi(next()));
    } else {
      std::fprintf(stderr, "unknown argument %s\n", a.c_str());
      return 2;
    }
  }
  cp.client_name = "owc_core_probe";
  Events ev;
  Client client(&ev, o);
  client.connect(cp);
  bool sent = false;
  uint64_t logId = 0;
  for (int t = 0; t < seconds * 10; ++t) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!sent && (client.state() == ClientState::Connected || client.state() == ClientState::Streaming)) {
      for (auto& kv : sets) client.setParam(kv.first, kv.second);
      sent = true;
    }
    for (const LogEntry& l : client.logs(logId)) {
      logId = l.id;
      std::printf("[log %s %d] %s\n", l.source == LogSource::Phone ? "phone" : "pc",
                  static_cast<int>(l.level), l.text.c_str());
    }
    if (t % 10 == 9) {
      const ClientStats s = client.stats();
      std::printf(
          "[%2ds] %s rx %.1f fps %.0f kbps recv %llu lost %llu | dec %llu (%.1f fps) pub %llu | "
          "out %ux%u | %s%s | decode %.2f ms (rb %.2f) delay %.2f/%u | cap->arr %.1f cap->pub "
          "%.1f/%.1f ms | rtt %.2f ms | kf %u%s%s | phone %.1f fps q %u drop %u\n",
          t / 10 + 1, toString(s.state), s.rx_fps, s.rx_bitrate_bps / 1000.0,
          static_cast<unsigned long long>(s.frames_received), static_cast<unsigned long long>(s.frames_lost),
          static_cast<unsigned long long>(s.frames_decoded), s.decoded_fps,
          static_cast<unsigned long long>(s.frames_published), s.output_width, s.output_height,
          s.decoder_name.c_str(), s.decoder_error.empty() ? "" : (" ERR " + s.decoder_error).c_str(),
          s.decode_ms_avg, s.readback_ms_avg, s.decoder_delay_frames_avg, s.decoder_delay_frames_max,
          s.capture_to_arrival_ms_avg, s.capture_to_publish_ms_avg, s.capture_to_publish_ms_p95,
          s.rtt_ms, s.keyframe_requests, s.decoder_selection.empty() ? "" : " | ",
          s.decoder_selection.c_str(), s.phone.encode_fps, s.phone.send_queue, s.phone.dropped_frames);
      std::fflush(stdout);
    }
  }
  const uint64_t frames = ev.frames;
  client.disconnect();
  std::printf("frames delivered to onFrame: %llu (last %ux%u matrix %u)\n",
              static_cast<unsigned long long>(frames), ev.lastW.load(), ev.lastH.load(), ev.lastMatrix.load());
  return frames > 0 ? 0 : 1;
}

void printCounters(const Discovery::Counters& c) {
  std::printf("  counters: requests %llu | sweep rounds %llu sent %llu subnets %u last %lld ms | "
              "send errors %llu (last WSA %d) | replies %llu announces %llu invalid %llu\n",
              static_cast<unsigned long long>(c.requests_sent),
              static_cast<unsigned long long>(c.sweep_rounds),
              static_cast<unsigned long long>(c.sweep_sent), c.sweep_subnets,
              static_cast<long long>(c.last_sweep_ms), static_cast<unsigned long long>(c.send_errors),
              c.last_send_error, static_cast<unsigned long long>(c.replies),
              static_cast<unsigned long long>(c.announces), static_cast<unsigned long long>(c.invalid));
}

// --discover N [--no-sweep] [--no-broadcast] [--no-announce] [--no-known]
int discoverMode(int argc, char** argv) {
  Discovery::Options o;
  int seconds = 10;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--discover" && i + 1 < argc) seconds = atoi(argv[++i]);
    else if (a == "--no-sweep") o.subnet_sweep = false;
    else if (a == "--no-broadcast") o.broadcast = false;
    else if (a == "--no-announce") o.listen_announcements = false;
    else if (a == "--no-known") o.persist_known = false;
  }
  std::printf("sweep subnets:\n");
  for (auto& s : Discovery::sweepSubnets(o.sweep_min_prefix))
    std::printf("  %s on '%s' (this PC %s)\n", s.cidr.c_str(), s.adapter.c_str(), s.address.c_str());
  std::printf("firewall rule '%s' present: %d\n", Firewall::ruleName(), Firewall::rulesPresent() ? 1 : 0);
  Discovery d;
  const int64_t t0 = detail::steadyMs();
  if (!d.start(o, [t0](const std::vector<DiscoveredDevice>& v) {
        std::printf("[+%lld ms] device list changed: %zu device(s)\n",
                    static_cast<long long>(detail::steadyMs() - t0), v.size());
      })) {
    std::printf("start failed: %s\n", d.lastError().c_str());
    return 1;
  }
  std::printf("announce listener: %s %s\n", d.announceListening() ? "on" : "OFF",
              d.lastError().c_str());
  for (int t = 1; t <= seconds; ++t) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::printf("[%2ds] %zu device(s)\n", t, d.devices().size());
    for (auto& dev : d.devices())
      std::printf("  %s:%u udp %u '%s' (%s)%s%s\n", dev.ip.c_str(), dev.control_port, dev.udp_port,
                  dev.name.c_str(), dev.model.c_str(), dev.busy ? " BUSY" : "",
                  dev.via_announce ? " [announce]" : "");
    printCounters(d.counters());
  }
  const size_t n = d.devices().size();
  d.stop();
  return n ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--connect") == 0) return connectMode(argc, argv);
    if (std::strcmp(argv[i], "--discover") == 0) return discoverMode(argc, argv);
  }
  if (argc > 1) {
    std::fprintf(stderr, "usage: owc_core_probe [--discover N [--no-sweep] [--no-broadcast] [--no-announce] [--no-known]] [--connect host:port [--transport tcp|udp] "
                         "[--seconds N] [--set id=value]... [--decoder auto|hw|sw] [--shm] "
                         "[--no-patch] [--jitter MS]]\n");
    return 2;
  }
  return readOnlyProbe();
}
