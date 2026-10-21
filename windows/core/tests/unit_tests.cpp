// owc_core unit tests: transforms, clock sync, jitter/delay trend, keyframe
// policy, adb parsing, process exec, Annex-B helpers, counter strip, presets.
#include "test_util.h"

#include "annexb.h"
#include "codec_select.h"
#include "record_setup.h"
#include "metrics.h"
#include "transform.h"
#include "util.h"

#include "pattern.h"

#include <owc/adb.h>
#include <owc/discovery.h>
#include <owc/firewall.h>
#include <owc/presets.h>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace owc;
using namespace owc::detail;

namespace {

// ------------------------------------------------------------------ transform
struct Img {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> y, uv;  // tight
  Img(uint32_t W, uint32_t H) : w(W), h(H), y(W * H), uv(W * H / 2) {}
  uint8_t Y(uint32_t x, uint32_t r) const { return y[r * w + x]; }
  uint16_t UV(uint32_t x2, uint32_t r2) const {
    uint16_t v;
    memcpy(&v, &uv[r2 * w + x2 * 2], 2);
    return v;
  }
};

// Reference: dst(dx,dy) for final = mirror(rotate_cw(src)).
void refSrcCoord(int rot, bool mirror, int sw, int sh, int dx, int dy, int& sx, int& sy) {
  const int dw = (rot == 90 || rot == 270) ? sh : sw;
  const int rx = mirror ? dw - 1 - dx : dx;
  switch (rot) {
    case 0: sx = rx; sy = dy; break;
    case 90: sx = dy; sy = sh - 1 - rx; break;
    case 180: sx = sw - 1 - rx; sy = sh - 1 - dy; break;
    default: sx = sw - 1 - dy; sy = rx; break;
  }
}

void testTransformRandom() {
  std::mt19937 rng(1);
  const uint32_t sizes[][2] = {{2, 2}, {4, 2}, {10, 6}, {64, 34}, {70, 100}};
  for (auto& sz : sizes) {
    Img src(sz[0], sz[1]);
    for (auto& b : src.y) b = static_cast<uint8_t>(rng());
    for (auto& b : src.uv) b = static_cast<uint8_t>(rng());
    for (int rot : {0, 90, 180, 270}) {
      for (bool mirror : {false, true}) {
        uint32_t ow, oh;
        transformedSize(src.w, src.h, rot, ow, oh);
        Img dst(ow, oh);
        transformNV12(src.y.data(), src.w, src.uv.data(), src.w, src.w, src.h, rot, mirror,
                      dst.y.data(), ow, dst.uv.data(), ow);
        bool ok = true;
        for (uint32_t dy = 0; dy < oh && ok; ++dy)
          for (uint32_t dx = 0; dx < ow && ok; ++dx) {
            int sx, sy;
            refSrcCoord(rot, mirror, static_cast<int>(src.w), static_cast<int>(src.h),
                        static_cast<int>(dx), static_cast<int>(dy), sx, sy);
            ok = dst.Y(dx, dy) == src.Y(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
          }
        for (uint32_t dy = 0; dy < oh / 2 && ok; ++dy)
          for (uint32_t dx = 0; dx < ow / 2 && ok; ++dx) {
            int sx, sy;
            refSrcCoord(rot, mirror, static_cast<int>(src.w / 2), static_cast<int>(src.h / 2),
                        static_cast<int>(dx), static_cast<int>(dy), sx, sy);
            ok = dst.UV(dx, dy) == src.UV(static_cast<uint32_t>(sx), static_cast<uint32_t>(sy));
          }
        CHECK_MSG(ok, "size %ux%u rot %d mirror %d", src.w, src.h, rot, mirror ? 1 : 0);
      }
    }
  }
}

void testTransformKnown() {
  // 4x2 luma:  a b c d / e f g h  ->  rot90 cw (2x4): e a / f b / g c / h d
  Img s(4, 2);
  const uint8_t v[8] = {'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
  memcpy(s.y.data(), v, 8);
  s.uv = {1, 2, 3, 4};  // two UV pairs: (1,2) (3,4)
  Img d(2, 4);
  transformNV12(s.y.data(), 4, s.uv.data(), 4, 4, 2, 90, false, d.y.data(), 2, d.uv.data(), 2);
  CHECK(memcmp(d.y.data(), "eafbgchd", 8) == 0);
  // UV plane is 2x1 pairs -> rot90 -> 1x2 pairs: (1,2) then (3,4)
  CHECK(d.uv[0] == 1 && d.uv[1] == 2 && d.uv[2] == 3 && d.uv[3] == 4);
  // mirror only: d c b a / h g f e
  Img m(4, 2);
  transformNV12(s.y.data(), 4, s.uv.data(), 4, 4, 2, 0, true, m.y.data(), 4, m.uv.data(), 4);
  CHECK(memcmp(m.y.data(), "dcbahgfe", 8) == 0);
  CHECK(m.uv[0] == 3 && m.uv[1] == 4 && m.uv[2] == 1 && m.uv[3] == 2);
  // rot270: d h / c g / b f / a e
  Img r(2, 4);
  transformNV12(s.y.data(), 4, s.uv.data(), 4, 4, 2, 270, false, r.y.data(), 2, r.uv.data(), 2);
  CHECK(memcmp(r.y.data(), "dhcgbfae", 8) == 0);
  // rot180: h g f e / d c b a
  Img h(4, 2);
  transformNV12(s.y.data(), 4, s.uv.data(), 4, 4, 2, 180, false, h.y.data(), 4, h.uv.data(), 4);
  CHECK(memcmp(h.y.data(), "hgfedcba", 8) == 0);
}

void testNormalizeRotation() {
  CHECK(normalizeRotation(0) == 0);
  CHECK(normalizeRotation(90) == 90);
  CHECK(normalizeRotation(-90) == 270);
  CHECK(normalizeRotation(450) == 90);
  CHECK(normalizeRotation(100) == 90);
  CHECK(normalizeRotation(140) == 180);
  CHECK(normalizeRotation(359) == 0);
  CHECK(normalizeRotation(270 + 360 * 3) == 270);
}

// ------------------------------------------------------------------ clock
void testClockSync() {
  ClockSync cs(16);
  CHECK(!cs.synced());
  const int64_t trueOffset = 3600LL * 1000000 + 12345;  // phone = pc + offset
  std::mt19937 rng(7);
  // Asymmetric queueing on most samples; one clean sample (1 ms each way).
  int64_t t = 1000000;
  for (int i = 0; i < 10; ++i) {
    const int64_t up = (i == 6) ? 1000 : 1000 + static_cast<int64_t>(rng() % 20000);
    const int64_t down = (i == 6) ? 1000 : 1000 + static_cast<int64_t>(rng() % 5000);
    const int64_t t0 = t;
    const int64_t phone = t0 + up + trueOffset;
    const int64_t t1 = t0 + up + down;
    CHECK(cs.addSample(t0, phone, t1));
    t += 500000;
  }
  CHECK(cs.synced());
  CHECK(cs.minRttUs() == 2000);
  CHECK_MSG(std::llabs(cs.offsetUs() - trueOffset) <= 1, "offset %lld", static_cast<long long>(cs.offsetUs()));
  CHECK(cs.phoneToPc(trueOffset + 5000000) == 5000000 + (trueOffset - cs.offsetUs()));
  CHECK(!cs.addSample(10, 0, 5));  // t1 < t0 rejected
  // Window: after 16 newer noisy samples the clean one leaves the window.
  for (int i = 0; i < 16; ++i) {
    const int64_t t0 = t;
    cs.addSample(t0, t0 + 4000 + trueOffset, t0 + 5000);  // rtt 5 ms, asymmetric +1.5 ms
    t += 500000;
  }
  CHECK(cs.minRttUs() == 5000);
  CHECK(std::llabs(cs.offsetUs() - (trueOffset + 1500)) <= 1);
}

// ------------------------------------------------------------------ jitter
void testJitter() {
  JitterEstimator j;
  for (int i = 0; i < 100; ++i) j.add(i * 33333, i * 33333 + 5000);  // constant transit
  CHECK(j.jitterUs() < 1e-9);
  JitterEstimator k;
  // transit alternates 5 ms / 9 ms: |D| = 4 ms each step -> J converges to 4 ms
  for (int i = 0; i < 400; ++i) k.add(i * 33333, i * 33333 + (i % 2 ? 9000 : 5000));
  CHECK_MSG(std::fabs(k.jitterUs() - 4000.0) < 10.0, "jitter %.1f", k.jitterUs());
  // RFC 3550 single step from zero: J = |D|/16
  JitterEstimator s;
  s.add(0, 1000);
  s.add(33333, 33333 + 1000 + 1600);
  CHECK(std::fabs(s.jitterUs() - 100.0) < 1e-6);
}

void testDelayTrend() {
  DelayTrend d(20);
  CHECK(d.gradientMs() == 0);
  // Constant delay -> 0
  for (int i = 0; i < 30; ++i) d.add(i * 33333, i * 33333 + 7000);
  CHECK(std::fabs(d.gradientMs()) < 1e-6);
  // Delay grows 1 ms per frame: across the last 20 frames span (19 intervals) -> +19 ms
  DelayTrend g(20);
  for (int i = 0; i < 40; ++i) {
    const int64_t send = i * 33333;
    g.add(send, send + 5000 + i * 1000);
  }
  CHECK_MSG(std::fabs(g.gradientMs() - 19.0) < 0.1, "gradient %.3f", g.gradientMs());
  // Decreasing -> negative
  DelayTrend n(20);
  for (int i = 0; i < 20; ++i) n.add(i * 33333, i * 33333 + 50000 - i * 500);
  CHECK(n.gradientMs() < -9.0 && n.gradientMs() > -10.0);
}

void testRateAndWindow() {
  RateMeter r;
  for (int i = 0; i <= 60; ++i) r.add(1000000 + i * 33333);
  CHECK_MSG(std::fabs(r.rate(1000000 + 61 * 33333) - 30.0) < 1.5, "rate %.2f", r.rate(1000000 + 61 * 33333));
  SampleWindow w(10);
  for (int i = 1; i <= 20; ++i) w.add(i);  // keeps 11..20
  CHECK(w.size() == 10);
  CHECK(std::fabs(w.avg() - 15.5) < 1e-9);
  CHECK(w.percentile(95) == 20);
  CHECK(w.percentile(0) == 11);
  CHECK(w.max() == 20);
}

void testKeyframePolicy() {
  KeyframePolicy p;
  const int64_t minI = 250000, rtt = 20000, fi = 33333;
  // limit = max(250 ms, 2*20 + 2*33.3 = 106.7 ms) = 250 ms
  CHECK(p.rateLimitUs(minI, rtt, fi) == 250000);
  CHECK(p.rateLimitUs(minI, 200000, fi) == 466666);
  int64_t t = 10000000;
  CHECK(!p.poll(t, false, minI, rtt, fi));
  CHECK(p.poll(t, true, minI, rtt, fi));            // first loss: immediate
  CHECK(!p.poll(t + 100000, true, minI, rtt, fi));  // still needed, too soon
  CHECK(!p.poll(t + 400000, true, minI, rtt, fi));  // in-episode resend waits 500 ms
  CHECK(p.poll(t + 500000, true, minI, rtt, fi));   // resend
  CHECK(!p.poll(t + 600000, false, minI, rtt, fi)); // recovered
  CHECK(!p.poll(t + 700000, true, minI, rtt, fi));  // new episode but < 250 ms since last
  CHECK(p.poll(t + 750000, true, minI, rtt, fi));   // allowed at 250 ms
}

// ------------------------------------------------------------------ adb
void testAdbParseDevices() {
  const std::string text =
      "* daemon not running; starting now at tcp:5037\r\n"
      "* daemon started successfully\r\n"
      "List of devices attached\r\n"
      "25f8e5e431047ece       device product:crownqltezh model:SM_N9600 device:crownqltechn transport_id:30\r\n"
      "emulator-5554          offline transport_id:2\r\n"
      "R58M12ABCDE            unauthorized usb:1-1 transport_id:7\r\n"
      "0123456789ABCDEF       no permissions (user in plugdev group; are your udev rules wrong?); see [http://developer.android.com/tools/device.html] usb:3-2 transport_id:9\r\n"
      "192.168.0.20:5555      device product:x model:Pixel_7 device:panther transport_id:11\r\n"
      "\r\n";
  auto d = Adb::parseDevices(text);
  CHECK(d.size() == 5);
  if (d.size() == 5) {
    CHECK(d[0].serial == "25f8e5e431047ece" && d[0].state == "device" && d[0].online());
    CHECK(d[0].model == "SM_N9600" && d[0].product == "crownqltezh" && d[0].device == "crownqltechn");
    CHECK(d[0].transport_id == 30);
    CHECK(d[1].serial == "emulator-5554" && d[1].state == "offline" && !d[1].online());
    CHECK(d[1].transport_id == 2);
    CHECK(d[2].state == "unauthorized" && d[2].transport_id == 7);
    CHECK(d[3].state.compare(0, 14, "no permissions") == 0 && d[3].transport_id == 9);
    CHECK(d[4].serial == "192.168.0.20:5555" && d[4].model == "Pixel_7");
  }
  // without -l (tab separated)
  auto e = Adb::parseDevices("List of devices attached\nabc\tdevice\n\n");
  CHECK(e.size() == 1 && e[0].serial == "abc" && e[0].state == "device" && e[0].transport_id == -1);
  CHECK(Adb::parseDevices("List of devices attached\n\n").empty());
}

void testAdbParseForward() {
  const std::string text =
      "25f8e5e431047ece tcp:51234 tcp:47800\n"
      "emulator-5554 tcp:6000 localabstract:scrcpy\r\n"
      "garbage line\n"
      "x tcp:notaport tcp:1\n";
  auto f = Adb::parseForwardList(text);
  CHECK(f.size() == 3);
  if (f.size() == 3) {
    CHECK(f[0].serial == "25f8e5e431047ece" && f[0].localTcpPort() == 51234 && f[0].remoteTcpPort() == 47800);
    CHECK(f[1].localTcpPort() == 6000 && f[1].remoteTcpPort() == 0 && f[1].remote == "localabstract:scrcpy");
    CHECK(f[2].localTcpPort() == 0 && f[2].remoteTcpPort() == 1);
  }
  CHECK(Adb::parseForwardList("").empty());
  CHECK(Adb::pickFreeTcpPort() != 0);
}

void testRunProcess() {
  wchar_t sys[MAX_PATH];
  GetSystemDirectoryW(sys, MAX_PATH);
  const std::wstring cmd = std::wstring(sys) + L"\\cmd.exe";
  ProcessResult r = runProcess(cmd, {"/c", "echo hello world& echo oops 1>&2& exit 3"}, 5000);
  CHECK(r.started && !r.timed_out);
  CHECK(r.exit_code == 3);
  CHECK_MSG(r.out.find("hello world") != std::string::npos, "out='%s'", r.out.c_str());
  CHECK_MSG(r.err.find("oops") != std::string::npos, "err='%s'", r.err.c_str());
  const int64_t t0 = steadyMs();
  ProcessResult t = runProcess(std::wstring(sys) + L"\\ping.exe", {"-n", "10", "127.0.0.1"}, 300);
  CHECK(t.started && t.timed_out);
  CHECK_MSG(steadyMs() - t0 < 3000, "timeout took %lld ms", static_cast<long long>(steadyMs() - t0));
  ProcessResult n = runProcess(L"C:\\does\\not\\exist.exe", {}, 1000);
  CHECK(!n.started);
  Adb adb;
  adb.setAdbPath("C:\\does\\not\\adb.exe");
  CHECK(adb.adbPath().empty());  // explicit path is never replaced by a search
  std::vector<AdbDevice> devs;
  std::string err;
  CHECK(!adb.listDevices(devs, &err) && !err.empty());
}

// ------------------------------------------------------------------ annexb
void testAnnexB() {
  const uint8_t au[] = {0, 0, 0, 1, 0x67, 1, 2, 3, 0, 0, 1, 0x68, 4, 0, 0, 0, 1, 0x65, 9, 9, 9, 0};
  std::vector<int> types;
  std::vector<size_t> lens;
  forEachNal(au, sizeof(au), [&](const uint8_t* p, size_t n) {
    types.push_back(nalType(Codec::H264, p[0]));
    lens.push_back(n);
  });
  CHECK(types.size() == 3);
  if (types.size() == 3) {
    CHECK(types[0] == 7 && types[1] == 8 && types[2] == 5);
    CHECK(lens[0] == 4 && lens[1] == 2 && lens[2] == 4);
  }
  CHECK(hasSps(Codec::H264, au, sizeof(au)));
  CHECK(!hasSps(Codec::H264, au + 15, sizeof(au) - 15));
  // HEVC: VPS(32) SPS(33) PPS(34) IDR_W_RADL(19)
  const uint8_t h[] = {0, 0, 1, 0x40, 1, 0, 0, 1, 0x42, 1, 0, 0, 1, 0x44, 1, 0, 0, 1, 0x26, 1, 7};
  CHECK(hasSps(Codec::Hevc, h, sizeof(h)));
  CHECK(isIdrNal(Codec::Hevc, nalType(Codec::Hevc, 0x26)));
  CHECK(isParamSetNal(Codec::Hevc, nalType(Codec::Hevc, 0x40)));
}

// ------------------------------------------------------------------ strip
void testCounterStrip() {
  const uint32_t w = 640, h = 480;
  std::vector<uint8_t> f(w * h * 3 / 2);
  for (uint32_t v : {0u, 1u, 0x80000000u, 0xDEADBEEFu, 123456789u}) {
    owc::fake::drawTestFrame(f.data(), w, h, v);
    uint32_t got = 0;
    CHECK(owc::fake::readCounterStrip(f.data(), w, w, h, got));
    CHECK(got == v);
    // UV inside the strip is neutral
    CHECK(f[w * h + (h - 32) / 2 * w + 16] == 128);
  }
  uint32_t dummy;
  CHECK(!owc::fake::readCounterStrip(f.data(), w, 500, h, dummy));  // too narrow
  std::vector<uint8_t> gray(w * h * 3 / 2, 128);
  CHECK(!owc::fake::readCounterStrip(gray.data(), w, w, h, dummy));  // ambiguous
}

// ------------------------------------------------------------------ presets
void testPresets() {
  const auto& p = builtinPresets();
  CHECK(p.size() == 3);
  CHECK(builtinPreset(PresetId::UltraLowLatency).jitter_buffer_ms == 0);
  CHECK(builtinPreset(PresetId::MaxQuality).jitter_buffer_ms > builtinPreset(PresetId::Balanced).jitter_buffer_ms);
  CHECK(builtinPreset(PresetId::MaxQuality).low_latency_decode);
  ClientOptions o;
  o.transform.rotation_deg = 90;
  builtinPreset(PresetId::Balanced).applyTo(o);
  CHECK(o.jitter_buffer_ms == 40 && o.transform.rotation_deg == 90);
  bool hasB = false;
  for (auto& kv : builtinPreset(PresetId::MaxQuality).phone)
    if (kv.first == ParamId::BFrames && kv.second.i == 2) hasB = true;
  CHECK(hasB);
  for (auto& x : p) CHECK(x.name && x.name[0] && x.key && x.key[0]);
}

void testDiscoveryHelpers() {
  auto h = Discovery::subnetHosts("192.168.0.77/24");
  CHECK(h.size() == 254 && h.front() == "192.168.0.1" && h.back() == "192.168.0.254");
  CHECK(Discovery::subnetHosts("10.1.4.9/22").size() == 1022);
  CHECK(Discovery::subnetHosts("10.1.4.9/22").front() == "10.1.4.1");
  CHECK(Discovery::subnetHosts("10.0.0.0/21").empty());  // too big
  CHECK(Discovery::subnetHosts("127.0.0.0/29").size() == 6);
  CHECK(Discovery::subnetHosts("1.2.3.4/32").size() == 1);
  CHECK(Discovery::subnetHosts("garbage").empty());
  CHECK(Discovery::subnetHosts("1.2.3.4/33").empty());
  auto ip = [](int a, int b, int c, int d) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
  };
  CHECK(Discovery::isPrivateIPv4(ip(192, 168, 0, 142)));
  CHECK(Discovery::isPrivateIPv4(ip(10, 140, 29, 7)));
  CHECK(Discovery::isPrivateIPv4(ip(172, 20, 47, 1)) && Discovery::isPrivateIPv4(ip(172, 31, 255, 1)));
  CHECK(!Discovery::isPrivateIPv4(ip(172, 32, 0, 1)) && !Discovery::isPrivateIPv4(ip(8, 8, 8, 8)));
  CHECK(Discovery::isVirtualAdapter("vEthernet (WSL)", "Hyper-V Virtual Ethernet Adapter"));
  CHECK(Discovery::isVirtualAdapter("Ethernet 3", "TAP-Windows Adapter V9"));
  CHECK(Discovery::isVirtualAdapter("NordLynx", "NordLynx Tunnel"));
  CHECK(!Discovery::isVirtualAdapter("Wi-Fi", "Intel(R) Wi-Fi 6 AX201 160MHz"));
  CHECK(!Discovery::isVirtualAdapter("Ethernet", "Realtek PCIe GbE Family Controller"));
  const std::string cmd = Firewall::addRuleCommandLine();
  CHECK(cmd.find("netsh advfirewall firewall add rule") == 0);
  CHECK(cmd.find("localport=47803") != std::string::npos && cmd.find("profile=private") != std::string::npos);
  CHECK(cmd.find(Firewall::ruleName()) != std::string::npos);
  std::printf("  firewall rule present: %d\n", Firewall::rulesPresent() ? 1 : 0);
}

void testCodecSelection() {
  DecoderCapabilities none, hevcHw, hevcSw, all;
  none.h264.software = none.h264.hardware = true;
  hevcHw = none;
  hevcHw.hevc.hardware = hevcHw.hevc.software = true;
  hevcSw = none;
  hevcSw.hevc.software = true;
  all = hevcHw;
  all.av1.hardware = true;
  const uint32_t h264 = codecBit(Codec::H264), hevc = codecBit(Codec::Hevc), av1 = codecBit(Codec::Av1);
  auto c = chooseCodec(CodecPreference::Auto, h264 | hevc, hevcHw, 0);
  CHECK(c.codec == Codec::Hevc && c.reason == "auto: HEVC (hardware decoder present)");
  c = chooseCodec(CodecPreference::Auto, h264 | hevc, none, 0);
  CHECK(c.codec == Codec::H264);
  CHECK_MSG(c.reason == "auto: H.264 (no HEVC decoder: install 'HEVC Video Extensions')", "%s", c.reason.c_str());
  c = chooseCodec(CodecPreference::Auto, h264 | hevc, hevcSw, 0);  // SW-only HEVC: not in Auto
  CHECK(c.codec == Codec::H264 && c.reason.find("software-only") != std::string::npos);
  c = chooseCodec(CodecPreference::Auto, h264, all, 0);
  CHECK(c.codec == Codec::H264 && c.reason == "auto: H.264 (phone encodes H.264 only)");
  c = chooseCodec(CodecPreference::Auto, h264 | hevc | av1, all, 0);  // AV1 > HEVC
  CHECK(c.codec == Codec::Av1);
  c = chooseCodec(CodecPreference::Auto, h264 | hevc | av1, all, av1);  // AV1 failed -> HEVC
  CHECK(c.codec == Codec::Hevc);
  c = chooseCodec(CodecPreference::Auto, h264 | hevc, hevcHw, hevc);  // HEVC failed -> H.264
  CHECK(c.codec == Codec::H264 && c.reason.find("failed earlier") != std::string::npos);
  c = chooseCodec(CodecPreference::Hevc, h264 | hevc, hevcSw, 0);  // explicit: SW is fine
  CHECK(c.codec == Codec::Hevc && c.reason == "user: HEVC (software decoder)");
  c = chooseCodec(CodecPreference::Hevc, h264, hevcHw, 0);
  CHECK(c.codec == Codec::H264 && c.reason.find("phone has no HEVC encoder") != std::string::npos);
  c = chooseCodec(CodecPreference::Av1, h264 | av1, hevcHw, 0);
  CHECK(c.codec == Codec::H264 && c.reason.find("AV1 Video Extension") != std::string::npos);
  c = chooseCodec(CodecPreference::H264, h264 | hevc, all, 0);
  CHECK(c.codec == Codec::H264 && c.reason == "user: H.264");
  const DecoderCapabilities real = Client::decoderCapabilities();
  std::printf("  this PC: H.264 hw %d sw %d | HEVC hw %d sw %d | AV1 hw %d sw %d | adapter '%s' discrete %d (%llu MB)\n",
              real.h264.hardware, real.h264.software, real.hevc.hardware, real.hevc.software,
              real.av1.hardware, real.av1.software, real.adapter.c_str(), real.adapter_discrete,
              static_cast<unsigned long long>(real.dedicated_video_memory >> 20));
  CHECK(real.h264.any());
}

void testLinkClassification() {
  using L = LinkType;
  const uint32_t eth = 6, wifi = 71, other = 1;
  // Android USB tethering drivers (Windows 10/11)
  CHECK(Discovery::classifyAdapter(eth, "Ethernet 5", "Remote NDIS based Internet Sharing Device") == L::UsbTether);
  CHECK(Discovery::classifyAdapter(eth, "Ethernet 3", "Remote NDIS Compatible Device #2") == L::UsbTether);
  CHECK(Discovery::classifyAdapter(eth, "Ethernet 4", "UsbNcm Host Device") == L::UsbTether);
  CHECK(Discovery::classifyAdapter(eth, "Ethernet 6", "USB NCM Device") == L::UsbTether);
  CHECK(Discovery::classifyAdapter(other, "Ethernet 7", "Android USB Ethernet/RNDIS") == L::UsbTether);
  CHECK(Discovery::classifyAdapter(eth, "RNDIS phone", "Generic adapter") == L::UsbTether);
  // not tethering
  CHECK(Discovery::classifyAdapter(wifi, "Wi-Fi", "Intel(R) Wi-Fi 6 AX201 160MHz") == L::Wifi);
  CHECK(Discovery::classifyAdapter(eth, "Ethernet", "Realtek PCIe GbE Family Controller") == L::Ethernet);
  CHECK(Discovery::classifyAdapter(eth, "Ethernet 2", "Realtek USB GbE Family Controller") == L::Ethernet);
  CHECK(Discovery::classifyAdapter(eth, "vEthernet (WSL)", "Hyper-V Virtual Ethernet Adapter") == L::Other);
  CHECK(Discovery::classifyAdapter(other, "Bluetooth Network Connection", "Bluetooth Device (PAN)") == L::Other);
  CHECK(Discovery::classifyAdapter(eth, "Ethernet", "Broadcom NetXtreme Gigabit Ethernet") == L::Ethernet);
  CHECK(Discovery::classifyAdapter(eth, "Ethernet", "Syncmaster dock ethernet") == L::Ethernet);  // no "ncm" token
  // the USB tether adapter must survive the sweep's virtual-adapter filter
  CHECK(!Discovery::isVirtualAdapter("Ethernet 5", "Remote NDIS based Internet Sharing Device"));
  CHECK(Discovery::isPrivateIPv4((192u << 24) | (168u << 16) | (42u << 8) | 129u));
  CHECK(Discovery::subnetHosts("192.168.42.129/24").size() == 254);

  // linkFor + usbTetherAdapters with a synthetic adapter table
  std::vector<Discovery::LocalAdapter> ad(3);
  ad[0].name = "Wi-Fi";       ad[0].ipv4 = "192.168.0.108";  ad[0].prefix = 24; ad[0].link = L::Wifi;
  ad[0].has_default_gateway = true; ad[0].ipv4_metric = 35;
  ad[1].name = "Ethernet 5";  ad[1].ipv4 = "192.168.42.17";  ad[1].prefix = 24; ad[1].link = L::UsbTether;
  ad[1].has_default_gateway = true; ad[1].ipv4_metric = 25;  // lower metric than Wi-Fi!
  ad[2].name = "vEthernet";   ad[2].ipv4 = "172.20.32.1";    ad[2].prefix = 20; ad[2].link = L::Other;
  std::string name;
  CHECK(Discovery::linkFor("192.168.42.129", ad, &name) == L::UsbTether && name == "Ethernet 5");
  CHECK(Discovery::linkFor("192.168.0.142", ad, &name) == L::Wifi && name == "Wi-Fi");
  CHECK(Discovery::linkFor("172.20.40.2", ad) == L::Other);
  CHECK(Discovery::linkFor("127.0.0.1", ad) == L::Unknown);
  CHECK(Discovery::linkFor("garbage", ad) == L::Unknown);
  auto t = Discovery::usbTetherAdapters(ad);
  CHECK(t.size() == 1 && t[0].ipv4 == "192.168.42.17" && t[0].has_default_gateway && t[0].may_carry_internet);
  ad[1].ipv4_metric = 50;  // Wi-Fi preferred now
  t = Discovery::usbTetherAdapters(ad);
  CHECK(t.size() == 1 && !t[0].may_carry_internet);
  ad[1].has_default_gateway = false;
  t = Discovery::usbTetherAdapters(ad);
  CHECK(t.size() == 1 && !t[0].may_carry_internet);

  // same phone on Wi-Fi and USB tether -> keep the tether entry
  DiscoveredDevice w, u, other2;
  w.name = u.name = "SM-N9600";
  w.model = u.model = "SM-N9600";
  w.ip = "192.168.0.142";  w.link = L::Wifi;
  u.ip = "192.168.42.129"; u.link = L::UsbTether;
  other2.name = "Pixel"; other2.model = "Pixel 7"; other2.ip = "192.168.0.50"; other2.link = L::Wifi;
  auto best = Discovery::preferBestLink({w, other2, u});
  CHECK(best.size() == 2 && best[0].ip == "192.168.42.129" && best[1].name == "Pixel");
  best = Discovery::preferBestLink({u, w});
  CHECK(best.size() == 1 && best[0].link == L::UsbTether);
  CHECK(std::string(toString(L::UsbTether)) == "USB tethering");

  for (const auto& a : Discovery::localAdapters())
    std::printf("  local adapter '%s' (%s) %s/%u type %u gw %d metric %u -> %s\n", a.name.c_str(),
                a.description.c_str(), a.ipv4.c_str(), a.prefix, a.if_type, a.has_default_gateway ? 1 : 0,
                a.ipv4_metric, toString(a.link));
}

Capabilities mockPhoneCaps() {
  Capabilities c;
  auto add = [&](ParamId id, ValueType t, double mn, double mx, ParamValue cur,
                 std::vector<EnumOption> opts = {}) {
    ParamDesc d;
    d.id = id;
    d.type = t;
    d.flags = kParamSupported;
    d.min = mn;
    d.max = mx;
    d.current = cur;
    d.options = opts;
    c.params.push_back(d);
  };
  add(ParamId::VideoWidth, ValueType::Int32, 160, 3840, ParamValue::Int(1920));
  add(ParamId::VideoHeight, ValueType::Int32, 120, 2160, ParamValue::Int(1080));
  add(ParamId::Fps, ValueType::Int32, 1, 240, ParamValue::Int(30));
  add(ParamId::VideoCodec, ValueType::Enum, 0, 1, ParamValue::EnumV(0), {{0, "H.264"}, {1, "HEVC"}});
  add(ParamId::Bitrate, ValueType::Int32, 100000, 50000000, ParamValue::Int(8000000));
  add(ParamId::BitrateMode, ValueType::Enum, 0, 2, ParamValue::EnumV(0), {{0, "CBR"}, {1, "VBR"}, {2, "CQ"}});
  add(ParamId::AdaptiveBitrate, ValueType::Bool, 0, 1, ParamValue::Bool(true));
  add(ParamId::LatencyDropPolicy, ValueType::Enum, 0, 1, ParamValue::EnumV(0));
  add(ParamId::MaxSendQueueFrames, ValueType::Int32, 1, 60, ParamValue::Int(2));
  add(ParamId::KeyframeIntervalMs, ValueType::Int32, 0, 10000, ParamValue::Int(1000));
  add(ParamId::BFrames, ValueType::Int32, 0, 4, ParamValue::Int(0));
  add(ParamId::FecPercent, ValueType::Int32, -1, 100, ParamValue::Int(-1));
  add(ParamId::HighSpeedMode, ValueType::Enum, 0, 1, ParamValue::EnumV(0));
  return c;
}

bool planHas(const RecordPlan& p, ParamId id, int64_t v) {
  for (auto& kv : p.sets)
    if (kv.first == id) return kv.second.i == v;
  return false;
}
bool planSets(const RecordPlan& p, ParamId id) {
  for (auto& kv : p.sets)
    if (kv.first == id) return true;
  return false;
}

void testRecordSetupPlanner() {
  const Capabilities caps = mockPhoneCaps();
  DecoderCapabilities noHevc;
  noHevc.h264.hardware = noHevc.h264.software = true;
  const uint32_t both = codecBit(Codec::H264) | codecBit(Codec::Hevc);
  // bitrate rule
  CHECK(qualityBitrate(1920, 1080, 30) == 20000000u);
  CHECK_MSG(qualityBitrate(1280, 720, 240) >= 38000000u && qualityBitrate(1280, 720, 240) <= 42000000u,
            "720p240 %u", qualityBitrate(1280, 720, 240));
  CHECK_MSG(qualityBitrate(3840, 2160, 30) >= 57000000u && qualityBitrate(3840, 2160, 30) <= 63000000u,
            "4K30 %u", qualityBitrate(3840, 2160, 30));
  // (1) Quality slow motion: HEVC though the PC can't decode it -> preview off
  RecordPreset sm;
  sm.mode = RecordMode::SlowMotion;
  RecordPlan p = planRecording(sm, caps, both, noHevc, VideoTransport::Tcp);
  CHECK(p.status.codec == Codec::Hevc && !p.status.preview_available && p.forceCodec && p.requestedHevc);
  CHECK(p.status.width == 1280 && p.status.height == 720 && p.status.fps == 240);
  CHECK(planHas(p, ParamId::HighSpeedMode, 1) && planHas(p, ParamId::VideoCodec, 1) &&
        planHas(p, ParamId::Fps, 240) && planHas(p, ParamId::VideoWidth, 1280));
  CHECK(planHas(p, ParamId::AdaptiveBitrate, 0) && planHas(p, ParamId::LatencyDropPolicy, 1) &&
        planHas(p, ParamId::MaxSendQueueFrames, 60) && planHas(p, ParamId::BitrateMode, 1) &&
        planHas(p, ParamId::BFrames, 0) && planHas(p, ParamId::FecPercent, 0));
  CHECK(!planSets(p, ParamId::KeyframeIntervalMs));  // > 60 fps: the phone caps its own GOP
  CHECK(p.maxPresentFps == 60);
  CHECK(p.status.bitrate_bps >= 38000000u && p.status.bitrate_bps <= 42000000u);
  CHECK_MSG(p.status.summary == "Qualidade: HEVC 1280x720 @ 240 fps, 40 Mbps, sem preview (HEVC sem decoder)",
            "%s", p.status.summary.c_str());
  // order: high-speed + codec before geometry, geometry before rate control
  CHECK(p.sets.size() > 4 && p.sets[0].first == ParamId::HighSpeedMode && p.sets[1].first == ParamId::VideoCodec);
  // (2) Quality real time, keep geometry: only quality knobs
  RecordPreset rt;
  p = planRecording(rt, caps, both, noHevc, VideoTransport::Tcp);
  CHECK(!planSets(p, ParamId::VideoWidth) && !planSets(p, ParamId::Fps) && !planSets(p, ParamId::HighSpeedMode));
  CHECK(planHas(p, ParamId::Bitrate, 20000000) && planHas(p, ParamId::KeyframeIntervalMs, 2000) &&
        planHas(p, ParamId::MaxSendQueueFrames, 32));
  CHECK(p.status.width == 1920 && p.status.fps == 30 && p.maxPresentFps == 0);
  // (3) don't allow a codec the PC can't decode -> H.264, preview on
  rt.allow_codec_without_pc_decoder = false;
  p = planRecording(rt, caps, both, noHevc, VideoTransport::Tcp);
  CHECK(p.status.codec == Codec::H264 && p.status.preview_available && planHas(p, ParamId::VideoCodec, 0));
  // (4) phone without HEVC
  rt.allow_codec_without_pc_decoder = true;
  p = planRecording(rt, caps, codecBit(Codec::H264), noHevc, VideoTransport::Tcp);
  CHECK(p.status.codec == Codec::H264 && !p.requestedHevc);
  // (5) PC with HEVC decode: HEVC + preview
  DecoderCapabilities hevc = noHevc;
  hevc.hevc.hardware = true;
  p = planRecording(rt, caps, both, hevc, VideoTransport::Udp);
  CHECK(p.status.codec == Codec::Hevc && p.status.preview_available);
  CHECK(!planSets(p, ParamId::FecPercent));  // UDP keeps FEC
  // (6) explicit 1080p30 leaves high-speed mode
  RecordPreset hd;
  hd.width = 1920;
  hd.height = 1080;
  hd.fps = 30;
  p = planRecording(hd, caps, both, noHevc, VideoTransport::Tcp);
  CHECK(planHas(p, ParamId::HighSpeedMode, 0) && planHas(p, ParamId::Fps, 30));
  // (7) Latency real time = change nothing; Latency slow motion = legacy knobs
  RecordPreset lt;
  lt.priority = RecordPriority::Latency;
  p = planRecording(lt, caps, both, noHevc, VideoTransport::Tcp);
  CHECK(p.sets.empty() && !p.forceCodec && p.status.summary.find("Latência") == 0);
  lt.mode = RecordMode::SlowMotion;
  p = planRecording(lt, caps, both, noHevc, VideoTransport::Tcp);
  CHECK(!p.forceCodec && !planSets(p, ParamId::VideoCodec) && !planSets(p, ParamId::BitrateMode));
  CHECK(planHas(p, ParamId::Bitrate, 40000000) && planHas(p, ParamId::KeyframeIntervalMs, 1000) &&
        planHas(p, ParamId::MaxSendQueueFrames, 32) && planHas(p, ParamId::HighSpeedMode, 1));
  // (8) unsupported params are skipped; bitrate clamped to the phone's max
  Capabilities small = caps;
  for (auto& d : small.params) {
    if (d.id == ParamId::BitrateMode) d.flags = 0;
    if (d.id == ParamId::Bitrate) d.max = 30000000;
  }
  p = planRecording(sm, small, both, noHevc, VideoTransport::Tcp);
  CHECK(!planSets(p, ParamId::BitrateMode) && planHas(p, ParamId::Bitrate, 30000000));
}

void testQpcConversions() {
  for (int64_t us : {0LL, 1LL, 999999LL, 1000000LL, 123456789012LL}) {
    const int64_t q = usToQpc(us);
    CHECK(std::llabs(qpcToUs(q) - us) <= 1);
  }
}

}  // namespace

int main() {
  RUN(testTransformKnown);
  RUN(testTransformRandom);
  RUN(testNormalizeRotation);
  RUN(testClockSync);
  RUN(testJitter);
  RUN(testDelayTrend);
  RUN(testRateAndWindow);
  RUN(testKeyframePolicy);
  RUN(testAdbParseDevices);
  RUN(testAdbParseForward);
  RUN(testRunProcess);
  RUN(testAnnexB);
  RUN(testCounterStrip);
  RUN(testPresets);
  RUN(testQpcConversions);
  RUN(testDiscoveryHelpers);
  RUN(testCodecSelection);
  RUN(testLinkClassification);
  RUN(testRecordSetupPlanner);
  std::printf("%s (%d failure(s))\n", owctest::failures() ? "FAILED" : "ALL PASSED", owctest::failures());
  return owctest::failures() ? 1 : 0;
}
