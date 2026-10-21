// owc_core unit tests: transforms, clock sync, jitter/delay trend, keyframe
// policy, adb parsing, process exec, Annex-B helpers, counter strip, presets,
// colour grading (.cube parsing, YUV table bake/apply, export).
#include "test_util.h"

#include "annexb.h"
#include "codec_select.h"
#include "color_grade.h"
#include "record_setup.h"
#include "metrics.h"
#include "transform.h"
#include "util.h"

#include "pattern.h"

#include <owc/adb.h>
#include <owc/color_grade.h>
#include <owc/discovery.h>
#include <owc/firewall.h>
#include <owc/presets.h>
#include <owc/scopes.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <random>
#include <string>
#include <thread>
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

// ------------------------------------------------------------------ colour grading
std::string tempPath(const char* name) {
  wchar_t buf[MAX_PATH];
  GetTempPathW(MAX_PATH, buf);
  return wideToUtf8(buf) + name;
}

bool writeText(const std::string& path, const std::string& text) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, utf8ToWide(path).c_str(), L"wb") != 0 || !f) return false;
  const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  fclose(f);
  return ok;
}

const char* kIdentityCube2 =
    "# identity, size 2\n"
    "TITLE \"Ident\"\n"
    "\n"
    "LUT_3D_SIZE 2\n"
    "0 0 0\n1 0 0\n0 1 0\n1 1 0\n"
    "0 0 1\n1 0 1\n0 1 1\n1 1 1\n";

Img randomNv12(uint32_t w, uint32_t h, uint32_t seed) {
  std::mt19937 rng(seed);
  Img im(w, h);
  for (auto& b : im.y) b = static_cast<uint8_t>(rng());
  for (auto& b : im.uv) b = static_cast<uint8_t>(rng());
  return im;
}

// Horizontal luma ramp, chroma ramps in both directions (covers every code).
Img gradientNv12(uint32_t w, uint32_t h) {
  Img im(w, h);
  for (uint32_t r = 0; r < h; ++r)
    for (uint32_t x = 0; x < w; ++x) im.y[r * w + x] = static_cast<uint8_t>(x * 255 / (w - 1));
  for (uint32_t r = 0; r < h / 2; ++r)
    for (uint32_t x = 0; x < w / 2; ++x) {
      im.uv[r * w + 2 * x] = static_cast<uint8_t>(x * 255 / (w / 2 - 1));
      im.uv[r * w + 2 * x + 1] = static_cast<uint8_t>(r * 255 / (h / 2 - 1));
    }
  return im;
}

// In-gamut image: random RGB per 2x2 block, converted with the given matrix/range.
Img rgbBlocksNv12(uint32_t w, uint32_t h, uint32_t matrix, bool full, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> d(0, 1);
  const float kr = matrix == 1 ? 0.299f : 0.2126f, kb = matrix == 1 ? 0.114f : 0.0722f;
  Img im(w, h);
  for (uint32_t r = 0; r < h / 2; ++r)
    for (uint32_t x = 0; x < w / 2; ++x) {
      const float R = d(rng), G = d(rng), B = d(rng);
      const float Y = kr * R + (1 - kr - kb) * G + kb * B;
      const float cb = (B - Y) / (2 * (1 - kb)), cr = (R - Y) / (2 * (1 - kr));
      const uint8_t yc = static_cast<uint8_t>(std::lround(full ? Y * 255 : 16 + Y * 219));
      im.y[2 * r * w + 2 * x] = im.y[2 * r * w + 2 * x + 1] = yc;
      im.y[(2 * r + 1) * w + 2 * x] = im.y[(2 * r + 1) * w + 2 * x + 1] = yc;
      im.uv[r * w + 2 * x] = static_cast<uint8_t>(std::min(255L, std::lround(128 + cb * (full ? 255 : 224))));
      im.uv[r * w + 2 * x + 1] = static_cast<uint8_t>(std::min(255L, std::lround(128 + cr * (full ? 255 : 224))));
    }
  return im;
}

void copyOut(const uint8_t* y, uint32_t ys, const uint8_t* uv, uint32_t uvs, Img& out) {
  for (uint32_t r = 0; r < out.h; ++r) memcpy(&out.y[r * out.w], y + static_cast<size_t>(r) * ys, out.w);
  for (uint32_t r = 0; r < out.h / 2; ++r)
    memcpy(&out.uv[r * out.w], uv + static_cast<size_t>(r) * uvs, out.w);
}

// One frame through the grader, waiting (bounded) for the bake of `g`.
bool gradeFrame(ColorGrader& gr, const ColorGrade& g, uint32_t matrix, bool full, const Img& in,
                Img& out, std::vector<uint8_t>& buf) {
  const uint8_t* y = in.y.data();
  const uint8_t* uv = in.uv.data();
  uint32_t ys = in.w, uvs = in.w;
  gr.apply(g, matrix, full, y, ys, uv, uvs, in.w, in.h, buf);  // requests the bake
  if (!gr.waitBaked(10000)) return false;
  y = in.y.data();
  uv = in.uv.data();
  ys = uvs = in.w;
  if (!gr.apply(g, matrix, full, y, ys, uv, uvs, in.w, in.h, buf)) return false;
  copyOut(y, ys, uv, uvs, out);
  return true;
}

void maxDiff(const Img& a, const Img& b, int& dy, int& duv) {
  dy = duv = 0;
  for (size_t i = 0; i < a.y.size(); ++i) dy = std::max(dy, std::abs(a.y[i] - b.y[i]));
  for (size_t i = 0; i < a.uv.size(); ++i) duv = std::max(duv, std::abs(a.uv[i] - b.uv[i]));
}

void testCubeParse() {
  CubeLut c;
  std::string err;
  // 3D size 2 identity with comments, blank lines and a quoted title
  CHECK(parseCube(kIdentityCube2, c, err));
  CHECK(c.is3d && c.size == 2 && c.title == "Ident" && c.rgb.size() == 24);
  CHECK(c.rgb[3] == 1 && c.rgb[4] == 0 && c.rgb[23] == 1);
  // CRLF, BOM, unquoted title, inline comment, 1D with DOMAIN
  CHECK(parseCube("\xEF\xBB\xBFTITLE plain name\r\nLUT_1D_SIZE 3\r\nDOMAIN_MIN 0 0 0\r\n"
                  "DOMAIN_MAX 2 2 4 # wide\r\n0 0 0\r\n0.5 0.25 1e-1\r\n+1 1 1\r\n",
                  c, err));
  CHECK(!c.is3d && c.size == 3 && c.title == "plain name" && c.rgb.size() == 9);
  CHECK(c.domain_max[0] == 2 && c.domain_max[2] == 4 && c.domain_min[1] == 0);
  CHECK(std::fabs(c.rgb[5] - 0.1f) < 1e-7f);
  // LUT_3D_INPUT_RANGE sets every channel's domain
  CHECK(parseCube("LUT_3D_SIZE 2\nLUT_3D_INPUT_RANGE -0.5 1.5\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n"
                  "0 0 1\n1 0 1\n0 1 1\n1 1 1\n",
                  c, err));
  CHECK(c.domain_min[2] == -0.5f && c.domain_max[0] == 1.5f);
  // malformed -> false + line number
  auto bad = [&](const char* text, const char* expect) {
    CubeLut x;
    std::string e;
    const bool ok = parseCube(text, x, e);
    CHECK_MSG(!ok && e.find(expect) != std::string::npos, "err='%s' expected '%s'", e.c_str(), expect);
  };
  bad("LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n", "linha 8");  // 7 of 8
  bad("LUT_3D_SIZE 2\n0 0 0\n1 x 0\n", "linha 3: número inválido");
  bad("# c\nLUT_3D_SIZE 300\n", "linha 2");
  bad("LUT_1D_SIZE 1\n", "linha 1");
  bad("LUT_3D_SIZE 2\n0 0\n", "linha 2: esperados 3");
  bad("LUT_3D_SIZE 2\n0 0 0 0\n", "linha 2: esperados 3");
  bad("0 0 0\n", "linha 1");
  bad("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n1 1 1\n", "linha 4");
  bad("TITLE \"x\"\n", "ausente");
  bad("LUT_1D_SIZE 2\nDOMAIN_MAX 0 1 1\n0 0 0\n1 1 1\n", "DOMAIN");
  // file loading
  CHECK(!loadCubeFile(tempPath("owc_does_not_exist_42.cube"), c, err) && !err.empty());
}

void testLutLookup() {
  // 1D, domain 0..2: input 1 -> middle entry
  CubeLut l1;
  std::string err;
  CHECK(parseCube("LUT_1D_SIZE 3\nDOMAIN_MAX 2 2 2\n0 0 0\n0.2 0.4 0.6\n1 1 1\n", l1, err));
  float in[3] = {1, 1, 0.5f}, out[3];
  lookupLut(l1, in, out);
  CHECK(std::fabs(out[0] - 0.2f) < 1e-6f && std::fabs(out[1] - 0.4f) < 1e-6f &&
        std::fabs(out[2] - 0.3f) < 1e-6f);
  // 3D of an affine map: tetrahedral interpolation reproduces it exactly
  CubeLut l3;
  l3.size = 5;
  auto f = [](const float* c, float* o) {
    o[0] = 0.1f + 0.5f * c[0] + 0.2f * c[1] + 0.1f * c[2];
    o[1] = 0.3f * c[0] + 0.6f * c[1];
    o[2] = 0.9f - 0.4f * c[2] + 0.05f * c[0];
  };
  for (int b = 0; b < 5; ++b)
    for (int g = 0; g < 5; ++g)
      for (int r = 0; r < 5; ++r) {
        const float c[3] = {r / 4.0f, g / 4.0f, b / 4.0f};
        float o[3];
        f(c, o);
        l3.rgb.insert(l3.rgb.end(), o, o + 3);
      }
  std::mt19937 rng(7);
  std::uniform_real_distribution<float> d(0, 1);
  float worst = 0;
  for (int i = 0; i < 2000; ++i) {
    const float c[3] = {d(rng), d(rng), d(rng)};
    float o[3], e[3];
    lookupLut(l3, c, o);
    f(c, e);
    for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(o[k] - e[k]));
  }
  CHECK_MSG(worst < 1e-5f, "worst %g", worst);
  // every built-in look exists, 33^3, values in 0..1
  for (const BuiltinLook& lk : builtinLooks()) {
    auto lut = builtinLookLut(lk.key);
    CHECK_MSG(lut && lut->is3d && lut->size == 33, "%s", lk.key);
    if (!lut) continue;
    float mn = 1, mx = 0;
    for (float v : lut->rgb) {
      mn = std::min(mn, v);
      mx = std::max(mx, v);
    }
    CHECK_MSG(mn >= 0 && mx <= 1 && mx - mn > 0.5f, "%s range %g..%g", lk.key, mn, mx);
  }
  CHECK(!builtinLookLut("nope"));
}

void testGradeCurvesMonotonic() {
  // neutral pipeline: exact identity, out-of-range values included
  GradePipeline p0;
  p0.prepare(ColorGrade(), nullptr);
  CHECK(p0.noop());
  std::mt19937 rng(3);
  std::uniform_real_distribution<float> d(-1, 2);
  bool exact = true;
  for (int i = 0; i < 1000; ++i) {
    float c[3] = {d(rng), d(rng), d(rng)};
    const float o[3] = {c[0], c[1], c[2]};
    p0.run(c);
    exact = exact && c[0] == o[0] && c[1] == o[1] && c[2] == o[2];
  }
  CHECK(exact);
  // every tone control keeps a grey ramp monotonic (no inversion at the extremes)
  struct Case {
    const char* name;
    float ev, con, hi, sh, temp;
  } cases[] = {{"contrast+1", 0, 1, 0, 0, 0},   {"contrast-1", 0, -1, 0, 0, 0},
               {"shadows+1", 0, 0, 0, 1, 0},    {"shadows-1", 0, 0, 0, -1, 0},
               {"highlights+1", 0, 0, 1, 0, 0}, {"highlights-1", 0, 0, -1, 0, 0},
               {"ev+3", 3, 0, 0, 0, 0},         {"ev+0.3", 0.3f, 0, 0, 0, 0},
               {"ev-3", -3, 0, 0, 0, 0},        {"temp+1", 0, 0, 0, 0, 1},
               {"temp-1 ev+1", 1, 0, 0, 0, -1}, {"all", 1, 1, -1, 1, 0.5f}};
  for (const Case& cs : cases) {
    ColorGrade g;
    g.exposure_ev = cs.ev;
    g.contrast = cs.con;
    g.highlights = cs.hi;
    g.shadows = cs.sh;
    g.temperature = cs.temp;
    GradePipeline p;
    p.prepare(g, nullptr);
    float prev[3] = {-1, -1, -1};
    bool mono = true;
    for (int i = 0; i <= 1000; ++i) {
      float c[3] = {i / 1000.0f, i / 1000.0f, i / 1000.0f};
      p.run(c);
      for (int k = 0; k < 3; ++k) {
        mono = mono && c[k] >= prev[k] - 1e-5f;
        prev[k] = c[k];
      }
    }
    CHECK_MSG(mono, "%s", cs.name);
  }
  // exposure +3 keeps white at white (shoulder, no hard clip of the mids)
  ColorGrade g;
  g.exposure_ev = 1;
  GradePipeline p;
  p.prepare(g, nullptr);
  float w[3] = {1, 1, 1}, m[3] = {0.5f, 0.5f, 0.5f};
  p.run(w);
  p.run(m);
  CHECK(std::fabs(w[0] - 1) < 1e-3f && m[0] > 0.62f && m[0] < 0.75f);
  // out-of-gamut input: a neutral grade (identity LUT) keeps it, B&W greys it
  CubeLut ident;
  std::string err;
  CHECK(parseCube(kIdentityCube2, ident, err));
  ColorGrade lg;
  GradePipeline pid, pbw;
  pid.prepare(lg, std::make_shared<CubeLut>(ident));
  pbw.prepare(lg, builtinLookLut("bw"));
  const float oog[][3] = {{1.2f, -0.1f, 0.3f}, {-0.2f, 0.4f, 1.3f}, {1.1f, 1.0f, 1.0f}, {0.1f, -0.05f, 0.0f}};
  for (const auto& v : oog) {
    float a[3] = {v[0], v[1], v[2]}, b[3] = {v[0], v[1], v[2]};
    pid.run(a);
    pbw.run(b);
    CHECK_MSG(std::fabs(a[0] - v[0]) < 1e-4f && std::fabs(a[1] - v[1]) < 1e-4f && std::fabs(a[2] - v[2]) < 1e-4f,
              "identity (%g %g %g) -> (%g %g %g)", v[0], v[1], v[2], a[0], a[1], a[2]);
    CHECK_MSG(std::fabs(b[0] - b[1]) < 2e-3f && std::fabs(b[1] - b[2]) < 2e-3f, "bw (%g %g %g) -> (%g %g %g)",
              v[0], v[1], v[2], b[0], b[1], b[2]);
  }
  // saturation -1 = grey
  g = ColorGrade();
  g.saturation = -1;
  p.prepare(g, nullptr);
  float c[3] = {0.9f, 0.3f, 0.1f};
  p.run(c);
  CHECK(std::fabs(c[0] - c[1]) < 1e-5f && std::fabs(c[1] - c[2]) < 1e-5f);
}

void testGradeIdentity() {
  const std::string path = tempPath("owc_test_identity.cube");
  CHECK(writeText(path, kIdentityCube2));
  ColorGrade g;
  g.enabled = true;
  g.lut = path;
  g.lut_strength = 1;
  CHECK(!isIdentity(g));
  const struct {
    uint32_t matrix;
    bool full;
  } cases[] = {{1, false}, {2, true}, {2, false}, {1, true}};
  ColorGrader gr;
  std::vector<uint8_t> buf;
  for (const auto& cs : cases) {
    const Img imgs[] = {randomNv12(320, 180, 11), gradientNv12(512, 512)};
    for (const Img& in : imgs) {
      Img out(in.w, in.h);
      const bool ok = gradeFrame(gr, g, cs.matrix, cs.full, in, out, buf);
      CHECK(ok);
      int dy, duv;
      maxDiff(in, out, dy, duv);
      CHECK_MSG(dy <= 1 && duv <= 2, "matrix %u full %d %ux%u: max |dY| %d |dUV| %d", cs.matrix,
                cs.full, in.w, in.h, dy, duv);
    }
  }
  const ColorGrader::Status st = gr.status();
  CHECK(st.active && st.lutSize == 2 && st.lutTitle == "Ident" && st.error.empty());
  CHECK(st.bakeMs > 0 && st.applyMsAvg > 0);
}

void testGradeStrengthZeroAndDisabled() {
  const Img in = randomNv12(256, 144, 5);
  ColorGrade a;
  a.enabled = true;
  a.contrast = 0.4f;
  a.saturation = 0.3f;
  a.temperature = 0.2f;
  a.lut = "builtin:teal_orange";
  a.lut_strength = 0;
  ColorGrade b = a;
  b.lut.clear();
  b.lut_strength = 1;
  Img oa(in.w, in.h), ob(in.w, in.h), oc(in.w, in.h);
  ColorGrader ga, gb, gc;
  std::vector<uint8_t> ba, bb, bc;
  CHECK(gradeFrame(ga, a, 2, false, in, oa, ba));
  CHECK(gradeFrame(gb, b, 2, false, in, ob, bb));
  CHECK(oa.y == ob.y && oa.uv == ob.uv);
  ColorGrade c = a;
  c.lut_strength = 1;
  CHECK(gradeFrame(gc, c, 2, false, in, oc, bc));
  CHECK(oc.y != ob.y);
  CHECK(oa.y != in.y);
  // disabled / neutral / LUT at strength 0: pointers untouched, no copy
  ColorGrade off = c;
  off.enabled = false;
  ColorGrade neutral;
  neutral.enabled = true;
  ColorGrade lut0 = neutral;
  lut0.lut = "builtin:bw";
  lut0.lut_strength = 0;
  for (const ColorGrade* g : {&off, &neutral, &lut0}) {
    CHECK(isIdentity(*g));
    ColorGrader gr;
    std::vector<uint8_t> buf;
    const uint8_t* y = in.y.data();
    const uint8_t* uv = in.uv.data();
    uint32_t ys = in.w, uvs = in.w;
    CHECK(!gr.apply(*g, 2, false, y, ys, uv, uvs, in.w, in.h, buf));
    CHECK(y == in.y.data() && uv == in.uv.data() && ys == in.w && uvs == in.w && buf.empty());
    CHECK(!gr.status().active);
  }
  // switching an active grader off frees the table and passes through
  const uint8_t* y = in.y.data();
  const uint8_t* uv = in.uv.data();
  uint32_t ys = in.w, uvs = in.w;
  CHECK(!gc.apply(off, 2, false, y, ys, uv, uvs, in.w, in.h, bc) && y == in.y.data());
  CHECK(!gc.status().active);
}

void testGradeBwLook() {
  for (const char* look : {"builtin:bw", "builtin:bw_contrast"}) {
    for (int m = 1; m <= 2; ++m) {
      const Img in = rgbBlocksNv12(320, 180, static_cast<uint32_t>(m), m == 2, 9);
      ColorGrade g;
      g.enabled = true;
      g.lut = look;
      ColorGrader gr;
      std::vector<uint8_t> buf;
      Img out(in.w, in.h);
      CHECK(gradeFrame(gr, g, static_cast<uint32_t>(m), m == 2, in, out, buf));
      int worst = 0, ymin = 255, ymax = 0;
      for (uint8_t v : out.uv) worst = std::max(worst, std::abs(v - 128));
      for (uint8_t v : out.y) {
        ymin = std::min<int>(ymin, v);
        ymax = std::max<int>(ymax, v);
      }
      CHECK_MSG(worst <= 2, "%s matrix %d: max |UV-128| %d", look, m, worst);
      CHECK(ymax - ymin > 100);
      CHECK(gr.status().lutSize == 33 && !gr.status().lutTitle.empty());
      // random NV12: mostly out of gamut, the excess chroma is greyed too
      const Img rnd = randomNv12(320, 180, 13);
      CHECK(gradeFrame(gr, g, static_cast<uint32_t>(m), m == 2, rnd, out, buf));
      worst = 0;
      for (uint8_t v : out.uv) worst = std::max(worst, std::abs(v - 128));
      CHECK_MSG(worst <= 2, "%s matrix %d random: max |UV-128| %d", look, m, worst);
    }
  }
}

void testGradeCompareSplit() {
  const Img in = randomNv12(322, 64, 21);
  const uint32_t x0 = (in.w / 2) & ~1u;  // 160
  ColorGrade g;
  g.enabled = true;
  g.contrast = 0.6f;
  g.saturation = 0.5f;
  g.compare_split = true;
  ColorGrader gr;
  std::vector<uint8_t> buf;
  Img out(in.w, in.h);
  CHECK(gradeFrame(gr, g, 2, false, in, out, buf));
  bool leftSame = true, rightDiff = false;
  for (uint32_t r = 0; r < in.h; ++r)
    for (uint32_t x = 0; x < in.w; ++x) {
      if (x < x0) leftSame = leftSame && out.Y(x, r) == in.Y(x, r);
      else rightDiff = rightDiff || out.Y(x, r) != in.Y(x, r);
    }
  for (uint32_t r = 0; r < in.h / 2; ++r)
    for (uint32_t x = 0; x < x0; ++x) leftSame = leftSame && out.uv[r * in.w + x] == in.uv[r * in.w + x];
  CHECK(leftSame && rightDiff);
  // in place (the frame is already our own copy, e.g. the transform output)
  std::vector<uint8_t> own(in.y);
  own.insert(own.end(), in.uv.begin(), in.uv.end());
  const uint8_t* y = own.data();
  const uint8_t* uv = own.data() + in.y.size();
  uint32_t ys = in.w, uvs = in.w;
  CHECK(gr.apply(g, 2, false, y, ys, uv, uvs, in.w, in.h, own));
  CHECK(y == own.data() && uv == own.data() + in.y.size());
  CHECK(memcmp(own.data(), out.y.data(), out.y.size()) == 0 &&
        memcmp(own.data() + in.y.size(), out.uv.data(), out.uv.size()) == 0);
}

void testGradeCoalesce() {
  // a slider dragged continuously: only the latest value matters
  const Img in = randomNv12(128, 72, 4);
  ColorGrader gr;
  std::vector<uint8_t> buf;
  ColorGrade g;
  g.enabled = true;
  for (int i = 1; i <= 300; ++i) {
    g.contrast = static_cast<float>(i) / 400.0f;
    const uint8_t* y = in.y.data();
    const uint8_t* uv = in.uv.data();
    uint32_t ys = in.w, uvs = in.w;
    gr.apply(g, 1, false, y, ys, uv, uvs, in.w, in.h, buf);
  }
  g.contrast = 0.7f;
  Img a(in.w, in.h), b(in.w, in.h);
  CHECK(gradeFrame(gr, g, 1, false, in, a, buf));
  ColorGrader fresh;
  std::vector<uint8_t> buf2;
  CHECK(gradeFrame(fresh, g, 1, false, in, b, buf2));
  CHECK(a.y == b.y && a.uv == b.uv);
}

void testGradeLutFileErrorAndReload() {
  const Img in = gradientNv12(256, 64);
  // missing file: error reported, adjustments still applied
  ColorGrade g;
  g.enabled = true;
  g.contrast = 0.3f;
  g.lut = tempPath("owc_missing_lut_42.cube");
  ColorGrader gr;
  std::vector<uint8_t> buf;
  Img out(in.w, in.h), ref(in.w, in.h);
  CHECK(gradeFrame(gr, g, 2, false, in, out, buf));
  const ColorGrader::Status st = gr.status();
  CHECK_MSG(st.error.find("não foi possível abrir") != std::string::npos, "'%s'", st.error.c_str());
  CHECK(st.lutSize == 0);
  ColorGrade adj = g;
  adj.lut.clear();
  ColorGrader gr2;
  std::vector<uint8_t> buf2;
  CHECK(gradeFrame(gr2, adj, 2, false, in, ref, buf2));
  CHECK(out.y == ref.y && out.uv == ref.uv);
  // reload when the file changes (polled ~1 Hz by the bake thread)
  const std::string path = tempPath("owc_test_reload.cube");
  CHECK(writeText(path, kIdentityCube2));
  ColorGrade lg;
  lg.enabled = true;
  lg.lut = path;
  Img dark(64, 16);  // near black, neutral chroma
  std::fill(dark.y.begin(), dark.y.end(), static_cast<uint8_t>(40));
  std::fill(dark.uv.begin(), dark.uv.end(), static_cast<uint8_t>(128));
  Img darkOut(dark.w, dark.h);
  ColorGrader gl;
  std::vector<uint8_t> bl;
  CHECK(gradeFrame(gl, lg, 2, false, dark, darkOut, bl));
  CHECK(darkOut.y == dark.y && darkOut.uv == dark.uv);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK(writeText(path, "TITLE \"Invert\"\nLUT_3D_SIZE 2\n1 1 1\n0 1 1\n1 0 1\n0 0 1\n"
                        "1 1 0\n0 1 0\n1 0 0\n0 0 0\n"));
  bool reloaded = false;
  const auto t0 = std::chrono::steady_clock::now();
  while (!reloaded && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const uint8_t* y = dark.y.data();
    const uint8_t* uv = dark.uv.data();
    uint32_t ys = dark.w, uvs = dark.w;
    if (gl.apply(lg, 2, false, y, ys, uv, uvs, dark.w, dark.h, bl)) reloaded = y[0] > 200;  // inverted
  }
  CHECK_MSG(reloaded, "title '%s' error '%s'", gl.status().lutTitle.c_str(), gl.status().error.c_str());
  CHECK(gl.status().lutTitle == "Invert");
}

void testExportCube() {
  const std::string path = tempPath("owc_test_export.cube");
  std::string err;
  ColorGrade g;  // neutral (enabled is ignored)
  CHECK(exportCube(g, path, 17, err));
  CubeLut c;
  CHECK_MSG(loadCubeFile(path, c, err), "%s", err.c_str());
  CHECK(c.is3d && c.size == 17 && c.title == "OWC grade" && c.rgb.size() == 17 * 17 * 17 * 3);
  float worst = 0;
  for (int b = 0, i = 0; b < 17; ++b)
    for (int gg = 0; gg < 17; ++gg)
      for (int r = 0; r < 17; ++r, i += 3) {
        worst = std::max(worst, std::fabs(c.rgb[i] - r / 16.0f));
        worst = std::max(worst, std::fabs(c.rgb[i + 1] - gg / 16.0f));
        worst = std::max(worst, std::fabs(c.rgb[i + 2] - b / 16.0f));
      }
  CHECK_MSG(worst < 1e-5f, "identity worst %g", worst);
  // a real grade round-trips to the same values as the live pipeline
  ColorGrade g2;
  g2.contrast = 0.2f;
  g2.vibrance = 0.4f;
  g2.temperature = -0.3f;
  g2.lut = "builtin:film";
  g2.lut_strength = 0.7f;
  CHECK(exportCube(g2, path, 9, err));
  CHECK(loadCubeFile(path, c, err) && c.size == 9);
  GradePipeline p;
  p.prepare(g2, builtinLookLut("film"));
  worst = 0;
  for (int b = 0, i = 0; b < 9; ++b)
    for (int gg = 0; gg < 9; ++gg)
      for (int r = 0; r < 9; ++r, i += 3) {
        float e[3] = {r / 8.0f, gg / 8.0f, b / 8.0f};
        p.run(e);
        for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs(c.rgb[i + k] - e[k]));
      }
  CHECK_MSG(worst < 1e-5f, "graded worst %g", worst);
  CHECK(!exportCube(g, path, 1, err) && !err.empty());
  ColorGrade bad;
  bad.lut = "builtin:nope";
  CHECK(!exportCube(bad, path, 17, err) && !err.empty());
}

void testGradeBenchmark() {
  // 1080p, natural-ish content (ramps + noise); prints, does not assert a time.
  const uint32_t w = 1920, h = 1080;
  Img in(w, h);
  std::mt19937 rng(1);
  for (uint32_t r = 0; r < h; ++r)
    for (uint32_t x = 0; x < w; ++x)
      in.y[r * w + x] = static_cast<uint8_t>(std::min<uint32_t>(235, 16 + (x + r) * 219 / (w + h) + rng() % 8));
  for (uint32_t r = 0; r < h / 2; ++r)
    for (uint32_t x = 0; x < w / 2; ++x) {
      in.uv[r * w + 2 * x] = static_cast<uint8_t>(96 + x * 64 / (w / 2) + rng() % 4);
      in.uv[r * w + 2 * x + 1] = static_cast<uint8_t>(160 - r * 64 / (h / 2) + rng() % 4);
    }
  ColorGrade g;
  g.enabled = true;
  g.contrast = 0.2f;
  g.vibrance = 0.3f;
  g.shadows = 0.2f;
  g.lut = "builtin:teal_orange";
  g.lut_strength = 0.8f;
  ColorGrader gr;
  std::vector<uint8_t> buf;
  Img out(w, h);
  CHECK(gradeFrame(gr, g, 2, false, in, out, buf));
  const double firstBake = gr.status().bakeMs;
  g.contrast = 0.25f;  // steady-state bake (look LUT cached)
  CHECK(gradeFrame(gr, g, 2, false, in, out, buf));
  const double bake = gr.status().bakeMs;
  auto timeIt = [&](bool inPlace) {
    std::vector<uint8_t> own;
    const int frames = 60;
    double total = 0, best = 1e9;
    for (int i = 0; i < frames + 5; ++i) {
      const uint8_t* y = in.y.data();
      const uint8_t* uv = in.uv.data();
      uint32_t ys = w, uvs = w;
      std::vector<uint8_t>* dst = &buf;
      if (inPlace) {
        own = in.y;
        own.insert(own.end(), in.uv.begin(), in.uv.end());
        y = own.data();
        uv = own.data() + in.y.size();
        dst = &own;
      }
      const auto t0 = std::chrono::steady_clock::now();
      gr.apply(g, 2, false, y, ys, uv, uvs, w, h, *dst);
      const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      if (i >= 5) {
        total += ms;
        best = std::min(best, ms);
      }
    }
    std::printf("  1080p grade apply (%s): avg %.2f ms, best %.2f ms\n", inPlace ? "in place" : "copy",
                total / frames, best);
  };
  timeIt(false);
  timeIt(true);
  std::printf("  bake: %.1f ms (first, incl. look generation %.1f ms), %u hw threads\n", bake, firstBake,
              std::thread::hardware_concurrency());
}


// ------------------------------------------------------------------ wheels / curves / scopes
float runOne(const GradePipeline& p, float r, float g, float b, int k) {
  float c[3] = {r, g, b};
  p.run(c);
  return c[k];
}

float degOf(float x, float y) {
  float a = std::atan2(y, x) * 180.0f / 3.14159265f;
  return a < 0 ? a + 360 : a;
}

void testWheels() {
  std::mt19937 rng(17);
  std::uniform_real_distribution<float> d(-1, 1);
  float worstRt = 0, worstSum = 0, worstMag = 0;
  for (int i = 0; i < 2000; ++i) {
    float px = d(rng), py = d(rng);
    if (px * px + py * py > 1) continue;
    float r, g, b, qx, qy;
    wheelToRgb(px, py, r, g, b);
    rgbToWheel(r, g, b, qx, qy);
    worstRt = std::max(worstRt, std::max(std::fabs(qx - px), std::fabs(qy - py)));
    worstSum = std::max(worstSum, std::fabs(r + g + b));
    const float mx = std::max(std::fabs(r), std::max(std::fabs(g), std::fabs(b)));
    worstMag = std::max(worstMag, std::fabs(mx - std::sqrt(px * px + py * py)));
  }
  CHECK_MSG(worstRt < 1e-5f && worstSum < 1e-5f && worstMag < 1e-5f, "rt %g sum %g mag %g", worstRt,
            worstSum, worstMag);
  // hue placement = BT.709 vectorscope directions (R upper left ~103, G ~230, B ~355)
  float px, py;
  rgbToWheel(1, -0.5f, -0.5f, px, py);
  CHECK_MSG(std::fabs(degOf(px, py) - 102.9f) < 0.5f && std::fabs(std::hypot(px, py) - 1) < 1e-5f,
            "red at %g deg", degOf(px, py));
  rgbToWheel(-0.5f, 1, -0.5f, px, py);
  CHECK_MSG(std::fabs(degOf(px, py) - 229.7f) < 0.5f, "green at %g deg", degOf(px, py));
  rgbToWheel(-0.5f, -0.5f, 1, px, py);
  CHECK_MSG(std::fabs(degOf(px, py) - 354.8f) < 0.5f, "blue at %g deg", degOf(px, py));
  for (const VectorTarget& t : vectorscopeTargets(2)) {
    const float tx = t.x100 - 0.5f, ty = 0.5f - t.y100;  // image y down -> up
    const float k = 1.0f / std::hypot(tx, ty);
    float r, g, b;
    wheelToRgb(tx * k, ty * k, r, g, b);
    rgbToWheel(r, g, b, px, py);
    CHECK_MSG(std::fabs(degOf(px, py) - degOf(tx, ty)) < 0.01f, "%s", t.label);
    if (std::string(t.label) == "R") CHECK(r > 0 && g < 0 && b < 0 && std::fabs(r - 1) < 1e-5f);
    if (std::string(t.label) == "Yl") CHECK(b < 0 && std::fabs(b + 1) < 1e-5f);
  }
  // non-zero-sum input is projected (grey removed); zero stays zero
  float ax, ay, bx, by;
  rgbToWheel(1.5f, 0, 0, ax, ay);
  rgbToWheel(1, -0.5f, -0.5f, bx, by);
  CHECK(std::fabs(ax - bx) < 1e-6f && std::fabs(ay - by) < 1e-6f);
  rgbToWheel(0.3f, 0.3f, 0.3f, ax, ay);
  float r0, g0, b0;
  wheelToRgb(0, 0, r0, g0, b0);
  CHECK(ax == 0 && ay == 0 && r0 == 0 && g0 == 0 && b0 == 0);
}

void testCurveEval() {
  // identity: empty, ends only, any points on the diagonal
  const std::vector<CurvePoint> diag = {{0, 0}, {0.3f, 0.3f}, {1, 1}};
  bool exact = true;
  for (int i = 0; i <= 100; ++i) {
    const float x = i / 100.0f;
    exact = exact && evalCurve({}, x) == x && evalCurve({{0, 0}, {1, 1}}, x) == x && evalCurve(diag, x) == x;
  }
  CHECK(exact);
  CHECK(evalCurve({}, -0.5f) == 0 && evalCurve({}, 2) == 1);
  // monotone data -> monotone curve through the points, inside 0..1
  const std::vector<CurvePoint> s = {{0, 0}, {0.3f, 0.6f}, {0.5f, 0.62f}, {0.8f, 0.9f}, {1, 1}};
  float prev = -1;
  bool mono = true, inRange = true;
  for (int i = 0; i <= 2000; ++i) {
    const float y = evalCurve(s, i / 2000.0f);
    mono = mono && y >= prev;
    inRange = inRange && y >= 0 && y <= 1;
    prev = y;
  }
  CHECK(mono && inRange);
  for (const CurvePoint& p : s) CHECK_MSG(std::fabs(evalCurve(s, p.x) - p.y) < 1e-6f, "at %g", p.x);
  // no overshoot on non-monotone data: stays within the points' range
  const std::vector<CurvePoint> bump = {{0, 0.2f}, {0.5f, 1}, {1, 0.2f}};
  float mn = 2, mx = -1;
  for (int i = 0; i <= 1000; ++i) {
    const float y = evalCurve(bump, i / 1000.0f);
    mn = std::min(mn, y);
    mx = std::max(mx, y);
  }
  CHECK_MSG(mn >= 0.2f - 1e-6f && mx <= 1, "range %g..%g", mn, mx);
  // the per-thread "last curve" cache follows the points
  const float s4 = evalCurve(s, 0.4f), b4 = evalCurve(bump, 0.4f);
  CHECK(s4 != b4 && evalCurve(s, 0.4f) == s4 && evalCurve(bump, 0.4f) == b4);
  std::vector<CurvePoint> edit = s;
  edit[1].y = 0.3f;
  CHECK(evalCurve(edit, 0.3f) == 0.3f && std::fabs(evalCurve(s, 0.3f) - 0.6f) < 1e-6f);
  // y outside 0..1 is clamped
  CHECK(evalCurve({{0, -1}, {0.5f, 2}, {1, 1}}, 0.5f) == 1 && evalCurve({{0, -1}, {1, 1}}, 0) == 0);
  // unsorted input + duplicate x (merged into their mean) == sorted input
  const std::vector<CurvePoint> uns = {{1, 1}, {0.5f, 0.7f}, {0, 0}, {0.5f, 0.5f}, {0.25f, 0.3f}};
  const std::vector<CurvePoint> srt = {{0, 0}, {0.25f, 0.3f}, {0.5f, 0.6f}, {1, 1}};
  float worst = 0;
  for (int i = 0; i <= 100; ++i) worst = std::max(worst, std::fabs(evalCurve(uns, i / 100.0f) - evalCurve(srt, i / 100.0f)));
  CHECK_MSG(worst < 1e-6f, "unsorted vs sorted %g", worst);
  // a single point bends the identity through it (implicit (0,0) and (1,1))
  const std::vector<CurvePoint> one = {{0.5f, 0.7f}};
  CHECK(evalCurve(one, 0) == 0 && evalCurve(one, 1) == 1 && std::fabs(evalCurve(one, 0.5f) - 0.7f) < 1e-6f);
  CHECK(evalCurve(one, 0.25f) > 0.25f && evalCurve(one, 0.75f) > 0.75f);
  // hue curve: periodic, continuous (value and slope) across 1 -> 0
  const std::vector<CurvePoint> hue = {{0.2f, 0.8f}, {0.6f, 0.3f}, {0.9f, 0.55f}};
  const float e = 1e-4f;
  CHECK(std::fabs(evalHueCurve(hue, 0) - evalHueCurve(hue, 1)) < 1e-6f);
  CHECK(std::fabs(evalHueCurve(hue, e) - evalHueCurve(hue, 1 - e)) < 1e-3f);
  const float slopeL = (evalHueCurve(hue, 1) - evalHueCurve(hue, 1 - e)) / e;
  const float slopeR = (evalHueCurve(hue, e) - evalHueCurve(hue, 0)) / e;
  CHECK_MSG(std::fabs(slopeL - slopeR) < 0.02f, "slopes %g %g", slopeL, slopeR);
  CHECK(std::fabs(evalHueCurve(hue, 1.2f) - evalHueCurve(hue, 0.2f)) < 1e-6f &&
        std::fabs(evalHueCurve(hue, -0.4f) - 0.3f) < 1e-5f);
  for (const CurvePoint& p : hue) CHECK(std::fabs(evalHueCurve(hue, p.x) - p.y) < 1e-6f);
  CHECK(evalHueCurve({}, 0.3f) == 0.5f && evalHueCurve({{0.4f, 0.7f}}, 0.9f) == 0.7f);
  // a point at x = 1 is the point at x = 0
  CHECK(std::fabs(evalHueCurve({{1, 0.9f}, {0.5f, 0.5f}}, 0) - 0.9f) < 1e-6f);
  // luma vs sat: flat ends, 0.5 when empty
  const std::vector<CurvePoint> ls = {{0.2f, 0.3f}, {0.8f, 0.6f}};
  CHECK(evalLumaSatCurve({}, 0.1f) == 0.5f && evalLumaSatCurve(ls, 0) == 0.3f && evalLumaSatCurve(ls, 1) == 0.6f);
  CHECK(evalLumaSatCurve(ls, 0.5f) > 0.3f && evalLumaSatCurve(ls, 0.5f) < 0.6f);
  // saturation gain of a curve y
  CHECK(satGainOfCurveY(0) == 0 && satGainOfCurveY(0.5f) == 1 && std::fabs(satGainOfCurveY(1) - 4) < 1e-5f);
  CHECK(std::fabs(satGainOfCurveY(0.75f) - 2) < 1e-5f);
}

void testGradeWheels() {
  ColorGrade n;  // every wheel present but neutral, pivot moved (contrast 0)
  n.pivot = 0.3f;
  GradePipeline p;
  p.prepare(n, nullptr);
  CHECK(p.noop());
  auto pipe = [](const ColorGrade& g) {
    GradePipeline q;
    q.prepare(g, nullptr);
    return q;
  };
  const float tol = 2e-4f;
  // offset: x += 0.25 (m + c)
  ColorGrade g;
  g.offset.y = 0.2f;
  g.offset.r = 0.4f;
  g.offset.g = -0.2f;
  g.offset.b = -0.2f;
  p = pipe(g);
  CHECK(std::fabs(runOne(p, 0.3f, 0.3f, 0.3f, 0) - (0.3f + 0.25f * 0.6f)) < tol);
  CHECK(std::fabs(runOne(p, 0.3f, 0.3f, 0.3f, 1) - 0.3f) < tol);
  CHECK(std::fabs(runOne(p, 0.6f, 0.2f, 0.4f, 2) - 0.4f) < tol);
  // lift: x += 0.25 (m + c)(1 - x): black moves, white fixed
  g = ColorGrade();
  g.lift.y = 0.4f;
  p = pipe(g);
  for (float x : {0.0f, 0.25f, 0.5f, 1.0f})
    CHECK_MSG(std::fabs(runOne(p, x, x, x, 1) - (x + 0.1f * (1 - x))) < tol, "lift at %g", x);
  g.lift.y = 0;
  g.lift.b = 0.8f;
  g.lift.r = g.lift.g = -0.4f;
  p = pipe(g);
  CHECK(std::fabs(runOne(p, 0, 0, 0, 2) - 0.2f) < tol && runOne(p, 0, 0, 0, 0) == 0);  // red clamps at 0
  CHECK(std::fabs(runOne(p, 1, 1, 1, 0) - 1) < tol && std::fabs(runOne(p, 1, 1, 1, 2) - 1) < tol);
  // gamma: x^(2^-(m + c)): black and white fixed
  g = ColorGrade();
  g.gamma.y = 0.5f;
  p = pipe(g);
  for (float x : {0.0f, 0.25f, 0.5f, 1.0f})
    CHECK_MSG(std::fabs(runOne(p, x, x, x, 0) - std::pow(x, std::exp2(-0.5f))) < tol, "gamma at %g", x);
  g.gamma.y = 0;
  g.gamma.g = -1;
  g.gamma.r = g.gamma.b = 0.5f;
  p = pipe(g);
  CHECK(std::fabs(runOne(p, 0.5f, 0.5f, 0.5f, 1) - 0.25f) < tol &&
        std::fabs(runOne(p, 0.5f, 0.5f, 0.5f, 0) - std::pow(0.5f, std::exp2(-0.5f))) < tol);
  // gain: x * 2^(m + c): black fixed
  g = ColorGrade();
  g.gain.y = -0.5f;
  p = pipe(g);
  for (float x : {0.0f, 0.4f, 0.8f, 1.0f})
    CHECK_MSG(std::fabs(runOne(p, x, x, x, 2) - x * std::exp2(-0.5f)) < tol, "gain at %g", x);
  g.gain.y = 0.5f;  // white goes over 1: clamped at the end
  p = pipe(g);
  CHECK(runOne(p, 1, 1, 1, 0) == 1 && std::fabs(runOne(p, 0.5f, 0.5f, 0.5f, 0) - 0.7071f) < tol);
  // contrast pivots around `pivot` (black, white and the pivot stay put)
  for (float pv : {0.3f, 0.435f, 0.5f, 0.7f}) {
    g = ColorGrade();
    g.contrast = 0.6f;
    g.pivot = pv;
    p = pipe(g);
    CHECK_MSG(std::fabs(runOne(p, pv, pv, pv, 1) - pv) < tol, "pivot %g", pv);
    CHECK(runOne(p, 0, 0, 0, 1) == 0 && std::fabs(runOne(p, 1, 1, 1, 1) - 1) < tol);
    CHECK(runOne(p, pv + 0.1f, pv + 0.1f, pv + 0.1f, 1) > pv + 0.1f &&
          runOne(p, pv - 0.1f, pv - 0.1f, pv - 0.1f, 1) < pv - 0.1f);
  }
  // all four wheels + contrast keep a grey ramp monotonic
  g = ColorGrade();
  g.offset.y = -0.1f;
  g.lift.y = 0.3f;
  g.gamma.y = -0.4f;
  g.gain.y = 0.3f;
  g.contrast = 0.5f;
  p = pipe(g);
  float prev = -1;
  bool mono = true;
  for (int i = 0; i <= 1000; ++i) {
    const float v = runOne(p, i / 1000.0f, i / 1000.0f, i / 1000.0f, 1);
    mono = mono && v >= prev - 1e-6f;
    prev = v;
  }
  CHECK(mono);
}

void testGradeCurvesStage() {
  // Y curve acts on all channels, then the channel curve
  ColorGrade g;
  g.curve_y = {{0, 0}, {0.5f, 0.6f}, {1, 1}};
  g.curve_b = {{0, 0.1f}, {1, 1}};
  GradePipeline p;
  p.prepare(g, nullptr);
  const float y5 = evalCurve(g.curve_y, 0.5f);
  CHECK(std::fabs(runOne(p, 0.5f, 0.5f, 0.5f, 0) - y5) < 1e-4f);
  CHECK(std::fabs(runOne(p, 0.5f, 0.5f, 0.5f, 2) - evalCurve(g.curve_b, y5)) < 1e-4f);
  // hue vs sat: y = 0 on red only -> red goes grey (luma kept), others untouched
  g = ColorGrade();
  g.hue_vs_sat = {{0, 0}, {1 / 6.0f, 0.5f}, {2 / 6.0f, 0.5f}, {3 / 6.0f, 0.5f}, {4 / 6.0f, 0.5f}, {5 / 6.0f, 0.5f}};
  p.prepare(g, nullptr);
  float c[3] = {1, 0, 0};
  p.run(c);
  CHECK_MSG(std::fabs(c[0] - 0.2126f) < 2e-3f && std::fabs(c[1] - 0.2126f) < 2e-3f && std::fabs(c[2] - 0.2126f) < 2e-3f,
            "red -> %g %g %g", c[0], c[1], c[2]);
  const float others[][3] = {{0, 1, 0}, {0, 0, 1}, {0, 1, 1}, {1, 1, 0}, {1, 0, 1}, {0.2f, 0.6f, 0.3f}};
  for (const auto& o : others) {
    float v[3] = {o[0], o[1], o[2]};
    p.run(v);
    CHECK_MSG(std::fabs(v[0] - o[0]) < 2e-3f && std::fabs(v[1] - o[1]) < 2e-3f && std::fabs(v[2] - o[2]) < 2e-3f,
              "(%g %g %g) -> (%g %g %g)", o[0], o[1], o[2], v[0], v[1], v[2]);
  }
  // hue vs hue: a constant +1/6 turn takes a muted red to the yellow hue at the same luma
  g = ColorGrade();
  g.hue_vs_hue = {{0.3f, 0.5f + 1 / 6.0f}};
  p.prepare(g, nullptr);
  float in[3] = {0.6f, 0.4f, 0.4f}, out[3] = {0.6f, 0.4f, 0.4f};
  p.run(out);
  auto hueOf = [](const float* v) {
    const float L = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
    return hueFromChromaAngle(std::atan2((v[0] - L) / 1.5748f, (v[2] - L) / 1.8556f));
  };
  auto lumaOf = [](const float* v) { return 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2]; };
  float dh = hueOf(out) - hueOf(in);
  dh -= std::floor(dh + 0.5f);
  CHECK_MSG(std::fabs(dh - 1 / 6.0f) < 2e-3f, "hue shift %g", dh);
  CHECK(std::fabs(lumaOf(out) - lumaOf(in)) < 1e-4f);
  // near grey the hue curves fade out: grey stays grey, tiny chroma barely moves
  float grey[3] = {0.4f, 0.4f, 0.4f};
  p.run(grey);
  CHECK(std::fabs(grey[0] - 0.4f) < 1e-5f && std::fabs(grey[2] - 0.4f) < 1e-5f);
  // luma vs sat at y = 0 everywhere: all grey
  g = ColorGrade();
  g.luma_vs_sat = {{0.5f, 0}};
  p.prepare(g, nullptr);
  float v[3] = {0.9f, 0.3f, 0.1f};
  p.run(v);
  CHECK(std::fabs(v[0] - v[1]) < 1e-5f && std::fabs(v[1] - v[2]) < 1e-5f);
  // luma vs sat keyed on luma: dark colours desaturated, bright ones kept
  g.luma_vs_sat = {{0, 0}, {0.3f, 0.5f}, {1, 0.5f}};
  p.prepare(g, nullptr);
  float dark[3] = {0.05f, 0.0f, 0.0f}, bright[3] = {1, 0.8f, 0.7f};
  p.run(dark);
  p.run(bright);
  CHECK(std::fabs(dark[0] - dark[1]) < 4e-3f && std::fabs(bright[0] - 1) < 1e-3f && std::fabs(bright[2] - 0.7f) < 1e-3f);
}

ColorGrade neutralButPresent() {
  ColorGrade g;
  g.enabled = true;
  g.pivot = 0.25f;
  g.lift = ColorWheel();
  g.curve_y = {{0, 0}, {1, 1}};
  g.curve_r = {{0.5f, 0.5f}};
  g.curve_g = {{0, 0}, {0.25f, 0.25f}, {1, 1}};
  g.curve_b = {};
  g.hue_vs_hue = {{0, 0.5f}, {0.5f, 0.5f}};
  g.hue_vs_sat = {{0.3f, 0.5f}};
  g.luma_vs_sat = {{0, 0.5f}, {1, 0.5f}};
  return g;
}

void testGradeNeutralNewFields() {
  const ColorGrade n = neutralButPresent();
  CHECK(isIdentity(n));
  GradePipeline p;
  p.prepare(n, nullptr);
  CHECK(p.noop());
  std::mt19937 rng(5);
  std::uniform_real_distribution<float> d(-1, 2);
  bool exact = true;
  for (int i = 0; i < 1000; ++i) {
    float c[3] = {d(rng), d(rng), d(rng)};
    const float o[3] = {c[0], c[1], c[2]};
    p.run(c);
    exact = exact && c[0] == o[0] && c[1] == o[1] && c[2] == o[2];
  }
  CHECK(exact);
  // with an identity LUT the table path stays exact (as in testGradeIdentity)
  const std::string path = tempPath("owc_test_identity_new.cube");
  CHECK(writeText(path, kIdentityCube2));
  ColorGrade gl = n;
  gl.lut = path;
  ColorGrader gr;
  std::vector<uint8_t> buf;
  const Img in = gradientNv12(512, 512);
  Img out(in.w, in.h);
  CHECK(gradeFrame(gr, gl, 2, false, in, out, buf));
  int dy, duv;
  maxDiff(in, out, dy, duv);
  CHECK_MSG(dy <= 1 && duv <= 2, "max |dY| %d |dUV| %d", dy, duv);
  // every single new control, when moved, makes the grade non-identity
  std::vector<ColorGrade> moved(12, n);
  moved[0].lift.r = 0.1f;
  moved[1].gamma.y = 0.1f;
  moved[2].gain.b = -0.1f;
  moved[3].offset.g = 0.1f;
  moved[4].curve_y = {{0.5f, 0.6f}};
  moved[5].curve_r = {{0, 0.1f}, {1, 1}};
  moved[6].curve_g = {{0.5f, 0.4f}};
  moved[7].curve_b = {{1, 0.9f}};
  moved[8].hue_vs_hue = {{0.2f, 0.6f}};
  moved[9].hue_vs_sat = {{0.2f, 0.4f}};
  moved[10].luma_vs_sat = {{0.2f, 0.4f}, {1, 0.5f}};
  moved[11].contrast = 0.1f;
  for (size_t i = 0; i < moved.size(); ++i) {
    CHECK_MSG(!isIdentity(moved[i]), "case %zu", i);
    p.prepare(moved[i], nullptr);
    CHECK_MSG(!p.noop(), "case %zu", i);
  }
}

void testExportCubeWheels() {
  const std::string path = tempPath("owc_test_export_wheels.cube");
  std::string err;
  ColorGrade g;
  g.gain.y = 0.5f;
  CHECK(exportCube(g, path, 17, err));
  CubeLut c;
  CHECK(loadCubeFile(path, c, err) && c.size == 17);
  // node (8, 8, 8) = grey 0.5 -> 0.5 * 2^0.5
  const size_t i = (static_cast<size_t>(8) * 17 * 17 + 8 * 17 + 8) * 3;
  CHECK_MSG(std::fabs(c.rgb[i] - 0.70711f) < 1e-4f && std::fabs(c.rgb[i + 2] - 0.70711f) < 1e-4f, "%g", c.rgb[i]);
  // everything at once round-trips to the live pipeline
  ColorGrade a;
  a.lift = {0.1f, -0.05f, -0.05f, 0.05f};
  a.gamma = {-0.1f, 0.05f, 0.05f, 0.1f};
  a.gain = {0.05f, 0, -0.05f, -0.1f};
  a.offset = {0, 0.02f, -0.02f, 0};
  a.contrast = 0.3f;
  a.pivot = 0.4f;
  a.curve_y = {{0, 0}, {0.4f, 0.45f}, {1, 1}};
  a.curve_r = {{0, 0}, {0.5f, 0.55f}, {1, 1}};
  a.hue_vs_hue = {{0.0f, 0.55f}, {0.5f, 0.45f}};
  a.hue_vs_sat = {{0.3f, 0.7f}, {0.7f, 0.4f}};
  a.luma_vs_sat = {{0, 0.3f}, {0.5f, 0.5f}};
  a.lut = "builtin:teal_orange";
  a.lut_strength = 0.5f;
  CHECK(exportCube(a, path, 9, err));
  CHECK(loadCubeFile(path, c, err) && c.size == 9);
  GradePipeline p;
  p.prepare(a, builtinLookLut("teal_orange"));
  float worst = 0;
  for (int b = 0, k = 0; b < 9; ++b)
    for (int gg = 0; gg < 9; ++gg)
      for (int r = 0; r < 9; ++r, k += 3) {
        float e[3] = {r / 8.0f, gg / 8.0f, b / 8.0f};
        p.run(e);
        for (int j = 0; j < 3; ++j) worst = std::max(worst, std::fabs(c.rgb[k + j] - std::min(1.0f, std::max(0.0f, e[j]))));
      }
  CHECK_MSG(worst < 1e-5f, "graded worst %g", worst);
}

// Full-range RGB of an NV12 pixel (block chroma), as renderGradedRgba converts.
void nv12PixelRgb(const Img& im, uint32_t x, uint32_t r, uint32_t matrix, bool full, float* c) {
  const float kr = matrix == 1 ? 0.299f : 0.2126f, kb = matrix == 1 ? 0.114f : 0.0722f;
  const float yn = (im.Y(x, r) - (full ? 0.0f : 16.0f)) / (full ? 255.0f : 219.0f);
  const uint8_t* uv = &im.uv[(r / 2) * im.w + (x & ~1u)];
  const float cb = (uv[0] - 128.0f) / (full ? 255.0f : 224.0f), cr = (uv[1] - 128.0f) / (full ? 255.0f : 224.0f);
  c[0] = yn + 2 * (1 - kr) * cr;
  c[2] = yn + 2 * (1 - kb) * cb;
  c[1] = (yn - kr * c[0] - kb * c[2]) / (1 - kr - kb);
}

void testRenderGradedRgba() {
  // downscale: constant stays constant, 2x2 boxes average exactly
  Img flat(64, 32);
  std::fill(flat.y.begin(), flat.y.end(), static_cast<uint8_t>(77));
  std::fill(flat.uv.begin(), flat.uv.end(), static_cast<uint8_t>(140));
  std::vector<uint8_t> small;
  downscaleNV12(flat.y.data(), flat.w, flat.uv.data(), flat.w, flat.w, flat.h, 16, 8, small);
  CHECK(small.size() == 16 * 8 * 3 / 2 && std::all_of(small.begin(), small.begin() + 128, [](uint8_t v) { return v == 77; }) &&
        std::all_of(small.begin() + 128, small.end(), [](uint8_t v) { return v == 140; }));
  Img q(4, 4);
  for (int i = 0; i < 16; ++i) q.y[i] = static_cast<uint8_t>(i * 10);
  downscaleNV12(q.y.data(), 4, q.uv.data(), 4, 4, 4, 2, 2, small);
  CHECK(small.size() == 6 && small[0] == 25 && small[1] == 45 && small[2] == 105 && small[3] == 125);
  downscaleNV12(q.y.data(), 4, q.uv.data(), 4, 4, 4, 3, 3, small);  // odd -> even (2x2)
  CHECK(small.size() == 6);

  // matches the table path (in-gamut image, moderate grade)
  const Img in = rgbBlocksNv12(320, 180, 2, false, 31);
  ColorGrade g;
  g.enabled = true;
  g.contrast = 0.3f;
  g.lift = {0.05f, -0.025f, -0.025f, 0.05f};
  g.gain = {0, 0, 0, 0.1f};
  g.curve_y = {{0, 0}, {0.5f, 0.55f}, {1, 1}};
  g.hue_vs_sat = {{0, 0.65f}, {0.5f, 0.45f}};
  g.saturation = 0.1f;
  g.lut = "builtin:teal_orange";
  g.lut_strength = 0.6f;
  ColorGrader gr;
  std::vector<uint8_t> buf;
  Img out(in.w, in.h);
  CHECK(gradeFrame(gr, g, 2, false, in, out, buf));
  std::vector<uint8_t> rgba;
  std::string err;
  CHECK(renderGradedRgba(g, nullptr, 0, 0, 2, false, rgba, &err) == false && !err.empty());
  std::vector<uint8_t> packed(in.y);
  packed.insert(packed.end(), in.uv.begin(), in.uv.end());
  CHECK(renderGradedRgba(g, packed.data(), in.w, in.h, 2, false, rgba, &err) && err.empty());
  CHECK(rgba.size() == static_cast<size_t>(in.w) * in.h * 4);
  int worst = 0, over4 = 0;
  double sum = 0;
  bool opaque = true;
  for (uint32_t r = 0; r < in.h; ++r)
    for (uint32_t x = 0; x < in.w; ++x) {
      float c[3];
      nv12PixelRgb(out, x, r, 2, false, c);
      const uint8_t* p = &rgba[(static_cast<size_t>(r) * in.w + x) * 4];
      for (int k = 0; k < 3; ++k) {
        const int ref = static_cast<int>(std::lround(std::min(1.0f, std::max(0.0f, c[k])) * 255));
        worst = std::max(worst, std::abs(ref - p[k]));
        sum += std::abs(ref - p[k]);
        over4 += std::abs(ref - p[k]) > 4;
      }
      opaque = opaque && p[3] == 255;
    }
  const double mean = sum / (in.w * in.h * 3.0);
  const double pctOver4 = 100.0 * over4 / (in.w * in.h * 3.0);
  std::printf("  thumbnail vs table path: max %d, mean %.2f codes, %.2f%% > 4 codes\n", worst, mean, pctOver4);
  // The table interpolates chroma bilinearly on an 8-code grid: saturated
  // colours pushed against the gamut edge differ by a few codes (worst ~11).
  CHECK_MSG(worst <= 16 && mean < 1.0 && pctOver4 < 1.0, "max %d mean %.2f >4: %.2f%%", worst, mean, pctOver4);
  CHECK(opaque);
  // enabled / compare_split are ignored
  ColorGrade off = g;
  off.enabled = false;
  off.compare_split = true;
  std::vector<uint8_t> rgba2;
  CHECK(renderGradedRgba(off, packed.data(), in.w, in.h, 2, false, rgba2) && rgba2 == rgba);

  // .cube through the cache: concurrent renders agree; a missing file reports
  const std::string path = tempPath("owc_test_thumb.cube");
  ColorGrade look;
  look.contrast = 0.4f;
  look.temperature = 0.5f;
  CHECK(exportCube(look, path, 17, err));
  ColorGrade fileGrade;
  fileGrade.lut = path;
  std::vector<std::vector<uint8_t>> res(4);
  std::vector<int> oks(4, 0);
  std::vector<std::thread> th;
  for (int i = 0; i < 4; ++i)
    th.emplace_back([&, i] { oks[i] = renderGradedRgba(fileGrade, packed.data(), in.w, in.h, 2, false, res[i]); });
  for (auto& t : th) t.join();
  CHECK(oks[0] && oks[1] && oks[2] && oks[3] && res[0] == res[1] && res[0] == res[2] && res[0] == res[3]);
  CHECK(res[0] != rgba);
  // the cached LUT-less base follows the grade
  std::vector<uint8_t> again;
  CHECK(renderGradedRgba(g, packed.data(), in.w, in.h, 2, false, again) && again == rgba);
  ColorGrade missing = g;
  missing.lut = tempPath("owc_missing_thumb_42.cube");
  CHECK(!renderGradedRgba(missing, packed.data(), in.w, in.h, 2, false, rgba2, &err) && !err.empty());
  ColorGrade noLut = g;
  noLut.lut.clear();
  std::vector<uint8_t> rgba3;
  CHECK(renderGradedRgba(noLut, packed.data(), in.w, in.h, 2, false, rgba3) && rgba3 == rgba2);
}

// Alpha of pixel (x, y) of an RGBA image of width w.
inline uint8_t alphaAt(const std::vector<uint8_t>& img, uint32_t w, uint32_t x, uint32_t y) {
  return img[(static_cast<size_t>(y) * w + x) * 4 + 3];
}

void testScopes() {
  ScopeOptions o;  // 512x256, vector 256, step 2
  ScopeImages s;
  // black frame (limited range): waveform energy on the bottom row only
  Img black(320, 180);
  std::fill(black.y.begin(), black.y.end(), static_cast<uint8_t>(16));
  std::fill(black.uv.begin(), black.uv.end(), static_cast<uint8_t>(128));
  computeScopes(black.y.data(), black.w, black.uv.data(), black.w, black.w, black.h, o, s);
  CHECK(s.w == 512 && s.h == 256 && s.vs == 256 && s.waveform.size() == 512 * 256 * 4 &&
        s.vectorscope.size() == 256 * 256 * 4);
  // (code 16 is the centre of the black row: box resampling lets at most
  // the row above catch the sliver of the code's interval beyond the edge)
  bool onlyBottom = true;
  int bottomLit = 0;
  for (uint32_t y = 0; y < s.h; ++y)
    for (uint32_t x = 0; x < s.w; ++x) {
      const uint8_t a = alphaAt(s.waveform, s.w, x, y);
      if (y == s.h - 1) bottomLit += a > 0;
      else onlyBottom = onlyBottom && (a == 0 || y == s.h - 2);
    }
  CHECK_MSG(onlyBottom && bottomLit > 500, "bottom lit %d", bottomLit);
  // vectorscope: only the centre
  bool centreOnly = true;
  for (uint32_t y = 0; y < s.vs; ++y)
    for (uint32_t x = 0; x < s.vs; ++x)
      if (alphaAt(s.vectorscope, s.vs, x, y))
        centreOnly = centreOnly && std::abs(static_cast<int>(x) - 128) <= 1 && std::abs(static_cast<int>(y) - 128) <= 1;
  CHECK(centreOnly);

  // pure red, full range BT.709: parade R at the top, G and B at the bottom
  Img red(320, 180);
  std::fill(red.y.begin(), red.y.end(), static_cast<uint8_t>(54));
  for (size_t i = 0; i < red.uv.size(); i += 2) {
    red.uv[i] = 99;
    red.uv[i + 1] = 255;
  }
  ScopeOptions of = o;
  of.full_range = true;
  of.matrix = 2;
  computeScopes(red.y.data(), red.w, red.uv.data(), red.w, red.w, red.h, of, s);
  const uint32_t P = s.w / 3;
  int minRow[3] = {9999, 9999, 9999}, maxRow[3] = {-1, -1, -1};
  for (uint32_t y = 0; y < s.h; ++y)
    for (uint32_t x = 0; x < 3 * P; ++x)
      if (alphaAt(s.parade, s.w, x, y)) {
        const uint32_t k = x / P;
        minRow[k] = std::min(minRow[k], static_cast<int>(y));
        maxRow[k] = std::max(maxRow[k], static_cast<int>(y));
      }
  CHECK_MSG(maxRow[0] >= 0 && maxRow[0] <= 2, "R rows %d..%d", minRow[0], maxRow[0]);
  CHECK_MSG(minRow[1] >= static_cast<int>(s.h) - 3 && minRow[2] >= static_cast<int>(s.h) - 3, "G from %d, B from %d",
            minRow[1], minRow[2]);
  // red's parade traces are reddish
  const uint8_t* rp = &s.parade[(static_cast<size_t>(maxRow[0]) * s.w + P / 2) * 4];
  CHECK(rp[3] > 0 && rp[0] >= rp[1] && rp[0] >= rp[2]);

  // 75% colour bars (limited 709): vectorscope energy at the 75% targets only
  const float bars[6][3] = {{0.75f, 0.75f, 0}, {0, 0.75f, 0.75f}, {0, 0.75f, 0}, {0.75f, 0, 0.75f}, {0.75f, 0, 0}, {0, 0, 0.75f}};
  Img cb(384, 64);
  for (uint32_t x = 0; x < cb.w; ++x) {
    const float* c = bars[x * 6 / cb.w];
    const float Y = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    const float u = (c[2] - Y) / 1.8556f, v = (c[0] - Y) / 1.5748f;
    for (uint32_t r = 0; r < cb.h; ++r) cb.y[r * cb.w + x] = static_cast<uint8_t>(std::lround(16 + 219 * Y));
    for (uint32_t r = 0; r < cb.h / 2; ++r) {
      cb.uv[r * cb.w + (x & ~1u)] = static_cast<uint8_t>(std::lround(128 + 224 * u));
      cb.uv[r * cb.w + (x & ~1u) + 1] = static_cast<uint8_t>(std::lround(128 + 224 * v));
    }
  }
  ScopeOptions ob = o;
  ob.matrix = 2;
  computeScopes(cb.y.data(), cb.w, cb.uv.data(), cb.w, cb.w, cb.h, ob, s);
  const std::vector<VectorTarget> tg = vectorscopeTargets(2);
  CHECK(tg.size() == 6);
  std::vector<int> hits(tg.size(), 0);
  bool allNear = true;
  for (uint32_t y = 0; y < s.vs; ++y)
    for (uint32_t x = 0; x < s.vs; ++x) {
      if (!alphaAt(s.vectorscope, s.vs, x, y)) continue;
      bool isNear = false;
      for (size_t k = 0; k < tg.size(); ++k) {
        const float dx = (x + 0.5f) - tg[k].x75 * s.vs, dy = (y + 0.5f) - tg[k].y75 * s.vs;
        if (dx * dx + dy * dy <= 3.0f * 3.0f) {
          isNear = true;
          ++hits[k];
        }
      }
      allNear = allNear && isNear;
    }
  CHECK(allNear);
  for (size_t k = 0; k < tg.size(); ++k) CHECK_MSG(hits[k] > 0, "no energy at %s", tg[k].label);
  // the 100% targets are 4/3 of the 75% ones from the centre, the skin line in the R..Yl sector
  CHECK(std::fabs((tg[0].x100 - 0.5f) * 0.75f - (tg[0].x75 - 0.5f)) < 1e-6f);
  const float skin = vectorscopeSkinLineAngle() * 180 / 3.14159265f;
  CHECK(skin > 115 && skin < 130);

  // histogram: two equal halves (full-range grey 64 / 192) -> two equal bars
  Img two(256, 128);
  for (uint32_t r = 0; r < two.h; ++r)
    for (uint32_t x = 0; x < two.w; ++x) two.y[r * two.w + x] = x < two.w / 2 ? 64 : 192;
  std::fill(two.uv.begin(), two.uv.end(), static_cast<uint8_t>(128));
  computeScopes(two.y.data(), two.w, two.uv.data(), two.w, two.w, two.h, of, s);
  std::vector<int> colLit(s.w, 0);
  for (uint32_t y = 0; y < s.h; ++y)
    for (uint32_t x = 0; x < s.w; ++x) colLit[x] += alphaAt(s.histogram, s.w, x, y) > 0;
  const uint32_t c64 = 64 * s.w / 256, c192 = 192 * s.w / 256;
  CHECK_MSG(colLit[c64] == static_cast<int>(s.h) && colLit[c192] == static_cast<int>(s.h), "bars %d %d", colLit[c64],
            colLit[c192]);
  bool elsewhereEmpty = true;
  for (uint32_t x = 0; x < s.w; ++x)
    if (x + 1 < c64 || (x > c64 + 2 && x + 1 < c192) || x > c192 + 2) elsewhereEmpty = elsewhereEmpty && colLit[x] == 0;
  CHECK(elsewhereEmpty);
  // the bar is grey (R+G+B additive) under the white luma outline at its edge
  const uint8_t* hp = &s.histogram[(static_cast<size_t>(s.h / 2) * s.w + c64 + 1) * 4];
  CHECK(hp[3] > 0 && std::abs(hp[0] - hp[1]) < 40 && std::abs(hp[1] - hp[2]) < 40);
  // transparent background
  CHECK(s.histogram[3] == 0 && s.waveform[3] == 0);

  // (a) luma gradient across the code range, tall waveform: no empty rows
  {
    Img gr(1024, 64);
    std::fill(gr.uv.begin(), gr.uv.end(), static_cast<uint8_t>(128));
    for (int pass = 0; pass < 4; ++pass) {
      const bool full = pass & 1, wide = pass & 2;  // wide: 0..255 codes in limited range too
      const int lo = full || wide ? 0 : 16, hi = full || wide ? 255 : 235;
      for (uint32_t r = 0; r < gr.h; ++r)
        for (uint32_t x = 0; x < gr.w; ++x)
          gr.y[r * gr.w + x] = static_cast<uint8_t>(lo + (hi - lo) * x / (gr.w - 1));
      for (uint32_t H : {128u, 256u, 512u, 700u}) {
        ScopeOptions og;
        og.width = 512;
        og.height = H;
        og.step = 1;
        og.full_range = full;
        computeScopes(gr.y.data(), gr.w, gr.uv.data(), gr.w, gr.w, gr.h, og, s);
        // value range of the gradient -> rows
        const double vlo = full ? lo / 255.0 : std::max(0.0, (lo - 16) / 219.0);
        const double vhi = full ? hi / 255.0 : std::min(1.0, (hi - 16) / 219.0);
        const int rTop = static_cast<int>(std::ceil((H - 1) * (1 - vhi))), rBot = static_cast<int>((H - 1) * (1 - vlo));
        int empty = 0;
        for (int y = rTop; y <= rBot; ++y) {
          bool any = false;
          for (uint32_t x = 0; x < s.w && !any; ++x) any = alphaAt(s.waveform, s.w, x, static_cast<uint32_t>(y)) > 0;
          empty += !any;
        }
        CHECK_MSG(empty == 0, "waveform gradient pass %d H=%u: %d empty rows in %d..%d", pass, H, empty, rTop, rBot);
        // and every column of the gradient is lit
        int darkCols = 0;
        for (uint32_t x = 0; x < s.w; ++x) {
          bool any = false;
          for (uint32_t y = 0; y < s.h && !any; ++y) any = alphaAt(s.waveform, s.w, x, y) > 0;
          darkCols += !any;
        }
        CHECK_MSG(darkCols == 0, "pass %d H=%u: %d dark columns", pass, H, darkCols);
      }
    }
  }
  // (b) flat mid-grey: energy in 1..3 rows around the expected one, any height
  {
    Img gy(256, 64);
    std::fill(gy.y.begin(), gy.y.end(), static_cast<uint8_t>(128));
    std::fill(gy.uv.begin(), gy.uv.end(), static_cast<uint8_t>(128));
    for (uint32_t H : {128u, 256u, 512u}) {
      ScopeOptions og;
      og.height = H;
      og.full_range = true;
      computeScopes(gy.y.data(), gy.w, gy.uv.data(), gy.w, gy.w, gy.h, og, s);
      const uint32_t x = s.w / 2;
      int lit = 0, brightest = 0;
      uint32_t best = 0;
      for (uint32_t y = 0; y < s.h; ++y) {
        const uint8_t a = alphaAt(s.waveform, s.w, x, y);
        lit += a > 0;
        if (a > brightest) brightest = a, best = y;
      }
      const double expect = (H - 1) * (1 - 128 / 255.0);
      CHECK_MSG(lit >= 1 && lit <= 3 && std::fabs(best - expect) <= 1.0, "flat grey H=%u: %d rows, best %u expect %.1f", H,
                lit, best, expect);
    }
  }
  // (c) vectorscope of a smooth chroma disc: continuous, no lattice
  {
    Img dc(1024, 1024);
    std::fill(dc.y.begin(), dc.y.end(), static_cast<uint8_t>(120));
    std::fill(dc.uv.begin(), dc.uv.end(), static_cast<uint8_t>(128));
    const double R = 100;  // chroma codes
    for (uint32_t r2 = 0; r2 < dc.h / 2; ++r2)
      for (uint32_t x2 = 0; x2 < dc.w / 2; ++x2) {
        const double dx = (x2 + 0.5) / (dc.w / 2.0) * 2 - 1, dy = (r2 + 0.5) / (dc.h / 2.0) * 2 - 1;
        if (dx * dx + dy * dy > 1) continue;
        dc.uv[r2 * dc.w + x2 * 2] = static_cast<uint8_t>(std::lround(128 + R * dx));
        dc.uv[r2 * dc.w + x2 * 2 + 1] = static_cast<uint8_t>(std::lround(128 + R * dy));
      }
    for (uint32_t VS : {128u, 256u, 512u}) {
      ScopeOptions ov;
      ov.vector_size = VS;
      ov.step = 1;
      ov.matrix = 1;
      computeScopes(dc.y.data(), dc.w, dc.uv.data(), dc.w, dc.w, dc.h, ov, s);
      const double rpx = 0.95 * R / 224.0 * 0.9 * VS;  // footprint radius in pixels
      int inside = 0, zero = 0;
      for (uint32_t y = 0; y < VS; ++y)
        for (uint32_t x = 0; x < VS; ++x) {
          const double ddx = x + 0.5 - VS / 2.0, ddy = y + 0.5 - VS / 2.0;
          if (ddx * ddx + ddy * ddy > rpx * rpx) continue;
          ++inside;
          zero += alphaAt(s.vectorscope, VS, x, y) == 0;
        }
      std::printf("  vectorscope disc VS=%u: %d px inside, %d empty\n", VS, inside, zero);
      CHECK_MSG(inside > 0 && zero * 100 < inside, "VS=%u: %d of %d footprint pixels empty", VS, zero, inside);
    }
  }
  // (d) histogram of a limited-range gradient: no empty column in the range
  {
    Img gr(1024, 16);
    std::fill(gr.uv.begin(), gr.uv.end(), static_cast<uint8_t>(128));
    for (uint32_t r = 0; r < gr.h; ++r)
      for (uint32_t x = 0; x < gr.w; ++x) gr.y[r * gr.w + x] = static_cast<uint8_t>(16 + 219 * x / (gr.w - 1));
    for (uint32_t W : {240u, 512u, 1024u}) {
      ScopeOptions oh;
      oh.width = W;
      oh.step = 1;
      computeScopes(gr.y.data(), gr.w, gr.uv.data(), gr.w, gr.w, gr.h, oh, s);
      int empty = 0;
      for (uint32_t x = 0; x < W; ++x) empty += alphaAt(s.histogram, W, x, s.h - 1) == 0;
      CHECK_MSG(empty == 0, "histogram W=%u: %d empty columns", W, empty);
    }
  }
}

void testGradeAllOnBenchmark() {
  // Everything active: wheels, every curve, hue curves, a .cube file LUT.
  const uint32_t w = 1920, h = 1080;
  Img in(w, h);
  std::mt19937 rng(2);
  for (uint32_t r = 0; r < h; ++r)
    for (uint32_t x = 0; x < w; ++x)
      in.y[r * w + x] = static_cast<uint8_t>(std::min<uint32_t>(235, 16 + (x + r) * 219 / (w + h) + rng() % 8));
  for (uint32_t r = 0; r < h / 2; ++r)
    for (uint32_t x = 0; x < w / 2; ++x) {
      in.uv[r * w + 2 * x] = static_cast<uint8_t>(96 + x * 64 / (w / 2) + rng() % 4);
      in.uv[r * w + 2 * x + 1] = static_cast<uint8_t>(160 - r * 64 / (h / 2) + rng() % 4);
    }
  std::string err;
  const std::string cube = tempPath("owc_test_allon.cube");
  ColorGrade look;
  look.contrast = 0.2f;
  look.temperature = 0.3f;
  look.vibrance = 0.3f;
  CHECK(exportCube(look, cube, 33, err));
  ColorGrade g;
  g.enabled = true;
  g.exposure_ev = 0.2f;
  g.temperature = 0.1f;
  g.contrast = 0.2f;
  g.shadows = 0.1f;
  g.vibrance = 0.2f;
  g.lift = {0.05f, -0.025f, -0.025f, 0.02f};
  g.gamma = {-0.02f, 0.01f, 0.01f, 0.05f};
  g.gain = {0, 0.02f, -0.02f, 0.05f};
  g.offset = {0.01f, 0, -0.01f, 0};
  g.curve_y = {{0, 0}, {0.25f, 0.22f}, {0.75f, 0.8f}, {1, 1}};
  g.curve_r = {{0, 0}, {0.5f, 0.52f}, {1, 1}};
  g.curve_g = {{0, 0.02f}, {1, 1}};
  g.curve_b = {{0, 0}, {0.5f, 0.47f}, {1, 0.98f}};
  g.hue_vs_hue = {{0.05f, 0.52f}, {0.3f, 0.48f}, {0.6f, 0.5f}};
  g.hue_vs_sat = {{0.0f, 0.55f}, {0.33f, 0.45f}, {0.66f, 0.6f}};
  g.luma_vs_sat = {{0, 0.4f}, {0.5f, 0.5f}, {1, 0.45f}};
  g.lut = cube;
  g.lut_strength = 0.8f;
  ColorGrader gr;
  std::vector<uint8_t> buf;
  Img out(w, h);
  CHECK(gradeFrame(gr, g, 2, false, in, out, buf));
  CHECK_MSG(gr.status().error.empty() && gr.status().lutSize == 33, "%s", gr.status().error.c_str());
  double bakeBest = 1e9;
  for (int i = 0; i < 5; ++i) {
    g.contrast = 0.21f + 0.01f * i;  // steady-state re-bakes (LUT cached)
    CHECK(gradeFrame(gr, g, 2, false, in, out, buf));
    bakeBest = std::min(bakeBest, gr.status().bakeMs);
  }
  GradePipeline p;
  std::shared_ptr<const CubeLut> lut = cachedCubeFile(cube, err);
  auto t0 = std::chrono::steady_clock::now();
  p.prepare(g, lut);
  const double prepMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  GradeTable t;
  t0 = std::chrono::steady_clock::now();
  bakeGradeTable(p, 2, false, t, 1);
  const double bake1 = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  // apply: the ColorGrade (7 curve vectors) is passed per frame as present() does
  double total = 0, best = 1e9;
  const int frames = 60;
  for (int i = 0; i < frames + 5; ++i) {
    const ColorGrade perFrame = g;  // the getOpt() copy
    const uint8_t* y = in.y.data();
    const uint8_t* uv = in.uv.data();
    uint32_t ys = w, uvs = w;
    const auto a = std::chrono::steady_clock::now();
    gr.apply(perFrame, 2, false, y, ys, uv, uvs, w, h, buf);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    if (i >= 5) {
      total += ms;
      best = std::min(best, ms);
    }
  }
  std::printf("  all-on bake: %.1f ms (%d threads, best of 5), %.1f ms single-threaded (prepare %.2f ms)\n", bakeBest,
              std::max(1, std::min(4, static_cast<int>(std::thread::hardware_concurrency()) / 2)), bake1, prepMs);
  std::printf("  all-on 1080p apply: avg %.2f ms, best %.2f ms\n", total / frames, best);
  CHECK(bakeBest < 200);

  // gallery thumbnail: 1080p -> 192x108, then graded RGBA (cached .cube), single thread
  std::vector<uint8_t> small, rgba;
  t0 = std::chrono::steady_clock::now();
  downscaleNV12(in.y.data(), w, in.uv.data(), w, w, h, 192, 108, small);
  const double dsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  CHECK(renderGradedRgba(g, small.data(), 192, 108, 2, false, rgba, &err));  // warm the cache
  double rBest = 1e9, rTotal = 0;
  for (int i = 0; i < 10; ++i) {
    t0 = std::chrono::steady_clock::now();
    renderGradedRgba(g, small.data(), 192, 108, 2, false, rgba, &err);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    rBest = std::min(rBest, ms);
    rTotal += ms;
  }
  std::printf("  thumbnail 192x108 all-on: avg %.2f ms, best %.2f ms (downscale from 1080p %.2f ms)\n", rTotal / 10,
              rBest, dsMs);

  // scopes, 1080p, step 2, single thread
  ScopeOptions so;
  so.matrix = 2;
  ScopeImages si;
  double sBest = 1e9, sTotal = 0;
  for (int i = 0; i < 10; ++i) {
    t0 = std::chrono::steady_clock::now();
    computeScopes(in.y.data(), w, in.uv.data(), w, w, h, so, si);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    sBest = std::min(sBest, ms);
    sTotal += ms;
  }
  std::printf("  scopes 1080p step 2 (512x256, VS 256): avg %.2f ms, best %.2f ms\n", sTotal / 10, sBest);
  so.width = 1024;
  so.height = 512;
  so.vector_size = 512;
  sBest = 1e9, sTotal = 0;
  for (int i = 0; i < 10; ++i) {
    t0 = std::chrono::steady_clock::now();
    computeScopes(in.y.data(), w, in.uv.data(), w, w, h, so, si);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    sBest = std::min(sBest, ms);
    sTotal += ms;
  }
  std::printf("  scopes 1080p step 2 (W=1024 H=512 VS=512): avg %.2f ms, best %.2f ms\n", sTotal / 10, sBest);
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
  RUN(testCubeParse);
  RUN(testLutLookup);
  RUN(testGradeCurvesMonotonic);
  RUN(testGradeIdentity);
  RUN(testGradeStrengthZeroAndDisabled);
  RUN(testGradeBwLook);
  RUN(testGradeCompareSplit);
  RUN(testGradeCoalesce);
  RUN(testGradeLutFileErrorAndReload);
  RUN(testExportCube);
  RUN(testGradeBenchmark);
  RUN(testWheels);
  RUN(testCurveEval);
  RUN(testGradeWheels);
  RUN(testGradeCurvesStage);
  RUN(testGradeNeutralNewFields);
  RUN(testExportCubeWheels);
  RUN(testRenderGradedRgba);
  RUN(testScopes);
  RUN(testGradeAllOnBenchmark);
  std::printf("%s (%d failure(s))\n", owctest::failures() ? "FAILED" : "ALL PASSED", owctest::failures());
  return owctest::failures() ? 1 : 0;
}
