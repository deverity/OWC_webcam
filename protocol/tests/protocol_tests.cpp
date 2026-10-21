// Self-contained tests for the OWC wire protocol. No external dependencies.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "owc/protocol.h"

using namespace owc;

static int g_checks = 0, g_fails = 0, g_tests = 0, g_test_fails = 0;

#define CHECK(cond)                                                              \
  do {                                                                           \
    ++g_checks;                                                                  \
    if (!(cond)) {                                                               \
      ++g_fails;                                                                 \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                            \
  } while (0)

// ------------------------------------------------------------------ helpers
struct Rng {  // xorshift64*, deterministic
  uint64_t s;
  explicit Rng(uint64_t seed) : s(seed ? seed : 1) {}
  uint64_t next() {
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return s * 2685821657736338717ull;
  }
  uint32_t u32() { return static_cast<uint32_t>(next() >> 32); }
  size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
  void fill(std::vector<uint8_t>& v) { for (auto& b : v) b = static_cast<uint8_t>(next() >> 56); }
};

static std::vector<uint8_t> pattern(size_t n, uint32_t seed = 1) {
  std::vector<uint8_t> v(n);
  Rng r(seed * 7919u + 13u);
  r.fill(v);
  return v;
}

static const char* kUnicode = "C\xC3\xA2mera \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x93\xB7 \xD0\x9F\xD1\x80\xD0\xB8";

static bool eq(const ParamDesc& a, const ParamDesc& b) {
  if (a.id != b.id || a.type != b.type || a.flags != b.flags || a.min != b.min || a.max != b.max ||
      a.step != b.step || !(a.current == b.current) || a.options.size() != b.options.size())
    return false;
  for (size_t i = 0; i < a.options.size(); ++i)
    if (a.options[i].value != b.options[i].value || a.options[i].label != b.options[i].label)
      return false;
  return true;
}
static bool eq(const CameraInfo& a, const CameraInfo& b) {
  if (a.id != b.id || a.facing != b.facing || a.hw_level != b.hw_level ||
      a.sensor_orientation != b.sensor_orientation || a.max_zoom != b.max_zoom ||
      a.has_flash != b.has_flash || a.fps_ranges != b.fps_ranges || a.sizes.size() != b.sizes.size())
    return false;
  for (size_t i = 0; i < a.sizes.size(); ++i)
    if (a.sizes[i].width != b.sizes[i].width || a.sizes[i].height != b.sizes[i].height ||
        a.sizes[i].max_fps != b.sizes[i].max_fps)
      return false;
  return true;
}
static bool eq(const Capabilities& a, const Capabilities& b) {
  if (a.active_camera != b.active_camera || a.cameras.size() != b.cameras.size() ||
      a.params.size() != b.params.size())
    return false;
  for (size_t i = 0; i < a.cameras.size(); ++i) if (!eq(a.cameras[i], b.cameras[i])) return false;
  for (size_t i = 0; i < a.params.size(); ++i) if (!eq(a.params[i], b.params[i])) return false;
  return true;
}

// Every decoder must reject every strict prefix of a valid payload. (Decoders
// ignore trailing bytes, and our layouts have no optional tail.)
template <class T>
static void checkTruncations(const std::vector<uint8_t>& enc) {
  for (size_t n = 0; n < enc.size(); ++n) {
    T t;
    CHECK(!decode(enc.data(), n, t));
  }
  T t;
  CHECK(decode(enc.data(), enc.size(), t));
  // trailing garbage tolerated
  std::vector<uint8_t> more = enc;
  more.push_back(0xAB);
  CHECK(decode(more.data(), more.size(), t));
}

static Capabilities makeCaps(size_t ncam, size_t nparam, size_t nopt) {
  Capabilities c;
  c.active_camera = 1;
  for (size_t i = 0; i < ncam; ++i) {
    CameraInfo ci;
    ci.id = std::to_string(i) + kUnicode;
    ci.facing = static_cast<Facing>(i % 3);
    ci.hw_level = static_cast<uint8_t>(i % 5);
    ci.sensor_orientation = (i % 4) * 90 - (i == 3 ? 360 : 0);
    ci.max_zoom = 1.0f + 0.5f * static_cast<float>(i);
    ci.has_flash = (i & 1) != 0;
    for (size_t k = 0; k < 5 + i; ++k)
      ci.sizes.push_back({static_cast<uint16_t>(640 + k * 160), static_cast<uint16_t>(480 + k * 90),
                          static_cast<uint16_t>(30 + k)});
    for (size_t k = 0; k < 3; ++k)
      ci.fps_ranges.emplace_back(static_cast<uint16_t>(15 + k), static_cast<uint16_t>(30 + k * 15));
    c.cameras.push_back(ci);
  }
  for (size_t i = 0; i < nparam; ++i) {
    ParamDesc d;
    d.id = i % 2 ? ParamId::AwbMode : ParamId::ExposureTimeNs;
    d.type = static_cast<ValueType>(i % 5);
    d.flags = static_cast<uint8_t>(i % 8);
    d.min = -1.5 * static_cast<double>(i); d.max = 1e300; d.step = 0.001;
    switch (d.type) {
      case ValueType::Bool: d.current = ParamValue::Bool(i % 3 == 0); break;
      case ValueType::Int32: d.current = ParamValue::Int(-static_cast<int32_t>(i) * 1000); break;
      case ValueType::Int64: d.current = ParamValue::Long(-(1ll << 40) - static_cast<int64_t>(i)); break;
      case ValueType::Float: d.current = ParamValue::Real(3.25f * static_cast<float>(i)); break;
      case ValueType::Enum: d.current = ParamValue::EnumV(static_cast<int32_t>(i)); break;
    }
    for (size_t k = 0; k < nopt; ++k)
      d.options.push_back({static_cast<int32_t>(k) - 3, std::string("opt") + kUnicode + std::to_string(k)});
    c.params.push_back(d);
  }
  return c;
}

// ---------------------------------------------------------- message tests
static void testMessages() {
  {  // Hello
    Hello h; h.client_name = kUnicode; h.transport = VideoTransport::Udp;
    auto e = encode(h);
    Hello o; CHECK(decode(e.data(), e.size(), o));
    CHECK(o.magic == kMagic && o.version == kProtocolVersion && o.transport == VideoTransport::Udp);
    CHECK(o.client_name == kUnicode);
    // v2: the client_id trailer is optional on decode, so only the v1 part must be complete
    for (size_t n = 0; n < e.size() - kIdSize; ++n) { Hello t; CHECK(!decode(e.data(), n, t)); }
    // exact bytes: "OWC1" magic LE, version 2, transport, "ab", 16-byte client_id
    Hello h2; h2.client_name = "ab";
    for (size_t i = 0; i < kIdSize; ++i) h2.client_id[i] = static_cast<uint8_t>(0xA0 + i);
    auto e2 = encode(h2);
    uint8_t exp[11 + kIdSize] = {0x4F, 0x57, 0x43, 0x31, 2, 0, 0, 2, 0, 'a', 'b'};
    for (size_t i = 0; i < kIdSize; ++i) exp[11 + i] = static_cast<uint8_t>(0xA0 + i);
    CHECK(e2.size() == sizeof(exp) && std::memcmp(e2.data(), exp, sizeof(exp)) == 0);
    // wrong magic/version still parses
    Hello bad; bad.magic = 0x12345678; bad.version = 99;
    auto eb = encode(bad);
    Hello ob; CHECK(decode(eb.data(), eb.size(), ob));
    CHECK(ob.magic == 0x12345678u && ob.version == 99);
    // max-length and over-length strings
    Hello big; big.client_name.assign(65535, 'x');
    auto ebig = encode(big);
    Hello obig; CHECK(decode(ebig.data(), ebig.size(), obig) && obig.client_name.size() == 65535);
    big.client_name.assign(70000, 'y');
    ebig = encode(big);
    CHECK(decode(ebig.data(), ebig.size(), obig) && obig.client_name.size() == 65535);
    Hello empty; auto ee = encode(empty);
    CHECK(decode(ee.data(), ee.size(), obig) && obig.client_name.empty());
  }
  {  // HelloAck
    HelloAck a; a.status = HelloStatus::Busy; a.session_id = 0xDEADBEEF; a.udp_port = 12345;
    a.device_name = kUnicode; a.model = std::string(65535, 'm'); a.android_api = 34;
    a.codec_mask = codecBit(Codec::H264) | codecBit(Codec::Hevc);
    auto e = encode(a);
    HelloAck o; CHECK(decode(e.data(), e.size(), o));
    CHECK(o.status == HelloStatus::Busy && o.session_id == 0xDEADBEEFu && o.udp_port == 12345);
    CHECK(o.device_name == kUnicode && o.model.size() == 65535 && o.android_api == 34);
    CHECK(o.codec_mask == 3 && o.magic == kMagic && o.version == kProtocolVersion);
    a.model = "M";
    {  // v2 trailer is optional on decode: only the v1 part must be complete
      auto ea = encode(a);
      for (size_t n = 0; n < ea.size() - (2 * kIdSize + 1); ++n) {
        HelloAck t; CHECK(!decode(ea.data(), n, t));
      }
      HelloAck t; CHECK(decode(ea.data(), ea.size(), t));
    }
  }
  {  // Capabilities
    Capabilities empty;
    auto e0 = encode(empty);
    Capabilities o0; CHECK(decode(e0.data(), e0.size(), o0) && eq(empty, o0));
    checkTruncations<Capabilities>(e0);
    Capabilities c = makeCaps(8, 12, 6);
    auto e = encode(c);
    Capabilities o; CHECK(decode(e.data(), e.size(), o) && eq(c, o));
    Capabilities small = makeCaps(2, 3, 2);
    checkTruncations<Capabilities>(encode(small));
    Capabilities many = makeCaps(40, 200, 40);
    auto em = encode(many);
    Capabilities om; CHECK(decode(em.data(), em.size(), om) && eq(many, om));
    // bad ParamValue type in a param => reject
    Capabilities one = makeCaps(0, 1, 0);
    auto eo = encode(one);
    // layout: u16 active, u16 ncam(0), u16 nparams, then desc: u16 id,u8 type,u8 flags,3*f64 = 28 -> value type at 6+28
    eo[6 + 28] = 99;
    Capabilities ob; CHECK(!decode(eo.data(), eo.size(), ob));
    // absurd counts must not allocate/crash
    std::vector<uint8_t> evil = {0, 0, 0xFF, 0xFF};
    CHECK(!decode(evil.data(), evil.size(), ob));
  }
  {  // SetParam / ParamState with every value type + every ParamId
    const ParamValue vals[] = {ParamValue::Bool(true), ParamValue::Bool(false),
                               ParamValue::Int(-5), ParamValue::Int(INT32_MIN), ParamValue::Int(INT32_MAX),
                               ParamValue::Long(INT64_MIN), ParamValue::Long(INT64_MAX),
                               ParamValue::Real(-0.0f), ParamValue::Real(1.5e-30f),
                               ParamValue::EnumV(7)};
    for (const auto& v : vals) {
      SetParam s; s.id = ParamId::Zoom; s.value = v;
      auto e = encode(s);
      SetParam o; CHECK(decode(e.data(), e.size(), o) && o.id == ParamId::Zoom && o.value == v);
      checkTruncations<SetParam>(e);
      ParamState ps; ps.id = ParamId::Iso; ps.status = ParamStatus::OutOfRange; ps.value = v;
      auto e2 = encode(ps);
      ParamState o2; CHECK(decode(e2.data(), e2.size(), o2) && o2.status == ParamStatus::OutOfRange &&
                           o2.id == ParamId::Iso && o2.value == v);
      checkTruncations<ParamState>(e2);
    }
    const ParamId ids[] = {ParamId::CameraIndex, ParamId::VideoWidth, ParamId::VideoHeight, ParamId::Fps,
        ParamId::VideoCodec, ParamId::Bitrate, ParamId::KeyframeIntervalMs, ParamId::BitrateMode,
        ParamId::IntraRefreshFrames, ParamId::FecPercent, ParamId::AdaptiveBitrate, ParamId::EncoderProfile,
        ParamId::MaxSendQueueFrames, ParamId::BFrames, ParamId::QpMin, ParamId::QpMax,
        ParamId::LatencyDropPolicy, ParamId::HighSpeedMode, ParamId::AfMode, ParamId::FocusDistance, ParamId::AeMode,
        ParamId::AeLock, ParamId::ExposureCompensation, ParamId::Iso, ParamId::ExposureTimeNs,
        ParamId::AwbMode, ParamId::AwbLock, ParamId::Zoom, ParamId::Torch, ParamId::AntiBanding,
        ParamId::VideoStabilization, ParamId::OpticalStabilization, ParamId::NoiseReduction,
        ParamId::EdgeMode, ParamId::ScreenMode};
    for (ParamId id : ids) {
      SetParam s; s.id = id; s.value = ParamValue::Int(static_cast<int32_t>(id));
      auto e = encode(s);
      SetParam o; CHECK(decode(e.data(), e.size(), o) && o.id == id);
      CHECK(std::strcmp(toString(id), "Unknown") != 0 && toString(id)[0] != 0);
      ParamDesc d; d.id = id;
      Capabilities c; c.params.push_back(d);
      auto ec = encode(c);
      Capabilities oc; CHECK(decode(ec.data(), ec.size(), oc) && oc.params.size() == 1 && oc.params[0].id == id);
    }
    CHECK(std::strcmp(toString(static_cast<ParamId>(9999)), "Unknown") == 0);
    // bad value type
    std::vector<uint8_t> bad = {1, 0, 9, 0, 0, 0, 0};
    SetParam o; CHECK(!decode(bad.data(), bad.size(), o));
    // exact wire: SetParam{Zoom=29, Float 1.0}
    SetParam z; z.id = ParamId::Zoom; z.value = ParamValue::Real(1.0f);
    auto ez = encode(z);
    const uint8_t exp[] = {29, 0, 3, 0, 0, 0x80, 0x3F};
    CHECK(ez.size() == sizeof(exp) && std::memcmp(ez.data(), exp, sizeof(exp)) == 0);
  }
  {  // Command
    Command c; c.cmd = CommandId::FocusAt; c.arg = -123456789012345ll; c.x = 0.25f; c.y = 0.75f;
    c.text = kUnicode;
    auto e = encode(c);
    CHECK(e.size() == 2 + 8 + 4 + 4 + 2 + std::strlen(kUnicode));
    Command o; CHECK(decode(e.data(), e.size(), o));
    CHECK(o.cmd == CommandId::FocusAt && o.arg == c.arg && o.x == 0.25f && o.y == 0.75f && o.text == kUnicode);
    checkTruncations<Command>(e);
    Command c2; c2.text.assign(65535, 'z');
    auto e2 = encode(c2);
    CHECK(decode(e2.data(), e2.size(), o) && o.text.size() == 65535);
  }
  {  // VideoFormat
    VideoFormat v; v.epoch = 0xFFFFFFFEu; v.codec = Codec::Hevc; v.width = 3840; v.height = 2160;
    v.fps = 60; v.rotation_deg = -90; v.mirror = true; v.csd = pattern(300);
    auto e = encode(v);
    CHECK(e.size() == 4 + 1 + 2 + 2 + 2 + 2 + 1 + 4 + 300);
    VideoFormat o; CHECK(decode(e.data(), e.size(), o));
    CHECK(o.epoch == v.epoch && o.codec == Codec::Hevc && o.width == 3840 && o.height == 2160 &&
          o.fps == 60 && o.rotation_deg == -90 && o.mirror && o.csd == v.csd);
    checkTruncations<VideoFormat>(e);
    VideoFormat e0; auto ee = encode(e0);
    CHECK(decode(ee.data(), ee.size(), o) && o.csd.empty());
    // csd length larger than payload
    auto bad = encode(v); bad[14] = 0xFF; bad[15] = 0xFF; bad[16] = 0xFF; bad[17] = 0x7F;
    CHECK(!decode(bad.data(), bad.size(), o));
  }
  {  // Ping / Pong
    Ping p; p.t0_us = 0x0102030405060708ull;
    auto e = encode(p);
    const uint8_t exp[] = {8, 7, 6, 5, 4, 3, 2, 1};
    CHECK(e.size() == 8 && std::memcmp(e.data(), exp, 8) == 0);
    Ping o; CHECK(decode(e.data(), e.size(), o) && o.t0_us == p.t0_us);
    checkTruncations<Ping>(e);
    Pong q; q.t0_us = ~0ull; q.phone_us = 42;
    auto e2 = encode(q);
    Pong o2; CHECK(decode(e2.data(), e2.size(), o2) && o2.t0_us == ~0ull && o2.phone_us == 42);
    checkTruncations<Pong>(e2);
  }
  {  // Stats
    Stats s; s.camera_fps = 29.97f; s.encode_fps = 30; s.send_fps = 29.5f; s.bitrate_bps = 12000000;
    s.dropped_frames = 77; s.send_queue = 2; s.encode_latency_ms = 11.5f; s.battery_pct = -1;
    s.battery_temp_c = 38.5f; s.charging = true; s.thermal_status = 3;
    s.target_bitrate_bps = 9000000; s.isp_latency_ms = 61.5f; s.fec_percent = -1;
    auto e = encode(s);
    Stats o; CHECK(decode(e.data(), e.size(), o));
    CHECK(o.camera_fps == s.camera_fps && o.encode_fps == 30 && o.send_fps == 29.5f &&
          o.bitrate_bps == 12000000 && o.dropped_frames == 77 && o.send_queue == 2 &&
          o.encode_latency_ms == 11.5f && o.battery_pct == -1 && o.battery_temp_c == 38.5f &&
          o.charging && o.thermal_status == 3 && o.target_bitrate_bps == 9000000 &&
          o.isp_latency_ms == 61.5f && o.fec_percent == -1);
    {  // v1.0 payload without trailer still decodes
      Stats old; CHECK(decode(e.data(), e.size() - 9, old) && old.thermal_status == 3 &&
                       old.target_bitrate_bps == 0);
    }
    s.battery_pct = 100; e = encode(s);
    CHECK(decode(e.data(), e.size(), o) && o.battery_pct == 100);
    // v1.0 part must be complete; the 9-byte v1.1 trailer is optional
    std::vector<uint8_t> v10(e.begin(), e.end() - 9);
    checkTruncations<Stats>(v10);
    for (size_t n = v10.size(); n < e.size(); ++n) { Stats x; CHECK(decode(e.data(), n, x)); }
  }
  {  // LogMsg
    LogMsg l; l.level = LogLevel::Error; l.text = kUnicode;
    auto e = encode(l);
    LogMsg o; CHECK(decode(e.data(), e.size(), o) && o.level == LogLevel::Error && o.text == kUnicode);
    checkTruncations<LogMsg>(e);
    l.text.assign(70000, 'q'); e = encode(l);
    CHECK(decode(e.data(), e.size(), o) && o.text.size() == 65535);
    l.text.clear(); e = encode(l);
    CHECK(decode(e.data(), e.size(), o) && o.text.empty());
  }
  {  // ReceiverReport
    ReceiverReport r; r.frames_received = 1; r.frames_lost = 2; r.fragments_received = 0xFFFFFFFFu;
    r.fragments_lost = 4; r.jitter_ms = 1.25f; r.delay_gradient_ms = -2.5f; r.recv_bitrate_bps = 8000000;
    r.decode_ms = 3.5f;
    auto e = encode(r);
    CHECK(e.size() == 32);
    ReceiverReport o; CHECK(decode(e.data(), e.size(), o));
    CHECK(o.frames_received == 1 && o.frames_lost == 2 && o.fragments_received == 0xFFFFFFFFu &&
          o.fragments_lost == 4 && o.jitter_ms == 1.25f && o.delay_gradient_ms == -2.5f &&
          o.recv_bitrate_bps == 8000000 && o.decode_ms == 3.5f);
    checkTruncations<ReceiverReport>(e);
  }
  {  // VideoFrame
    VideoFrameHeader h; h.epoch = 9; h.frame_seq = 0xFFFFFFFFu; h.capture_ts_us = 1ull << 60; h.flags = kFrameKey;
    auto au = pattern(1000);
    std::vector<uint8_t> out(5, 1);
    encodeVideoFrame(h, au.data(), au.size(), out);
    CHECK(out.size() == kVideoFrameHeaderSize + 1000);
    VideoFrameHeader o; const uint8_t* p = nullptr; size_t n = 0;
    CHECK(decodeVideoFrame(out.data(), out.size(), o, p, n));
    CHECK(o.epoch == 9 && o.frame_seq == 0xFFFFFFFFu && o.capture_ts_us == (1ull << 60) && o.flags == kFrameKey);
    CHECK(n == 1000 && p == out.data() + 17 && std::memcmp(p, au.data(), n) == 0);
    for (size_t k = 0; k < kVideoFrameHeaderSize; ++k) CHECK(!decodeVideoFrame(out.data(), k, o, p, n));
    encodeVideoFrame(h, nullptr, 0, out);
    CHECK(out.size() == 17 && decodeVideoFrame(out.data(), out.size(), o, p, n) && n == 0);
  }
  {  // Discovery
    auto rq = encodeDiscoveryRequest();
    CHECK(rq.size() == 7 && isDiscoveryRequest(rq.data(), rq.size()));
    for (size_t k = 0; k < rq.size(); ++k) CHECK(!isDiscoveryRequest(rq.data(), k));
    auto bad = rq; bad[0] ^= 1; CHECK(!isDiscoveryRequest(bad.data(), bad.size()));
    bad = rq; bad[6] = 2; CHECK(!isDiscoveryRequest(bad.data(), bad.size()));
    DiscoveryReply r; r.control_port = 1111; r.udp_port = 2222; r.device_name = kUnicode;
    r.model = "Pixel"; r.busy = true;
    auto e = encode(r);
    CHECK(!isDiscoveryRequest(e.data(), e.size()));
    DiscoveryReply o; CHECK(decode(e.data(), e.size(), o));
    CHECK(o.control_port == 1111 && o.udp_port == 2222 && o.device_name == kUnicode &&
          o.model == "Pixel" && o.busy && !o.announce);
    checkTruncations<DiscoveryReply>(e);
    r.announce = true;
    auto ea = encode(r);
    CHECK(ea.size() == e.size() && ea[6] == 3 && !isDiscoveryRequest(ea.data(), ea.size()));
    DiscoveryReply oa; CHECK(decode(ea.data(), ea.size(), oa) && oa.announce && oa.busy);
    bad = ea; bad[6] = 4; CHECK(!decode(bad.data(), bad.size(), oa));
    CHECK(!decode(rq.data(), rq.size(), o));
    bad = e; bad[1] ^= 0xFF; CHECK(!decode(bad.data(), bad.size(), o));
  }
  {  // toString
    const MsgType ts[] = {MsgType::Hello, MsgType::HelloAck, MsgType::Capabilities,
        MsgType::PairBegin, MsgType::PairCode, MsgType::AuthProof, MsgType::AuthResult, MsgType::SetParam,
        MsgType::ParamState, MsgType::Command, MsgType::VideoFormat, MsgType::VideoFrame, MsgType::Ping,
        MsgType::Pong, MsgType::Stats, MsgType::ReceiverReport, MsgType::Log, MsgType::Bye};
    for (MsgType t : ts) CHECK(std::strcmp(toString(t), "Unknown") != 0);
    CHECK(std::strcmp(toString(MsgType::Bye), "Bye") == 0);
    CHECK(std::strcmp(toString(MsgType::PairBegin), "PairBegin") == 0);
    CHECK(std::strcmp(toString(MsgType::PairCode), "PairCode") == 0);
    CHECK(std::strcmp(toString(MsgType::AuthProof), "AuthProof") == 0);
    CHECK(std::strcmp(toString(MsgType::AuthResult), "AuthResult") == 0);
    CHECK(std::strcmp(toString(static_cast<MsgType>(777)), "Unknown") == 0);
    CHECK(std::strcmp(toString(ParamId::BFrames), "BFrames") == 0);
  }
}

// ---------------------------------------------------------- pairing / auth
static std::string hex32(const Key32& k) { return toHex(k.data(), k.size()); }
static std::string hexOf(const std::string& s) {
  return hex32(sha256(reinterpret_cast<const uint8_t*>(s.data()), s.size()));
}
static std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
static std::string hmacHex(const std::vector<uint8_t>& key, const std::vector<uint8_t>& msg) {
  return hex32(hmacSha256(key.data(), key.size(), msg.data(), msg.size()));
}
static Id16 makeId(uint8_t seed) {
  Id16 id;
  for (size_t i = 0; i < id.size(); ++i) id[i] = static_cast<uint8_t>(seed + 7 * i);
  return id;
}

static void testSha256() {
  CHECK(hexOf("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hexOf("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  std::string million(1000000, 'a');
  CHECK(hexOf(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  // null pointer with length 0 is the empty message
  CHECK(hex32(sha256(nullptr, 0)) == hexOf(""));
}

static void testHmac() {
  // RFC 4231 test cases 1, 2, 3, 4, 6, 7
  CHECK(hmacHex(std::vector<uint8_t>(20, 0x0b), bytesOf("Hi There")) ==
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
  CHECK(hmacHex(bytesOf("Jefe"), bytesOf("what do ya want for nothing?")) ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  CHECK(hmacHex(std::vector<uint8_t>(20, 0xaa), std::vector<uint8_t>(50, 0xdd)) ==
        "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
  std::vector<uint8_t> k4(25);
  for (size_t i = 0; i < k4.size(); ++i) k4[i] = static_cast<uint8_t>(i + 1);
  CHECK(hmacHex(k4, std::vector<uint8_t>(50, 0xcd)) ==
        "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
  CHECK(hmacHex(std::vector<uint8_t>(131, 0xaa),
                bytesOf("Test Using Larger Than Block-Size Key - Hash Key First")) ==
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  CHECK(hmacHex(std::vector<uint8_t>(131, 0xaa),
                bytesOf("This is a test using a larger than block-size key and a larger than "
                        "block-size data. The key needs to be hashed before being used by the "
                        "HMAC algorithm.")) ==
        "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

static void testAuthMac() {
  Key32 key; for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(0x40 + i);
  Id16 ph = makeId(1), cl = makeId(50), no = makeId(99);
  Key32 m = authMac(key, ph, cl, no);
  CHECK(m == authMac(key, ph, cl, no));
  // matches the documented construction
  std::vector<uint8_t> msg = bytesOf("OWC-AUTH-2");
  CHECK(msg.size() == 10);
  msg.insert(msg.end(), ph.begin(), ph.end());
  msg.insert(msg.end(), cl.begin(), cl.end());
  msg.insert(msg.end(), no.begin(), no.end());
  CHECK(m == hmacSha256(key.data(), key.size(), msg.data(), msg.size()));
  // every input byte matters
  for (size_t i = 0; i < kKeySize; ++i) { Key32 k = key; k[i] ^= 1; CHECK(authMac(k, ph, cl, no) != m); }
  for (size_t i = 0; i < kIdSize; ++i) {
    Id16 x = ph; x[i] ^= 1; CHECK(authMac(key, x, cl, no) != m);
    x = cl; x[i] ^= 1; CHECK(authMac(key, ph, x, no) != m);
    x = no; x[i] ^= 1; CHECK(authMac(key, ph, cl, x) != m);
  }
  // roles are not interchangeable
  CHECK(authMac(key, cl, ph, no) != m);
}

static void testCtEqualHex() {
  const uint8_t a[4] = {1, 2, 3, 4};
  uint8_t b[4] = {1, 2, 3, 4};
  CHECK(constantTimeEqual(a, b, 4));
  CHECK(constantTimeEqual(a, b, 0));
  for (size_t i = 0; i < 4; ++i) {
    uint8_t c[4] = {1, 2, 3, 4};
    c[i] ^= 0x80;
    CHECK(!constantTimeEqual(a, c, 4));
  }
  CHECK(constantTimeEqual(a, b, 3));
  // hex
  const uint8_t d[5] = {0x00, 0x0f, 0xa5, 0xff, 0x10};
  std::string h = toHex(d, 5);
  CHECK(h == "000fa5ff10");
  CHECK(toHex(d, 0).empty());
  uint8_t o[5] = {};
  CHECK(fromHex(h, o, 5) && std::memcmp(o, d, 5) == 0);
  uint8_t o2[5] = {};
  CHECK(fromHex("000FA5Ff10", o2, 5) && std::memcmp(o2, d, 5) == 0);
  uint8_t e0[1] = {0x77};
  CHECK(fromHex("", e0, 0) && e0[0] == 0x77);
  // bad input: wrong length, odd length, bad characters; `out` untouched
  uint8_t z[5]; std::memset(z, 0x5A, 5);
  const char* bad[] = {"000fa5ff1", "000fa5ff100", "000fa5ff1g", "00 fa5ff10", "0x0fa5ff10", "", "000fa5ff1\0"};
  for (const char* s : bad) {
    CHECK(!fromHex(s, z, 5));
    bool same = true;
    for (int i = 0; i < 5; ++i) same = same && z[i] == 0x5A;
    CHECK(same);
  }
  // bad character in the LAST position must not leave a partial write either
  CHECK(!fromHex(std::string("0102030405").replace(9, 1, "z"), z, 5) && z[0] == 0x5A);
  // full-size key roundtrip
  Key32 k = sha256(d, 5), k2{};
  CHECK(fromHex(hex32(k), k2.data(), k2.size()) && k == k2);
}

static void testPairMessages() {
  {  // PairCode
    PairCode m; m.code = "042917";
    auto e = encode(m);
    CHECK(e.size() == 2 + 6 && e[0] == 6 && e[1] == 0);
    PairCode o; CHECK(decode(e.data(), e.size(), o) && o.code == "042917");
    checkTruncations<PairCode>(e);
    PairCode empty; auto ee = encode(empty);
    PairCode oe; oe.code = "x";
    CHECK(decode(ee.data(), ee.size(), oe) && oe.code.empty());
    // a failed decode does not clobber the destination
    PairCode keep; keep.code = "keep";
    CHECK(!decode(e.data(), e.size() - 1, keep) && keep.code == "keep");
  }
  {  // AuthProof
    AuthProof m; for (size_t i = 0; i < 32; ++i) m.mac[i] = static_cast<uint8_t>(i * 3 + 1);
    auto e = encode(m);
    CHECK(e.size() == 32);
    AuthProof o; CHECK(decode(e.data(), e.size(), o) && o.mac == m.mac);
    checkTruncations<AuthProof>(e);
  }
  {  // AuthResult
    AuthResult m; m.status = AuthStatus::CodeShown; m.attempts_left = 3; m.seconds = 0x0102;
    for (size_t i = 0; i < 32; ++i) m.key[i] = static_cast<uint8_t>(0xF0 - i);
    auto e = encode(m);
    CHECK(e.size() == 36 && e[0] == 1 && e[1] == 3 && e[2] == 0x02 && e[3] == 0x01 && e[4] == 0xF0);
    AuthResult o; CHECK(decode(e.data(), e.size(), o));
    CHECK(o.status == AuthStatus::CodeShown && o.attempts_left == 3 && o.seconds == 0x0102 &&
          o.key == m.key);
    checkTruncations<AuthResult>(e);
    AuthResult z; z.status = AuthStatus::Locked; z.seconds = 30;
    auto ez = encode(z);
    AuthResult oz; oz.key[0] = 9;
    CHECK(decode(ez.data(), ez.size(), oz) && oz.status == AuthStatus::Locked && oz.seconds == 30 &&
          oz.key == Key32{});
  }
}

static void testHelloV2() {
  Hello h; h.client_name = "pc"; h.transport = VideoTransport::Tcp; h.client_id = makeId(0x11);
  auto e = encode(h);
  CHECK(e.size() == 4 + 2 + 1 + 2 + 2 + kIdSize);
  Hello o; CHECK(decode(e.data(), e.size(), o));
  CHECK(o.version == 2 && o.client_name == "pc" && o.client_id == h.client_id);
  // v1-style payload (no trailer) decodes with zero id, even into a dirty destination
  std::vector<uint8_t> v1(e.begin(), e.end() - kIdSize);
  Hello o1; o1.client_id = makeId(5);
  CHECK(decode(v1.data(), v1.size(), o1) && o1.client_id == Id16{} && o1.client_name == "pc");
  // partial trailer (1..15 bytes) behaves like absent
  for (size_t extra = 1; extra < kIdSize; ++extra) {
    Hello op; op.client_id = makeId(5);
    CHECK(decode(e.data(), v1.size() + extra, op) && op.client_id == Id16{} && op.client_name == "pc");
  }
}

static void testHelloAckV2() {
  HelloAck a; a.status = HelloStatus::AuthRequired; a.session_id = 77; a.device_name = "Pixel";
  a.model = "P7"; a.android_api = 34; a.codec_mask = 3;
  a.phone_id = makeId(0x21); a.nonce = makeId(0x31); a.paired = true;
  auto e = encode(a);
  HelloAck o; CHECK(decode(e.data(), e.size(), o));
  CHECK(o.status == HelloStatus::AuthRequired && o.session_id == 77 && o.device_name == "Pixel" &&
        o.model == "P7" && o.android_api == 34 && o.codec_mask == 3);
  CHECK(o.phone_id == a.phone_id && o.nonce == a.nonce && o.paired);
  a.paired = false;
  auto en = encode(a);
  HelloAck on; on.paired = true; CHECK(decode(en.data(), en.size(), on) && !on.paired);
  // v1-style payload: zeros/false (dirty destination is reset)
  const size_t trailer = 2 * kIdSize + 1;
  std::vector<uint8_t> v1(e.begin(), e.end() - trailer);
  HelloAck o1; o1.phone_id = makeId(1); o1.nonce = makeId(2); o1.paired = true;
  CHECK(decode(v1.data(), v1.size(), o1) && o1.phone_id == Id16{} && o1.nonce == Id16{} && !o1.paired);
  CHECK(o1.codec_mask == 3 && o1.device_name == "Pixel");
  // any partial trailer behaves like absent
  for (size_t extra = 1; extra < trailer; ++extra) {
    HelloAck op; op.paired = true; op.phone_id = makeId(1);
    CHECK(decode(e.data(), v1.size() + extra, op) && op.phone_id == Id16{} && op.nonce == Id16{} &&
          !op.paired);
  }
}

// ---------------------------------------------------------- TCP framing
static std::vector<uint8_t> buildStream(const std::vector<std::pair<MsgType, std::vector<uint8_t>>>& ms) {
  std::vector<uint8_t> s;
  for (auto& m : ms) appendTcpMessage(s, m.first, m.second);
  return s;
}

static void testTcp() {
  {  // header layout
    uint8_t h[kTcpHeaderSize];
    writeTcpHeader(h, MsgType::VideoFrame, 0x01020304u);
    const uint8_t exp[] = {21, 0, 0, 0, 4, 3, 2, 1};
    CHECK(std::memcmp(h, exp, 8) == 0);
  }
  std::vector<std::pair<MsgType, std::vector<uint8_t>>> msgs;
  Rng rng(42);
  for (int i = 0; i < 50; ++i) {
    std::vector<uint8_t> p(rng.below(i % 7 == 0 ? 20000 : 300));
    rng.fill(p);
    msgs.push_back({static_cast<MsgType>(i % 3 == 0 ? 999 : 1 + i % 50), p});
  }
  msgs.push_back({MsgType::Bye, {}});
  auto stream = buildStream(msgs);

  auto drain = [&](TcpFrameParser& P, size_t& got) {
    TcpFrameParser::Message m;
    while (P.next(m)) {
      CHECK(got < msgs.size());
      if (got < msgs.size()) {
        CHECK(m.type == msgs[got].first);
        CHECK(m.payload == msgs[got].second);
      }
      ++got;
    }
  };
  {  // one shot
    TcpFrameParser P; size_t got = 0;
    CHECK(P.feed(stream.data(), stream.size()));
    drain(P, got);
    CHECK(got == msgs.size() && P.buffered() == 0 && !P.error());
  }
  {  // byte by byte
    TcpFrameParser P; size_t got = 0;
    for (uint8_t b : stream) { CHECK(P.feed(&b, 1)); drain(P, got); }
    CHECK(got == msgs.size() && P.buffered() == 0);
  }
  for (int iter = 0; iter < 30; ++iter) {  // random chunks
    TcpFrameParser P; size_t got = 0, off = 0;
    size_t maxchunk = 1 + rng.below(iter % 2 ? 50 : 5000);
    while (off < stream.size()) {
      size_t n = std::min(stream.size() - off, 1 + rng.below(maxchunk));
      CHECK(P.feed(stream.data() + off, n));
      off += n;
      if (rng.below(3)) drain(P, got);  // sometimes let several accumulate
    }
    drain(P, got);
    CHECK(got == msgs.size() && P.buffered() == 0);
  }
  {  // buffer does not grow unbounded: stream many messages, drain each time
    TcpFrameParser P;
    std::vector<uint8_t> one; appendTcpMessage(one, MsgType::Ping, std::vector<uint8_t>(100, 7));
    size_t maxb = 0; TcpFrameParser::Message m; int cnt = 0;
    for (int i = 0; i < 20000; ++i) {
      P.feed(one.data(), one.size() / 2);
      P.feed(one.data() + one.size() / 2, one.size() - one.size() / 2);
      maxb = std::max(maxb, P.buffered());
      while (P.next(m)) ++cnt;
    }
    CHECK(cnt == 20000 && maxb <= 2 * one.size());
  }
  {  // oversize
    uint8_t h[kTcpHeaderSize];
    writeTcpHeader(h, MsgType::VideoFrame, kMaxTcpPayload + 1);
    TcpFrameParser P;
    CHECK(!P.feed(h, 8));
    CHECK(P.error());
    TcpFrameParser::Message m;
    CHECK(!P.next(m));
    std::vector<uint8_t> ok = buildStream({{MsgType::Bye, {}}});
    CHECK(!P.feed(ok.data(), ok.size()) && P.error() && !P.next(m));  // permanent
    // exactly max is accepted (header only, waiting for payload)
    writeTcpHeader(h, MsgType::VideoFrame, kMaxTcpPayload);
    TcpFrameParser Q; CHECK(Q.feed(h, 8) && !Q.error() && !Q.next(m));
    // oversize behind a valid message is detected when reached
    std::vector<uint8_t> s = buildStream({{MsgType::Ping, {1, 2}}});
    s.insert(s.end(), h, h + 8);
    writeTcpHeader(h, MsgType::Ping, 0xFFFFFFFFu);
    s.insert(s.end() - 8, h, h);  // no-op, keep s
    TcpFrameParser R; R.feed(s.data(), s.size());
    CHECK(R.next(m) && m.payload.size() == 2);
    CHECK(!R.next(m));
    // header split across feeds with oversize
    writeTcpHeader(h, MsgType::Ping, 0xFFFFFFFFu);
    TcpFrameParser S; CHECK(S.feed(h, 5) && S.feed(h + 5, 3) == false && S.error());
  }
  {  // partial message stays pending
    auto s = buildStream({{MsgType::Ping, std::vector<uint8_t>(10, 1)}});
    TcpFrameParser P; TcpFrameParser::Message m;
    P.feed(s.data(), s.size() - 1);
    CHECK(!P.next(m) && P.buffered() == s.size() - 1);
    P.feed(s.data() + s.size() - 1, 1);
    CHECK(P.next(m) && m.type == MsgType::Ping && m.payload.size() == 10);
    CHECK(P.feed(nullptr, 0));
  }
}

// ---------------------------------------------------------- UDP header / packetize
struct Dgram { std::vector<uint8_t> bytes; UdpHeader h; };

static std::vector<Dgram> packetizeAll(const UdpHeader& base, const std::vector<uint8_t>& au,
                                       size_t maxp = kUdpMaxPayload, size_t* count = nullptr) {
  std::vector<Dgram> v;
  size_t c = packetize(base, au.data(), au.size(), [&](const uint8_t* d, size_t n) {
    Dgram g; g.bytes.assign(d, d + n);
    CHECK(readUdpHeader(d, n, g.h));
    v.push_back(std::move(g));
  }, maxp);
  if (count) *count = c;
  return v;
}

static void testUdp() {
  {  // header layout
    UdpHeader h; h.type = UdpType::Video; h.session_id = 0x11223344; h.frame_seq = 0x55667788;
    h.frag_idx = 0x0102; h.frag_count = 0x0304; h.frame_size = 0x0A0B0C0D;
    h.capture_ts_us = 0x1112131415161718ull; h.epoch = 0xAABB; h.flags = 3;
    uint8_t b[kUdpHeaderSize]; std::memset(b, 0xEE, sizeof(b));
    writeUdpHeader(h, b);
    const uint8_t exp[32] = {0x4F, 0x57, 1, 1, 0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0x02, 0x01,
                             0x04, 0x03, 0x0D, 0x0C, 0x0B, 0x0A, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13,
                             0x12, 0x11, 0xBB, 0xAA, 3, 0};
    CHECK(std::memcmp(b, exp, 32) == 0);
    UdpHeader o; CHECK(readUdpHeader(b, 32, o));
    CHECK(o.type == h.type && o.session_id == h.session_id && o.frame_seq == h.frame_seq &&
          o.frag_idx == h.frag_idx && o.frag_count == h.frag_count && o.frame_size == h.frame_size &&
          o.capture_ts_us == h.capture_ts_us && o.epoch == h.epoch && o.flags == h.flags);
    for (size_t n = 0; n < 32; ++n) CHECK(!readUdpHeader(b, n, o));
    uint8_t bad[32]; std::memcpy(bad, b, 32);
    bad[0] ^= 1; CHECK(!readUdpHeader(bad, 32, o));
    std::memcpy(bad, b, 32); bad[2] = 2; CHECK(!readUdpHeader(bad, 32, o));
    for (UdpType t : {UdpType::Punch, UdpType::PunchAck, UdpType::Fec}) {
      h.type = t; writeUdpHeader(h, b); CHECK(readUdpHeader(b, 32, o) && o.type == t);
    }
  }
  const size_t M = kUdpMaxPayload;
  const size_t sizes[] = {0, 1, M - 1, M, M + 1, 2 * M, 2 * M + 1, 5 * M, 5 * M - 1, 1u << 20};
  for (size_t sz : sizes) {
    auto au = pattern(sz, static_cast<uint32_t>(sz) + 1);
    UdpHeader base; base.session_id = 7; base.frame_seq = 99; base.capture_ts_us = 123456789;
    base.epoch = 5; base.flags = kFrameKey;
    size_t cnt = 0;
    auto ds = packetizeAll(base, au, M, &cnt);
    size_t expect = sz == 0 ? 1 : (sz + M - 1) / M;
    CHECK(cnt == expect && ds.size() == expect);
    size_t total = 0;
    std::vector<uint8_t> re;
    for (size_t i = 0; i < ds.size(); ++i) {
      const auto& d = ds[i];
      CHECK(d.bytes.size() <= kUdpMaxDatagram);
      CHECK(d.h.frag_idx == i && d.h.frag_count == expect && d.h.frame_size == sz);
      CHECK(d.h.capture_ts_us == 123456789 && d.h.epoch == 5 && d.h.flags == kFrameKey &&
            d.h.session_id == 7 && d.h.frame_seq == 99 && d.h.type == UdpType::Video);
      if (i + 1 < ds.size()) CHECK(d.bytes.size() == kUdpMaxDatagram);
      re.insert(re.end(), d.bytes.begin() + 32, d.bytes.end());
      total += d.bytes.size() - 32;
    }
    CHECK(total == sz && re == au);
  }
  {  // custom small max_payload, clamping, overflow
    auto au = pattern(1000);
    UdpHeader base;
    size_t c = 0; auto ds = packetizeAll(base, au, 100, &c);
    CHECK(c == 10 && ds.size() == 10 && ds[0].bytes.size() == 132);
    ds = packetizeAll(base, au, 100000, &c);  // clamped to kUdpMaxPayload
    CHECK(c == 1 && ds[0].bytes.size() == 1032);
    ds = packetizeAll(base, au, 0, &c);       // clamped to 1
    CHECK(c == 1000);
    std::vector<uint8_t> huge(70000);
    size_t calls = 0;
    size_t r = packetize(base, huge.data(), huge.size(), [&](const uint8_t*, size_t) { ++calls; }, 1);
    CHECK(r == 0 && calls == 0);
  }
}

// ---------------------------------------------------------- assembler
struct Rx {
  FrameAssembler fa;
  std::vector<FrameAssembler::Frame> out;
  explicit Rx(size_t mp = 4) : fa(mp) {}
  bool push(const Dgram& d) {
    FrameAssembler::Frame f;
    bool r = fa.push(d.h, d.bytes.data() + 32, d.bytes.size() - 32, f);
    if (r) out.push_back(std::move(f));
    return r;
  }
  void pushAll(const std::vector<Dgram>& v) { for (auto& d : v) push(d); }
};

static std::vector<Dgram> mkFrame(uint32_t seq, size_t sz, bool key, uint16_t epoch = 1,
                                  size_t maxp = kUdpMaxPayload) {
  UdpHeader b; b.session_id = 1; b.frame_seq = seq; b.epoch = epoch; b.flags = key ? kFrameKey : 0;
  b.capture_ts_us = 1000ull * seq;
  return packetizeAll(b, pattern(sz, seq), maxp);
}

static void testAssembler() {
  const size_t M = kUdpMaxPayload;
  {  // in order, mixed sizes, content check
    Rx rx;
    CHECK(rx.fa.needKeyframe());
    const size_t sizes[] = {0, 1, M, M + 1, 3 * M + 5, 100000};
    for (size_t i = 0; i < 6; ++i) rx.pushAll(mkFrame(10 + static_cast<uint32_t>(i), sizes[i], i == 0));
    // frame 10 is the keyframe (size 0); the rest are deltas that follow in order
    CHECK(rx.out.size() == 6);
    for (size_t i = 0; i < rx.out.size(); ++i) {
      CHECK(rx.out[i].frame_seq == 10 + i && rx.out[i].data == pattern(sizes[i], 10 + static_cast<uint32_t>(i)));
      CHECK(rx.out[i].capture_ts_us == 1000ull * (10 + i) && rx.out[i].epoch == 1);
    }
    CHECK(rx.fa.counters().frames_complete == 6 && rx.fa.counters().frames_lost == 0);
    CHECK(!rx.fa.needKeyframe());
  }
  {  // initially waits for keyframe: deltas dropped, then key accepted
    Rx rx;
    rx.pushAll(mkFrame(1, 3000, false));
    rx.pushAll(mkFrame(2, 3000, false));
    CHECK(rx.out.empty() && rx.fa.needKeyframe());
    rx.fa.clearNeedKeyframe();
    CHECK(!rx.fa.needKeyframe());
    rx.pushAll(mkFrame(3, 3000, false));  // still dropped, asks again
    CHECK(rx.out.empty() && rx.fa.needKeyframe());
    rx.pushAll(mkFrame(4, 3000, true));
    CHECK(rx.out.size() == 1 && rx.out[0].frame_seq == 4 && !rx.fa.needKeyframe());
    rx.pushAll(mkFrame(5, 3000, false));
    CHECK(rx.out.size() == 2);
  }
  {  // reversed and shuffled fragment order, including last fragment first
    Rx rx;
    auto f = mkFrame(1, 10 * M + 17, true);
    std::reverse(f.begin(), f.end());
    for (size_t i = 0; i < f.size(); ++i) CHECK(rx.push(f[i]) == (i + 1 == f.size()));
    CHECK(rx.out.size() == 1 && rx.out[0].data == pattern(10 * M + 17, 1));
    Rng rng(5);
    for (uint32_t s = 2; s < 60; ++s) {
      auto g = mkFrame(s, 1 + rng.below(20 * M), s % 5 == 0);
      for (size_t i = g.size(); i > 1; --i) std::swap(g[i - 1], g[rng.below(i)]);
      rx.pushAll(g);
    }
    // seq 1 was key; all later deltas are in order => all delivered
    CHECK(rx.out.size() == 59);
    for (size_t i = 0; i < rx.out.size(); ++i)
      CHECK(rx.out[i].data == pattern(rx.out[i].data.size(), rx.out[i].frame_seq));
    CHECK(rx.fa.counters().frames_lost == 0);
  }
  {  // duplicates ignored
    Rx rx;
    auto f = mkFrame(1, 4 * M, true);
    rx.push(f[0]); rx.push(f[0]); rx.push(f[1]); rx.push(f[1]);
    CHECK(rx.fa.counters().fragments_dropped == 2 && rx.fa.counters().fragments_received == 2);
    rx.push(f[2]);
    CHECK(rx.push(f[3]) && rx.out.size() == 1 && rx.out[0].data == pattern(4 * M, 1));
    // duplicate of the whole delivered frame = late
    for (auto& d : f) CHECK(!rx.push(d));
    CHECK(rx.out.size() == 1);
  }
  {  // single lost fragment: abandoned when newer completes, needKeyframe, deltas dropped until key
    Rx rx;
    rx.pushAll(mkFrame(1, 3 * M, true));
    CHECK(rx.out.size() == 1 && !rx.fa.needKeyframe());
    auto f2 = mkFrame(2, 3 * M, false);
    rx.push(f2[0]); rx.push(f2[2]);  // fragment 1 lost
    CHECK(rx.out.size() == 1);
    rx.pushAll(mkFrame(3, 3 * M, false));  // completes but frame 2 missing -> loss, dropped
    CHECK(rx.out.size() == 1 && rx.fa.needKeyframe());
    CHECK(rx.fa.counters().frames_lost == 1);
    rx.fa.clearNeedKeyframe();
    rx.pushAll(mkFrame(4, 2 * M, false));
    CHECK(rx.out.size() == 1);  // deltas still dropped
    // late fragment of abandoned frame 2 is discarded
    CHECK(!rx.push(f2[1]));
    rx.pushAll(mkFrame(5, 2 * M, true));
    CHECK(rx.out.size() == 2 && rx.out[1].frame_seq == 5 && !rx.fa.needKeyframe());
    rx.pushAll(mkFrame(6, 100, false));
    CHECK(rx.out.size() == 3);
    CHECK(rx.fa.counters().frames_lost == 1);
  }
  {  // wholly missing frame (seq gap, nothing received)
    Rx rx;
    rx.pushAll(mkFrame(1, 500, true));
    rx.pushAll(mkFrame(4, 500, false));
    CHECK(rx.out.size() == 1 && rx.fa.counters().frames_lost == 2 && rx.fa.needKeyframe());
    rx.pushAll(mkFrame(5, 500, true));
    CHECK(rx.out.size() == 2);
  }
  {  // lost frame followed directly by a keyframe: delivered, loss counted, no more need
    Rx rx;
    rx.pushAll(mkFrame(1, 500, true));
    rx.pushAll(mkFrame(3, 500, true));
    CHECK(rx.out.size() == 2 && rx.fa.counters().frames_lost == 1 && !rx.fa.needKeyframe());
  }
  {  // seq wraparound
    Rx rx;
    uint32_t s = 0xFFFFFFFDu;
    rx.pushAll(mkFrame(s, 2000, true));
    for (int i = 1; i < 8; ++i) rx.pushAll(mkFrame(s + static_cast<uint32_t>(i), 2000 + static_cast<size_t>(i), false));
    CHECK(rx.out.size() == 8 && rx.fa.counters().frames_lost == 0 && !rx.fa.needKeyframe());
    CHECK(rx.out[3].frame_seq == 0 && rx.out[7].frame_seq == 4);
    // late fragment across the wrap
    auto old = mkFrame(0xFFFFFFFFu, 2000, false);
    CHECK(!rx.push(old[0]));
    // loss across wrap
    rx.pushAll(mkFrame(6, 100, false));
    CHECK(rx.fa.needKeyframe() && rx.fa.counters().frames_lost == 1);
    // reordering across wrap: frame 0 before 0xFFFFFFFF inside pending window
    Rx r2;
    r2.pushAll(mkFrame(0xFFFFFFFEu, 100, true));
    auto a = mkFrame(0xFFFFFFFFu, 2 * M, false), b = mkFrame(0, 100, true);
    r2.push(a[0]); r2.pushAll(b);  // b complete first but a older and incomplete -> a abandoned
    CHECK(r2.out.size() == 2 && r2.out[1].frame_seq == 0);
    CHECK(!r2.fa.needKeyframe() && r2.fa.counters().frames_lost == 1);
  }
  {  // epoch change mid stream resets and waits for keyframe
    Rx rx;
    rx.pushAll(mkFrame(100, 500, true, 1));
    rx.pushAll(mkFrame(101, 500, false, 1));
    CHECK(rx.out.size() == 2);
    rx.pushAll(mkFrame(1, 500, false, 2));  // new epoch, seq restarts, delta
    CHECK(rx.out.size() == 2 && rx.fa.needKeyframe());
    rx.pushAll(mkFrame(2, 500, true, 2));
    CHECK(rx.out.size() == 3 && rx.out[2].epoch == 2 && rx.out[2].frame_seq == 2);
    rx.pushAll(mkFrame(3, 500, false, 2));
    CHECK(rx.out.size() == 4);
    CHECK(rx.fa.counters().frames_lost == 0);
    // partial frame from old epoch is dropped at switch
    Rx r2;
    r2.pushAll(mkFrame(5, 500, true, 1));
    auto part = mkFrame(6, 3 * M, false, 1);
    r2.push(part[0]);
    r2.pushAll(mkFrame(1, 500, true, 9));
    CHECK(r2.out.size() == 2 && r2.out[1].epoch == 9);
    r2.push(part[1]); r2.push(part[2]);
    CHECK(r2.out.size() == 2);  // stragglers from the old epoch are dropped...
    r2.pushAll(mkFrame(2, 500, false, 9));
    CHECK(r2.out.size() == 3 && r2.out[2].epoch == 9 && !r2.fa.needKeyframe());  // ...without resetting the new one
    // epoch wraparound: 0xFFFF -> 0 is a newer epoch, 0 -> 0xFFFF is a straggler
    Rx r3;
    r3.pushAll(mkFrame(1, 500, true, 0xFFFF));
    r3.pushAll(mkFrame(1, 500, true, 0));
    CHECK(r3.out.size() == 2 && r3.out[1].epoch == 0);
    r3.pushAll(mkFrame(2, 500, true, 0xFFFF));
    r3.pushAll(mkFrame(2, 500, false, 0));
    CHECK(r3.out.size() == 3 && r3.out[2].epoch == 0 && r3.out[2].frame_seq == 2);
  }
  {  // max_pending eviction
    Rx rx(3);
    rx.pushAll(mkFrame(1, 100, true));
    auto f2 = mkFrame(2, 2 * M, false), f3 = mkFrame(3, 2 * M, false), f4 = mkFrame(4, 2 * M, false),
         f5 = mkFrame(5, 2 * M, false);
    rx.push(f2[0]); rx.push(f3[0]); rx.push(f4[0]);
    CHECK(!rx.fa.needKeyframe() || rx.fa.needKeyframe());  // no assertion on pre-state
    rx.fa.clearNeedKeyframe();
    CHECK(!rx.fa.needKeyframe());
    rx.push(f5[0]);  // 4th in flight => evict frame 2
    CHECK(rx.fa.needKeyframe());
    rx.push(f2[1]);  // late fragment of evicted frame: ignored
    rx.push(f3[1]); rx.push(f4[1]);
    CHECK(rx.out.size() == 1);  // 3,4 complete but are deltas after loss -> dropped
    rx.fa.clearNeedKeyframe();
    rx.pushAll(mkFrame(6, 100, true));
    CHECK(rx.out.size() == 2 && rx.out[1].frame_seq == 6);
    // an older newcomer while full is dropped, pending are preserved
    Rx r2(2);
    r2.pushAll(mkFrame(1, 100, true));
    auto g5 = mkFrame(5, 2 * M, false), g6 = mkFrame(6, 2 * M, false), g3 = mkFrame(3, 2 * M, false);
    r2.push(g5[0]); r2.push(g6[0]);
    uint64_t d0 = r2.fa.counters().fragments_dropped;
    r2.push(g3[0]);
    CHECK(r2.fa.counters().fragments_dropped == d0 + 1);
    // max_pending = 0 behaves like 1 and does not crash
    Rx r3(0); r3.pushAll(mkFrame(1, 3 * M, true)); CHECK(r3.out.size() == 1);
  }
  {  // invalid headers rejected
    Rx rx;
    auto good = mkFrame(1, 3 * M, true);
    // true if the fragment was rejected (counted in fragments_dropped)
    auto tryPush = [&](Dgram d) {
      uint64_t before = rx.fa.counters().fragments_dropped;
      rx.push(d);
      return rx.fa.counters().fragments_dropped == before + 1;
    };
    Dgram d = good[0]; d.h.frag_count = 0; CHECK(tryPush(d));
    d = good[0]; d.h.frag_idx = 3; CHECK(tryPush(d));          // idx >= count
    d = good[0]; d.h.frame_size = kMaxTcpPayload + 1; CHECK(tryPush(d));
    d = good[0]; d.h.frag_count = 2; CHECK(tryPush(d));         // count inconsistent with size/payload
    d = good[0]; d.h.frag_count = 5; CHECK(tryPush(d));
    d = good[0]; d.h.frame_size = 10; CHECK(tryPush(d));        // size smaller than payload
    d = good[0]; d.h.type = UdpType::Punch; CHECK(tryPush(d));
    d = good[0]; d.h.frag_count = 9000; d.h.frame_size = 100; CHECK(tryPush(d));  // count > size
    // single-fragment frame with wrong len
    auto one = mkFrame(2, 100, true);
    d = one[0]; d.h.frame_size = 99; CHECK(tryPush(d));
    d = one[0]; d.h.frame_size = 101; CHECK(tryPush(d));
    CHECK(rx.out.empty());
    // inconsistent fragment within an in-flight frame
    CHECK(!rx.push(good[0]) && rx.fa.counters().fragments_received == 1);
    d = good[1]; d.h.frame_size += 1; CHECK(tryPush(d));
    d = good[1]; d.h.frag_count = 4; CHECK(tryPush(d));
    d = good[1]; d.bytes.resize(d.bytes.size() - 1); CHECK(tryPush(d));  // non-last with different len
    d = good[2]; d.bytes.resize(d.bytes.size() - 1); CHECK(tryPush(d));  // last whose offset mismatches
    CHECK(!rx.push(good[1]));
    CHECK(rx.push(good[2]) && rx.out.size() == 1 && rx.out[0].data == pattern(3 * M, 1));
    // last fragment first with bad len, then non-last establishing inconsistent P
    Rx r2;
    auto f = mkFrame(1, 3 * M + 10, true);
    CHECK(!r2.push(f[3]));
    d = f[0]; d.bytes.resize(32 + M - 5); CHECK(!r2.push(d));   // implied P inconsistent with last
    CHECK(!r2.push(f[0]) && !r2.push(f[1]) && r2.push(f[2]) && r2.out.size() == 1);
    CHECK(r2.fa.counters().fragments_dropped >= 1);
    // zero-length non-last fragment
    Rx r3;
    d = f[0]; d.bytes.resize(32); CHECK(!r3.push(d));
  }
  {  // stale fragments after delivery + reset()
    Rx rx;
    auto f1 = mkFrame(1, 2 * M, true), f2 = mkFrame(2, 2 * M, false);
    rx.pushAll(f2);  // delta first -> dropped (waiting)
    rx.pushAll(f1);  // older than floor now: late, ignored
    CHECK(rx.out.empty());
    rx.pushAll(mkFrame(3, 10, true));
    CHECK(rx.out.size() == 1);
    for (auto& d : f2) CHECK(!rx.push(d));
    rx.fa.reset();
    CHECK(rx.fa.needKeyframe() && rx.fa.counters().frames_complete == 0);
    rx.pushAll(mkFrame(1, 10, true));
    CHECK(rx.out.size() == 2);
  }
  {  // output buffer reuse via swap does not corrupt later frames
    FrameAssembler fa;
    FrameAssembler::Frame f;
    for (uint32_t s = 1; s < 20; ++s) {
      auto d = mkFrame(s, 500 + s * 777, s == 1);
      bool got = false;
      for (auto& g : d) got = fa.push(g.h, g.bytes.data() + 32, g.bytes.size() - 32, f) || got;
      CHECK(got && f.data == pattern(500 + s * 777, s));
    }
  }
}

// ---------------------------------------------------------- fuzz
template <class T>
static void fuzzDecode(Rng& rng, const std::vector<uint8_t>& seed) {
  T t;
  std::vector<uint8_t> v = seed;
  // random bytes
  std::vector<uint8_t> r(rng.below(200));
  rng.fill(r);
  (void)decode(r.data(), r.size(), t);
  // mutated valid encodings
  if (!v.empty()) {
    size_t flips = 1 + rng.below(4);
    for (size_t i = 0; i < flips; ++i) v[rng.below(v.size())] = static_cast<uint8_t>(rng.u32());
    (void)decode(v.data(), v.size(), t);
    v.resize(rng.below(v.size() + 1));
    (void)decode(v.data(), v.size(), t);
  }
}

static void testFuzz() {
  Rng rng(0xC0FFEE);
  Capabilities caps = makeCaps(3, 6, 4);
  auto sCaps = encode(caps);
  Hello hh; hh.client_name = kUnicode; auto sHello = encode(hh);
  HelloAck ha; ha.device_name = "dev"; auto sAck = encode(ha);
  SetParam sp; sp.value = ParamValue::Real(1); auto sSp = encode(sp);
  ParamState ps; auto sPs = encode(ps);
  Command cm; cm.text = "t"; auto sCm = encode(cm);
  VideoFormat vf; vf.csd = pattern(40); auto sVf = encode(vf);
  Ping pi; auto sPi = encode(pi);
  Pong po; auto sPo = encode(po);
  Stats st; auto sSt = encode(st);
  LogMsg lm; lm.text = "log"; auto sLm = encode(lm);
  ReceiverReport rr; auto sRr = encode(rr);
  DiscoveryReply dr; dr.device_name = "d"; auto sDr = encode(dr);

  const int N = 20000;
  for (int i = 0; i < N; ++i) {
    fuzzDecode<Capabilities>(rng, sCaps); fuzzDecode<Hello>(rng, sHello);
    fuzzDecode<HelloAck>(rng, sAck); fuzzDecode<SetParam>(rng, sSp);
    fuzzDecode<ParamState>(rng, sPs); fuzzDecode<Command>(rng, sCm);
    fuzzDecode<VideoFormat>(rng, sVf); fuzzDecode<Ping>(rng, sPi);
    fuzzDecode<Pong>(rng, sPo); fuzzDecode<Stats>(rng, sSt);
    fuzzDecode<LogMsg>(rng, sLm); fuzzDecode<ReceiverReport>(rng, sRr);
    fuzzDecode<DiscoveryReply>(rng, sDr);
    std::vector<uint8_t> r(rng.below(64)); rng.fill(r);
    VideoFrameHeader vh; const uint8_t* au; size_t aun;
    (void)decodeVideoFrame(r.data(), r.size(), vh, au, aun);
    UdpHeader uh; (void)readUdpHeader(r.data(), r.size(), uh);
    (void)isDiscoveryRequest(r.data(), r.size());
  }
  CHECK(true);

  // TcpFrameParser fuzz
  for (int i = 0; i < 20000; ++i) {
    TcpFrameParser P; TcpFrameParser::Message m;
    for (int k = 0; k < 4; ++k) {
      std::vector<uint8_t> r(rng.below(100)); rng.fill(r);
      if (rng.below(2) && r.size() >= 8) { r[6] &= 0x01; r[7] = 0; }  // keep lengths plausible
      P.feed(r.data(), r.size());
      while (P.next(m)) {}
    }
  }
  CHECK(true);

  // FrameAssembler fuzz: random headers (small ranges so things collide) + random payload
  for (int i = 0; i < 4; ++i) {
    FrameAssembler fa(1 + static_cast<size_t>(i));
    FrameAssembler::Frame out;
    std::vector<uint8_t> pay(kUdpMaxPayload);
    rng.fill(pay);
    for (int k = 0; k < 15000; ++k) {
      UdpHeader h;
      h.type = rng.below(50) ? UdpType::Video : static_cast<UdpType>(rng.below(7));
      h.frame_seq = 0xFFFFFFF0u + static_cast<uint32_t>(rng.below(40));
      h.epoch = static_cast<uint16_t>(rng.below(rng.below(100) ? 1 : 3));
      h.frag_count = static_cast<uint16_t>(rng.below(rng.below(20) ? 6 : 70000));
      h.frag_idx = static_cast<uint16_t>(rng.below(8));
      h.frame_size = rng.below(10) ? static_cast<uint32_t>(rng.below(6 * kUdpMaxPayload))
                                   : rng.u32();
      h.flags = static_cast<uint8_t>(rng.below(2));
      size_t len = rng.below(4) ? kUdpMaxPayload : rng.below(kUdpMaxPayload + 1);
      (void)fa.push(h, pay.data(), len, out);
      if (rng.below(100) == 0) fa.clearNeedKeyframe();
      if (rng.below(5000) == 0) fa.reset();
    }
  }
  // Structured fuzz: real packetized frames with random loss/dup/reorder, verify integrity.
  for (int iter = 0; iter < 30; ++iter) {
    Rx rx(1 + rng.below(5));
    std::vector<Dgram> all;
    uint32_t seq = rng.u32();
    std::vector<std::vector<uint8_t>> truth;
    std::vector<uint32_t> seqs;
    for (int f = 0; f < 60; ++f) {
      size_t sz = rng.below(8 * kUdpMaxPayload);
      UdpHeader b; b.frame_seq = seq; b.epoch = 3; b.flags = (f % 10 == 0) ? kFrameKey : 0;
      auto au = pattern(sz, seq);
      auto ds = packetizeAll(b, au);
      for (auto& d : ds) {
        if (rng.below(12) == 0) continue;                 // loss
        all.push_back(d);
        if (rng.below(15) == 0) all.push_back(d);         // dup
      }
      ++seq;
    }
    for (size_t i = 0; i + 1 < all.size(); ++i)           // local reordering
      if (rng.below(6) == 0) std::swap(all[i], all[i + 1]);
    for (auto& d : all) rx.push(d);
    uint32_t prev = 0; bool first = true;
    for (auto& f : rx.out) {
      CHECK(f.data == pattern(f.data.size(), f.frame_seq));  // never corrupted
      if (!first) CHECK(seqNewer(f.frame_seq, prev));        // strictly increasing
      first = false; prev = f.frame_seq;
    }
  }
}

// ---------------------------------------------------------- end-to-end smoke
static void testEndToEnd() {
  // Hello -> TCP stream -> parser -> decode
  Hello h; h.client_name = "pc";
  std::vector<uint8_t> s;
  appendTcpMessage(s, MsgType::Hello, encode(h));
  Capabilities c = makeCaps(2, 4, 3);
  appendTcpMessage(s, MsgType::Capabilities, encode(c));
  VideoFrameHeader vh; vh.frame_seq = 3; std::vector<uint8_t> vf;
  auto au = pattern(5000);
  encodeVideoFrame(vh, au.data(), au.size(), vf);
  appendTcpMessage(s, MsgType::VideoFrame, vf);
  TcpFrameParser P; P.feed(s.data(), s.size());
  TcpFrameParser::Message m;
  Hello oh; Capabilities oc;
  CHECK(P.next(m) && m.type == MsgType::Hello && decode(m.payload.data(), m.payload.size(), oh) && oh.client_name == "pc");
  CHECK(P.next(m) && m.type == MsgType::Capabilities && decode(m.payload.data(), m.payload.size(), oc) && eq(c, oc));
  CHECK(P.next(m) && m.type == MsgType::VideoFrame);
  VideoFrameHeader oh2; const uint8_t* p; size_t n;
  CHECK(decodeVideoFrame(m.payload.data(), m.payload.size(), oh2, p, n) && n == 5000 && oh2.frame_seq == 3);
  CHECK(!P.next(m));
  CHECK(seqNewer(1, 0) && !seqNewer(0, 1) && seqNewer(0, 0xFFFFFFFFu) && !seqNewer(5, 5));
}

// ---------------------------------------------------------- FEC
static void testFecLayout() {
  CHECK(fecLayout(0, 5).nblocks == 0);
  CHECK(fecLayout(5, 0).nblocks == 0);
  CHECK(fecLayout(65535, 255).nblocks == 0);   // would need > 8 blocks
  CHECK(fecLayout(255, 1).nblocks == 0);       // 255 + 1 > 255 with a single block
  CHECK(fecLayout(254, 1).nblocks == 1);
  CHECK(fecLayout(300, 20).nblocks == 2);
  CHECK(fecLayout(1, 255).nblocks == 0);       // 1 data block can hold at most 254 parity
  CHECK(fecLayout(1, 254).nblocks == 1 && fecLayout(2, 255).nblocks == 2);
  int checked = 0;
  for (unsigned count : {1u, 2u, 3u, 10u, 100u, 147u, 211u, 212u, 213u, 254u, 255u, 256u, 400u, 1000u, 1500u, 2000u}) {
    for (unsigned m : {1u, 2u, 5u, 20u, 30u, 64u, 100u, 128u, 200u, 255u}) {
      FecLayout L = fecLayout(static_cast<uint16_t>(count), static_cast<uint8_t>(m));
      if (L.nblocks == 0) continue;
      ++checked;
      CHECK(L.nblocks <= kFecMaxBlocks);
      unsigned df = 0, pf = 0;
      for (unsigned b = 0; b < L.nblocks; ++b) {
        const FecBlock& B = L.block[b];
        CHECK(B.data_first == df && B.parity_first == pf);
        CHECK(B.data_count >= 1 && B.parity_count >= 1 && B.data_count + B.parity_count <= 255);
        df += B.data_count; pf += B.parity_count;
      }
      CHECK(df == count && pf == m);
      // smallest block count that works
      if (L.nblocks > 1) {
        unsigned n = L.nblocks - 1;
        CHECK((count + n - 1) / n + (m + n - 1) / n > 255);
      }
    }
  }
  CHECK(checked > 100);
  CHECK(fecParityCount(10, 20) == 2);
  CHECK(fecParityCount(100, 20) == 20);
  CHECK(fecParityCount(147, 20) == 30);
  CHECK(fecParityCount(1, 50) == 2);
  CHECK(fecParityCount(5, 0) == 0 && fecParityCount(5, -1) == 0 && fecParityCount(0, 20) == 0);
  CHECK(fecParityCount(1000, 100) == 255);
  CHECK(fecShardSize(0) == 0 && fecShardSize(100) == 100 && fecShardSize(kUdpMaxPayload) == kUdpMaxPayload &&
        fecShardSize(5 * kUdpMaxPayload) == kUdpMaxPayload && fecShardSize(5000, 500) == 500);
}

struct FecSet {
  std::vector<Dgram> data, par;
  std::vector<uint8_t> au;
  FecLayout lay;
  uint16_t count = 0;
  uint8_t M = 0;
};

static FecSet makeFec(uint32_t seq, size_t sz, uint8_t M, bool key, uint16_t epoch = 1, size_t maxp = kUdpMaxPayload) {
  FecSet f;
  UdpHeader b; b.session_id = 1; b.frame_seq = seq; b.epoch = epoch; b.flags = key ? kFrameKey : 0;
  b.capture_ts_us = 1000ull * seq;
  f.au = pattern(sz, seq);
  f.data = packetizeAll(b, f.au, maxp);
  f.count = static_cast<uint16_t>(f.data.size());
  f.M = M;
  f.lay = fecLayout(f.count, M);
  size_t n = packetizeFec(b, f.au.data(), f.au.size(), M, [&](const uint8_t* d, size_t len) {
    Dgram g; g.bytes.assign(d, d + len);
    CHECK(readUdpHeader(d, len, g.h));
    f.par.push_back(std::move(g));
  }, maxp);
  CHECK(n == f.par.size());
  for (size_t p = 0; p < f.par.size(); ++p) {
    const UdpHeader& h = f.par[p].h;
    CHECK(h.type == UdpType::Fec && h.frag_idx == p && h.frag_count == f.count && h.fec_total == M &&
          h.frame_size == sz && h.frame_seq == seq && h.epoch == epoch);
    CHECK(f.par[p].bytes.size() <= kUdpMaxDatagram);
    CHECK(f.par[p].bytes.size() - 32 == fecShardSize(sz, maxp));
  }
  return f;
}

// True iff every block still has >= data_count shards.
static bool recoverable(const FecSet& f, const std::vector<char>& dgot, const std::vector<char>& pgot) {
  for (unsigned b = 0; b < f.lay.nblocks; ++b) {
    const FecBlock& B = f.lay.block[b];
    unsigned have = 0;
    for (unsigned i = 0; i < B.data_count; ++i) have += dgot[B.data_first + i] ? 1 : 0;
    for (unsigned j = 0; j < B.parity_count; ++j) have += pgot[B.parity_first + j] ? 1 : 0;
    if (have < B.data_count) return false;
  }
  return true;
}

// Sends the surviving datagrams in `order` (indices: <count data, >=count parity).
static bool fecTrial(Rx& rx, const FecSet& f, const std::vector<char>& dgot, const std::vector<char>& pgot,
                     Rng& rng, bool shuffle) {
  std::vector<const Dgram*> v;
  for (size_t i = 0; i < f.data.size(); ++i) if (dgot[i]) v.push_back(&f.data[i]);
  for (size_t j = 0; j < f.par.size(); ++j) if (pgot[j]) v.push_back(&f.par[j]);
  if (shuffle)
    for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[rng.below(i)]);
  size_t before = rx.out.size();
  for (auto* d : v) rx.push(*d);
  return rx.out.size() == before + 1;
}

static void fecRandomLossRun(size_t sz, uint8_t M, Rng& rng, int trials, size_t maxp = kUdpMaxPayload) {
  for (int t = 0; t < trials; ++t) {
    FecSet f = makeFec(1, sz, M, true, 1, maxp);
    CHECK(f.lay.nblocks != 0 || M == 0);
    if (f.lay.nblocks == 0) return;
    std::vector<char> dg(f.data.size(), 1), pg(f.par.size(), 1);
    int mode = t % 3;
    double pct = (rng.below(31)) / 100.0;
    if (mode == 0) {  // random loss 0..30 %
      for (auto& x : dg) if (rng.below(1000) < pct * 1000) x = 0;
      for (auto& x : pg) if (rng.below(1000) < pct * 1000) x = 0;
    } else if (mode == 1) {  // one burst across the data+parity sequence
      size_t total = dg.size() + pg.size();
      size_t len = rng.below(std::max<size_t>(1, static_cast<size_t>(total * 0.3)) + 1);
      size_t start = rng.below(total);
      for (size_t i = 0; i < len; ++i) {
        size_t k = (start + i) % total;
        if (k < dg.size()) dg[k] = 0; else pg[k - dg.size()] = 0;
      }
    } else {  // loss right around the recoverability edge: drop exactly parity_count in each block
      for (unsigned b = 0; b < f.lay.nblocks; ++b) {
        const FecBlock& B = f.lay.block[b];
        unsigned drop = B.parity_count + (rng.below(3) == 0 ? 1u : 0u);
        for (unsigned i = 0; i < drop && i < B.data_count; ++i) {
          size_t pick = B.data_first + rng.below(B.data_count);
          dg[pick] = 0;
        }
      }
    }
    bool want = recoverable(f, dg, pg);
    Rx rx;
    bool got = fecTrial(rx, f, dg, pg, rng, t % 2 == 1);
    CHECK(got == want);
    if (got) {
      CHECK(rx.out[0].data == f.au);
      CHECK(rx.fa.counters().frames_complete == 1);
    }
  }
}

static void testFec() {
  Rng rng(777);
  // layouts + parity wire format
  {
    FecSet f = makeFec(5, 10 * kUdpMaxPayload + 33, 4, true);
    CHECK(f.data.size() == 11 && f.par.size() == 4);
    for (auto& p : f.par) CHECK(p.bytes[31] == 4);  // reserved byte = M
    uint8_t buf[32];
    UdpHeader h; h.fec_total = 200; writeUdpHeader(h, buf);
    CHECK(buf[31] == 200);
    UdpHeader o; CHECK(readUdpHeader(buf, 32, o) && o.fec_total == 200);
    // au_len 0 or M=0 or impossible: no parity
    std::vector<uint8_t> par;
    CHECK(!fecEncode(f.au.data(), 0, 4, par) && par.empty());
    CHECK(!fecEncode(f.au.data(), 100, 0, par));
    size_t n = packetizeFec(UdpHeader(), f.au.data(), 0, 4, [&](const uint8_t*, size_t) {});
    CHECK(n == 0);
  }
  // sizes x parity counts x loss patterns, strict recoverability equivalence
  const size_t M0 = kUdpMaxPayload;
  struct Case { size_t sz; uint8_t m; };
  const Case cases[] = {
      {1, 2}, {1, 1}, {100, 2}, {M0 - 1, 2}, {M0, 2}, {M0 + 1, 2}, {2 * M0 + 5, 2}, {3 * M0, 3},
      {10 * M0 + 1, 2}, {25 * M0 + 777, 5}, {147 * M0 + 3, 30}, {211 * M0, 43}, {212 * M0 + 1, 43},
      {300 * M0 + 77, 60}, {600 * M0 + 5, 120}, {1000 * M0 + 1, 200}, {3 * M0 + 9, 255}, {1, 254},
      {200 * M0 + 1, 255}, {50 * M0 + 2, 50}, {7 * M0, 7}};
  for (const auto& c : cases) fecRandomLossRun(c.sz, c.m, rng, 18);
  // custom max_payload (shard size) and a tiny one
  fecRandomLossRun(100000, 20, rng, 12, 1000);
  fecRandomLossRun(5000, 8, rng, 12, 64);
  // lose ALL data fragments, recover purely from parity (M >= k)
  for (size_t k : {1u, 2u, 7u, 40u, 100u}) {
    size_t sz = k * M0 - (k > 1 ? 11 : 1000);
    FecSet f = makeFec(1, sz, static_cast<uint8_t>(k), true);
    if (f.lay.nblocks == 0) continue;
    std::vector<char> dg(f.data.size(), 0), pg(f.par.size(), 1);
    Rx rx; CHECK(fecTrial(rx, f, dg, pg, rng, true) && rx.out[0].data == f.au);
    CHECK(rx.fa.counters().fragments_recovered == f.count && rx.fa.counters().frames_recovered == 1);
  }
  // exactly one lost fragment everywhere (each index), m=2
  {
    FecSet f = makeFec(1, 9 * M0 + 100, 2, true);
    for (size_t lost = 0; lost < f.data.size(); ++lost) {
      Rx rx; std::vector<char> dg(f.data.size(), 1), pg(f.par.size(), 1);
      dg[lost] = 0;
      CHECK(fecTrial(rx, f, dg, pg, rng, false) && rx.out[0].data == f.au);
      CHECK(rx.fa.counters().fragments_recovered == 1);
    }
  }
  // counters: no loss -> parity is unused (arrives after completion)
  {
    FecSet f = makeFec(1, 5 * M0, 2, true);
    Rx rx; rx.pushAll(f.data);
    CHECK(rx.out.size() == 1);
    rx.pushAll(f.par);
    const auto& c = rx.fa.counters();
    CHECK(c.fec_unused == 2 && c.fec_received == 0 && c.frames_recovered == 0 && c.fragments_dropped == 0);
    // duplicates of parity inside an incomplete frame
    FecSet g = makeFec(2, 5 * M0, 3, false);
    rx.push(g.par[0]); rx.push(g.par[0]);
    CHECK(rx.fa.counters().fec_received == 1 && rx.fa.counters().fec_unused == 3);
  }
  // FEC-recovered stream: no loss reported, no keyframe request
  {
    Rx rx;
    for (uint32_t s = 1; s <= 40; ++s) {
      FecSet f = makeFec(s, (3 + s % 9) * M0 + s, 2, s == 1);
      std::vector<char> dg(f.data.size(), 1), pg(f.par.size(), 1);
      dg[rng.below(dg.size())] = 0;
      CHECK(fecTrial(rx, f, dg, pg, rng, s % 2 == 0));
    }
    CHECK(rx.out.size() == 40 && rx.fa.counters().frames_lost == 0 && !rx.fa.needKeyframe());
    CHECK(rx.fa.counters().frames_recovered == 40);
    for (auto& fr : rx.out) CHECK(fr.data == pattern(fr.data.size(), fr.frame_seq));
  }
  // unrecoverable loss -> normal loss path (abandon, needKeyframe, deltas dropped until key)
  {
    Rx rx;
    FecSet f1 = makeFec(1, 6 * M0, 2, true);
    CHECK(fecTrial(rx, f1, std::vector<char>(f1.data.size(), 1), std::vector<char>(2, 1), rng, false));
    FecSet f2 = makeFec(2, 6 * M0, 2, false);
    std::vector<char> dg(f2.data.size(), 1);
    dg[0] = dg[3] = dg[5] = 0;  // 3 lost > 2 parity
    CHECK(!fecTrial(rx, f2, dg, std::vector<char>(2, 1), rng, false));
    FecSet f3 = makeFec(3, 6 * M0, 2, false);
    CHECK(!fecTrial(rx, f3, std::vector<char>(f3.data.size(), 1), std::vector<char>(2, 1), rng, false));
    CHECK(rx.fa.needKeyframe() && rx.fa.counters().frames_lost == 1);
    FecSet f4 = makeFec(4, 6 * M0, 2, true);
    std::vector<char> d4(f4.data.size(), 1); d4[2] = 0;
    CHECK(fecTrial(rx, f4, d4, std::vector<char>(2, 1), rng, true) && !rx.fa.needKeyframe());
    // late parity of the abandoned frame 2 is just "unused"
    uint64_t un = rx.fa.counters().fec_unused;
    rx.pushAll(f2.par);
    CHECK(rx.fa.counters().fec_unused == un + 2);
  }
  // parity arrives first, epoch change with parity, max_pending eviction with parity-created slots
  {
    Rx rx;
    FecSet f = makeFec(10, 4 * M0, 2, true, 1);
    rx.push(f.par[0]); rx.push(f.par[1]);
    std::vector<char> dg(f.data.size(), 1); dg[1] = dg[2] = 0;
    for (size_t i = 0; i < f.data.size(); ++i) if (dg[i]) rx.push(f.data[i]);
    CHECK(rx.out.size() == 1 && rx.out[0].data == f.au);
    FecSet g = makeFec(1, 3 * M0, 2, true, 7);  // new epoch
    rx.push(g.par[0]);
    std::vector<char> d2(g.data.size(), 1); d2[0] = 0;
    for (size_t i = 0; i < g.data.size(); ++i) if (d2[i]) rx.push(g.data[i]);
    CHECK(rx.out.size() == 2 && rx.out[1].epoch == 7 && rx.out[1].data == g.au);
    // eviction: parity-only frames occupy slots
    Rx r2(2);
    r2.pushAll(makeFec(1, 100, 2, true).data);
    FecSet a = makeFec(2, 3 * M0, 2, false), b = makeFec(3, 3 * M0, 2, false), c = makeFec(4, 3 * M0, 2, false);
    r2.push(a.par[0]); r2.push(b.par[0]); r2.push(c.par[0]);  // evicts frame 2
    CHECK(r2.fa.needKeyframe());
    r2.push(a.par[1]);
    CHECK(r2.fa.counters().fec_unused >= 1);
  }
  // invalid Fec datagrams
  {
    Rx rx;
    FecSet f = makeFec(1, 4 * M0, 2, true);
    auto bad = [&](Dgram d) {
      uint64_t before = rx.fa.counters().fragments_dropped;
      rx.push(d);
      return rx.fa.counters().fragments_dropped == before + 1;
    };
    Dgram d = f.par[0]; d.h.fec_total = 0; CHECK(bad(d));
    d = f.par[0]; d.h.frag_idx = 2; CHECK(bad(d));                       // idx >= M
    d = f.par[0]; d.h.frame_size = 0; CHECK(bad(d));
    d = f.par[0]; d.bytes.resize(32); CHECK(bad(d));                     // empty shard
    d = f.par[0]; d.h.frag_count = 0; CHECK(bad(d));
    d = f.par[0]; d.h.fec_total = 255; d.h.frag_count = 300; CHECK(bad(d));   // layout impossible? (300+255)
    d = f.par[0]; d.bytes.resize(32 + 100); CHECK(bad(d));               // shard size inconsistent with size/count
    d = f.par[0]; d.h.frame_size = 9 * M0; CHECK(bad(d));
    d = f.par[0]; d.h.frame_size = kMaxTcpPayload + 1; CHECK(bad(d));
    // consistent state, then mismatching M / S on later parity of the same frame
    rx.push(f.par[0]);
    CHECK(rx.fa.counters().fec_received == 1);
    d = f.par[1]; d.h.fec_total = 3; CHECK(bad(d));
    d = f.par[1]; d.bytes.resize(d.bytes.size() - 1); CHECK(bad(d));
    // data fragments inconsistent with learned shard size
    d = f.data[0]; d.bytes.resize(d.bytes.size() - 1); CHECK(bad(d));
    // single fragment: shard smaller than frame is invalid
    FecSet one = makeFec(2, 100, 2, false);
    d = one.par[0]; d.bytes.resize(32 + 99); CHECK(bad(d));
    // then the good ones still work
    for (auto& g : f.data) rx.push(g);
    CHECK(rx.out.size() == 1 && rx.out[0].data == f.au);
  }
  // small random fuzz mixing real Fec datagrams, with integrity verification
  {
    for (int iter = 0; iter < 40; ++iter) {
      Rx rx(1 + rng.below(4));
      std::vector<Dgram> all;
      uint32_t seq = rng.u32();
      for (int fr = 0; fr < 25; ++fr, ++seq) {
        size_t sz = 1 + rng.below(30 * kUdpMaxPayload);
        FecSet f = makeFec(seq, sz, static_cast<uint8_t>(2 + rng.below(8)), fr % 7 == 0, 4);
        for (auto& d : f.data) if (rng.below(8)) all.push_back(d);
        for (auto& d : f.par) if (rng.below(8)) all.push_back(d);
        if (rng.below(10) == 0) all.push_back(f.par[0]);
      }
      for (size_t i = 0; i + 1 < all.size(); ++i) if (rng.below(5) == 0) std::swap(all[i], all[i + 1]);
      for (auto& d : all) rx.push(d);
      uint32_t prev = 0; bool first = true;
      for (auto& fr : rx.out) {
        CHECK(fr.data == pattern(fr.data.size(), fr.frame_seq));
        if (!first) CHECK(seqNewer(fr.frame_seq, prev));
        first = false; prev = fr.frame_seq;
      }
    }
  }
  // garbage Fec headers must never crash
  {
    FrameAssembler fa(3);
    FrameAssembler::Frame out;
    std::vector<uint8_t> pay(kUdpMaxPayload);
    rng.fill(pay);
    for (int k = 0; k < 20000; ++k) {
      UdpHeader h;
      h.type = UdpType::Fec;
      h.frame_seq = static_cast<uint32_t>(rng.below(6));
      h.epoch = 1;
      h.frag_count = static_cast<uint16_t>(1 + rng.below(300));
      h.frag_idx = static_cast<uint16_t>(rng.below(20));
      h.fec_total = static_cast<uint8_t>(rng.below(30));
      h.frame_size = static_cast<uint32_t>(rng.below(400 * kUdpMaxPayload));
      (void)fa.push(h, pay.data(), rng.below(kUdpMaxPayload + 2), out);
      if (rng.below(4) == 0) {
        h.type = UdpType::Video;
        (void)fa.push(h, pay.data(), rng.below(kUdpMaxPayload + 1), out);
      }
    }
    CHECK(true);
  }
}

static void testFecPerf() {
  // 1080p IDR-sized frame (~200 KB) with 20 % parity must encode in < 2 ms.
  auto au = pattern(200 * 1024, 3);
  uint8_t M = fecParityCount(static_cast<uint16_t>((au.size() + kUdpMaxPayload - 1) / kUdpMaxPayload), 20);
  std::vector<uint8_t> par;
  CHECK(fecEncode(au.data(), au.size(), M, par));  // warm up tables
  const int N = 100;
  auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < N; ++i) fecEncode(au.data(), au.size(), M, par);
  auto t1 = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count() / N;
  // decode with M lost fragments
  FecSet f = makeFec(1, au.size(), M, true);
  std::vector<char> dg(f.data.size(), 1), pg(f.par.size(), 1);
  for (unsigned i = 0; i < M; ++i) dg[i * 3] = 0;
  Rng rng(1);
  Rx rx;
  auto d0 = std::chrono::steady_clock::now();
  bool ok = fecTrial(rx, f, dg, pg, rng, false);
  auto d1 = std::chrono::steady_clock::now();
  double dms = std::chrono::duration<double, std::milli>(d1 - d0).count();
  CHECK(ok && rx.out[0].data == f.au);
  std::printf("  FEC perf: 200KB k=%u M=%u encode %.3f ms/frame, worst-case recover (%u lost) %.3f ms\n",
              static_cast<unsigned>(f.count), static_cast<unsigned>(M), ms, static_cast<unsigned>(M), dms);
#ifdef NDEBUG
  CHECK(ms < 2.0);
#endif
}

// ---------------------------------------------------------- SPS
namespace spstest {

struct W {
  std::vector<uint8_t> b; size_t n = 0;
  void bit(unsigned v) { if ((n & 7) == 0) b.push_back(0); if (v & 1u) b.back() = static_cast<uint8_t>(b.back() | (0x80u >> (n & 7))); ++n; }
  void bits(uint64_t v, unsigned k) { for (unsigned i = k; i > 0; --i) bit(static_cast<unsigned>((v >> (i - 1)) & 1u)); }
  void ue(uint32_t v) { uint64_t x = static_cast<uint64_t>(v) + 1; unsigned l = 0; while ((x >> l) != 0) ++l; for (unsigned i = 1; i < l; ++i) bit(0); bits(x, l); }
  void se(int32_t v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v)); }
  void trailing() { bit(1); while (n & 7) bit(0); }
};
static std::vector<uint8_t> escape(const std::vector<uint8_t>& in) {
  std::vector<uint8_t> o; unsigned z = 0;
  for (uint8_t v : in) { if (z >= 2 && v <= 3) { o.push_back(3); z = 0; } o.push_back(v); z = v == 0 ? z + 1 : 0; }
  return o;
}

struct Spec {
  unsigned profile = 66, level = 31, chroma = 1, poc = 0, num_ref = 1, wmb = 79, hmap = 44, fmo = 1;
  bool scaling = false, sep = false, crop = false;
  bool vui = false, aspect = false, aspect_ext = false, overscan = false, vsig = false, cdesc = false,
       chromaloc = false, timing = false, nalhrd = false, vclhrd = false, picstruct = false, br = false;
  uint32_t br_reorder = 5, br_dec = 9, br_mv = 1, br_bytes = 2, br_bits = 1, br_lh = 13, br_lv = 12;
};

static void hrd(W& w) {
  w.ue(1); w.bits(3, 4); w.bits(5, 4);
  for (int i = 0; i < 2; ++i) { w.ue(1000u + i); w.ue(5000u + i); w.bit(i); }
  w.bits(23, 5); w.bits(23, 5); w.bits(23, 5); w.bits(24, 5);
}

static std::vector<uint8_t> build(const Spec& s) {
  W w;
  w.bits(s.profile, 8); w.bits(0xC0, 8); w.bits(s.level, 8); w.ue(0);
  switch (s.profile) {
    case 100: case 110: case 122: case 244: case 44: {
      w.ue(s.chroma);
      if (s.chroma == 3) w.bit(s.sep);
      w.ue(0); w.ue(0); w.bit(0);
      w.bit(s.scaling);
      if (s.scaling) {
        unsigned n = s.chroma != 3 ? 8 : 12;
        for (unsigned i = 0; i < n; ++i) {
          unsigned kind = i % 3;
          w.bit(kind != 0);
          if (kind == 1) {  // explicit list with useDefault (nextScale 0 immediately: delta = -8)
            w.se(-8);
          } else if (kind == 2) {  // explicit full list
            unsigned sz = i < 6 ? 16 : 64;
            int last = 8;
            for (unsigned j = 0; j < sz; ++j) {
              int next = 8 + static_cast<int>((j * 3 + i) % 40);
              int delta = next - last;
              if (delta > 127) delta -= 256;
              if (delta < -128) delta += 256;
              w.se(delta);
              last = next;
            }
          }
        }
      }
      break;
    }
    default: break;
  }
  w.ue(2);  // log2_max_frame_num_minus4
  w.ue(s.poc);
  if (s.poc == 0) w.ue(4);
  else if (s.poc == 1) { w.bit(0); w.se(-3); w.se(2); w.ue(3); w.se(1); w.se(-1); w.se(2); }
  w.ue(s.num_ref);
  w.bit(0);
  w.ue(s.wmb); w.ue(s.hmap);
  w.bit(s.fmo); if (!s.fmo) w.bit(1);
  w.bit(1);
  w.bit(s.crop);
  if (s.crop) { w.ue(0); w.ue(0); w.ue(0); w.ue(4); }
  w.bit(s.vui);
  if (s.vui) {
    w.bit(s.aspect);
    if (s.aspect) { w.bits(s.aspect_ext ? 255 : 1, 8); if (s.aspect_ext) { w.bits(16, 16); w.bits(11, 16); } }
    w.bit(s.overscan); if (s.overscan) w.bit(1);
    w.bit(s.vsig);
    if (s.vsig) { w.bits(5, 3); w.bit(1); w.bit(s.cdesc); if (s.cdesc) { w.bits(1, 8); w.bits(1, 8); w.bits(1, 8); } }
    w.bit(s.chromaloc); if (s.chromaloc) { w.ue(1); w.ue(1); }
    w.bit(s.timing);
    if (s.timing) { w.bits(1, 32); w.bits(60, 32); w.bit(1); }  // 00 00 00 01 -> emulation prevention
    w.bit(s.nalhrd); if (s.nalhrd) hrd(w);
    w.bit(s.vclhrd); if (s.vclhrd) hrd(w);
    if (s.nalhrd || s.vclhrd) w.bit(1);
    w.bit(s.picstruct);
    w.bit(s.br);
    if (s.br) { w.bit(s.br_mv); w.ue(s.br_bytes); w.ue(s.br_bits); w.ue(s.br_lh); w.ue(s.br_lv); w.ue(s.br_reorder); w.ue(s.br_dec); }
  }
  w.trailing();
  std::vector<uint8_t> nal; nal.push_back(0x67);
  auto e = escape(w.b);
  nal.insert(nal.end(), e.begin(), e.end());
  return nal;
}

// What the patcher is specified to produce for a given spec.
static Spec patched(Spec s) {
  if (!s.vui) {
    s.vui = true; s.aspect = s.overscan = s.vsig = s.chromaloc = s.timing = s.nalhrd = s.vclhrd = s.picstruct = false;
  }
  if (!s.br) { s.br_mv = 1; s.br_bytes = 0; s.br_bits = 0; s.br_lh = 15; s.br_lv = 15; }
  s.br = true;
  s.br_reorder = 0;
  s.br_dec = std::max(1u, s.num_ref);
  return s;
}

// ---- HEVC
struct HSpec {
  unsigned maxsub = 0; bool ordering_present = true; bool conf = false;
  std::vector<std::array<uint32_t, 3>> entries;  // dec, reorder, lat
  unsigned tail_bits = 37;
};
static std::vector<uint8_t> buildHevc(const HSpec& s) {
  W w;
  w.bits(0, 4); w.bits(s.maxsub, 3); w.bit(1);
  // profile_tier_level: general 96 bits
  w.bits(1, 2); w.bit(0); w.bits(1, 5);          // space, tier, idc=1
  w.bits(0x60000000u, 32);                       // compat flags
  w.bits(0xA, 4); w.bits(0, 32); w.bits(0, 11); w.bit(0);  // 4 flags + 43 reserved + 1
  w.bits(120, 8);                                // level
  std::vector<int> pp(8), lp(8);
  for (unsigned i = 0; i < s.maxsub; ++i) { pp[i] = i % 2 == 0; lp[i] = i % 2 == 1 || i == 0; w.bit(pp[i]); w.bit(lp[i]); }
  if (s.maxsub > 0) for (unsigned i = s.maxsub; i < 8; ++i) w.bits(0, 2);
  for (unsigned i = 0; i < s.maxsub; ++i) {
    if (pp[i]) { w.bits(0x5555, 16); w.bits(0, 32); w.bits(0, 32); w.bits(0, 8); }
    if (lp[i]) w.bits(90, 8);
  }
  w.ue(0); w.ue(1); w.ue(1280); w.ue(720);
  w.bit(s.conf); if (s.conf) { w.ue(0); w.ue(0); w.ue(0); w.ue(4); }
  w.ue(0); w.ue(0); w.ue(4);
  w.bit(s.ordering_present);
  for (const auto& e : s.entries) { w.ue(e[0]); w.ue(e[1]); w.ue(e[2]); }
  for (unsigned i = 0; i < s.tail_bits; ++i) w.bit((i * 7 + 3) % 5 < 2);
  w.trailing();
  std::vector<uint8_t> nal = {0x42, 0x01};
  auto e = escape(w.b);
  nal.insert(nal.end(), e.begin(), e.end());
  return nal;
}

}  // namespace spstest

static void checkSpsCase(const spstest::Spec& s) {
  using namespace spstest;
  auto in = build(s);
  SpsInfo i0;
  CHECK(parseH264Sps(in.data(), in.size(), i0));
  CHECK(i0.profile_idc == s.profile && i0.num_ref_frames == s.num_ref && i0.vui_present == s.vui &&
        i0.bitstream_restriction == (s.vui && s.br) && i0.pic_order_cnt_type == s.poc);
  CHECK(i0.width == (s.wmb + 1) * 16);
  std::vector<uint8_t> out;
  CHECK(patchH264Sps(in.data(), in.size(), out));
  auto expect = build(patched(s));
  if (!(out == expect)) {
    std::printf("  SPS mismatch profile=%u num_ref=%u poc=%u vui=%d\n  in:  ", s.profile, s.num_ref,
                s.poc, static_cast<int>(s.vui));
    for (auto b : in) std::printf("%02X ", b);
    std::printf("\n  out: ");
    for (auto b : out) std::printf("%02X ", b);
    std::printf("\n  exp: ");
    for (auto b : expect) std::printf("%02X ", b);
    std::printf("\n");
  }
  CHECK(out == expect);
  SpsInfo i1;
  CHECK(parseH264Sps(out.data(), out.size(), i1));
  CHECK(i1.vui_present && i1.bitstream_restriction && i1.max_num_reorder_frames == 0 &&
        i1.max_dec_frame_buffering == std::max(1u, s.num_ref) && i1.num_ref_frames == s.num_ref);
  CHECK(i1.width == i0.width && i1.height == i0.height && i1.profile_idc == i0.profile_idc && i1.level_idc == i0.level_idc);
  // idempotent
  std::vector<uint8_t> out2;
  CHECK(patchH264Sps(out.data(), out.size(), out2) && out2 == out);
  // has_bframes: untouched
  std::vector<uint8_t> out3;
  CHECK(patchH264Sps(in.data(), in.size(), out3, true) && out3 == in);
  // every strict prefix: must not crash (result may be either)
  for (size_t n = 0; n < in.size(); ++n) {
    std::vector<uint8_t> tmp; SpsInfo ti;
    (void)patchH264Sps(in.data(), n, tmp);
    (void)parseH264Sps(in.data(), n, ti);
  }
}

static void testSps() {
  using namespace spstest;
  // profiles x VUI variants
  for (unsigned profile : {66u, 77u, 88u, 100u, 110u}) {
    for (unsigned variant = 0; variant < 9; ++variant) {
      Spec s; s.profile = profile; s.num_ref = variant % 5;
      s.poc = variant % 3; s.fmo = (variant == 4) ? 0 : 1; s.crop = (variant % 2) == 1;
      if (profile >= 100) { s.chroma = (profile == 110 && variant % 2) ? 3 : 1; s.scaling = variant % 2 == 0; s.sep = variant == 3; }
      switch (variant) {
        case 0: break;                                           // no VUI
        case 1: s.vui = true; break;                             // empty VUI
        case 2: s.vui = s.aspect = s.timing = true; break;       // timing (emulation prevention)
        case 3: s.vui = s.aspect = s.aspect_ext = s.overscan = s.vsig = s.cdesc = s.chromaloc = s.timing = true; break;
        case 4: s.vui = s.nalhrd = true; break;
        case 5: s.vui = s.vclhrd = s.timing = true; break;
        case 6: s.vui = s.nalhrd = s.vclhrd = s.timing = s.picstruct = true; break;
        case 7: s.vui = s.br = true; s.br_reorder = 5; s.br_dec = 9; break;   // existing restriction reduced
        case 8: s.vui = s.timing = s.nalhrd = s.br = true; s.br_reorder = 0; s.br_dec = 16; s.num_ref = 4; break;
        default: break;
      }
      checkSpsCase(s);
    }
  }
  // VUI colour fields (video_signal_type / colour_description), kept by the patch
  {
    Spec c; c.vui = c.vsig = c.cdesc = true;  // builder: full_range=1, primaries/transfer/matrix=1
    std::vector<uint8_t> in = build(c), out;
    SpsInfo ci;
    CHECK(parseH264Sps(in.data(), in.size(), ci));
    CHECK(ci.video_signal_type_present && ci.video_full_range && ci.colour_description_present &&
          ci.colour_primaries == 1 && ci.transfer_characteristics == 1 && ci.matrix_coefficients == 1);
    CHECK(patchH264Sps(in.data(), in.size(), out));
    SpsInfo co;
    CHECK(parseH264Sps(out.data(), out.size(), co) && co.video_full_range && co.matrix_coefficients == 1);
    Spec d; d.vui = d.vsig = true;  // no colour description -> matrix unspecified
    in = build(d);
    CHECK(parseH264Sps(in.data(), in.size(), ci) && ci.video_signal_type_present &&
          !ci.colour_description_present && ci.matrix_coefficients == 2);
    Spec e;  // no VUI
    in = build(e);
    CHECK(parseH264Sps(in.data(), in.size(), ci) && !ci.video_signal_type_present &&
          !ci.video_full_range && ci.matrix_coefficients == 2);
  }
  // exact bytes of a tiny known SPS without VUI: patched output is deterministic
  {
    Spec s; s.num_ref = 2; checkSpsCase(s);
    Spec t = s; t.num_ref = 0; checkSpsCase(t);
    auto in = build(t);
    std::vector<uint8_t> out; CHECK(patchH264Sps(in.data(), in.size(), out));
    SpsInfo inf; CHECK(parseH264Sps(out.data(), out.size(), inf) && inf.max_dec_frame_buffering == 1);
  }
  // non-SPS / garbage rejected
  {
    std::vector<uint8_t> out(3, 9);
    const uint8_t pps[] = {0x68, 0xCE, 0x3C, 0x80};
    CHECK(!patchH264Sps(pps, sizeof(pps), out) && out.size() == 3);
    CHECK(!patchH264Sps(nullptr, 0, out));
    const uint8_t tiny[] = {0x67, 0x42};
    CHECK(!patchH264Sps(tiny, sizeof(tiny), out));
    Rng rng(99);
    for (int i = 0; i < 30000; ++i) {
      std::vector<uint8_t> r(4 + rng.below(40)); rng.fill(r);
      r[0] = 0x67; if (i % 3 == 0 && r.size() > 3) r[1] = 100;
      std::vector<uint8_t> o; SpsInfo inf;
      (void)patchH264Sps(r.data(), r.size(), o);
      (void)parseH264Sps(r.data(), r.size(), inf);
      r[0] = 0x42; r[1] = 0x01;
      (void)patchHevcSps(r.data(), r.size(), o);
      (void)parseHevcSps(r.data(), r.size(), inf);
    }
    // mutated valid SPS
    Spec s; s.profile = 100; s.scaling = true; s.vui = s.timing = s.nalhrd = true;
    auto base = build(s);
    for (int i = 0; i < 20000; ++i) {
      auto m = base; m[1 + rng.below(m.size() - 1)] = static_cast<uint8_t>(rng.u32());
      std::vector<uint8_t> o; (void)patchH264Sps(m.data(), m.size(), o);
    }
    CHECK(true);
  }
  // real-world vector: x264 1080p High@4.0 SPS with VUI timing (from an ffmpeg/x264 stream)
  {
    const uint8_t x264[] = {0x67, 0x64, 0x00, 0x28, 0xAC, 0xD9, 0x40, 0x78, 0x02, 0x27, 0xE5, 0xC0, 0x44, 0x00,
                            0x00, 0x03, 0x00, 0x04, 0x00, 0x00, 0x03, 0x00, 0xC8, 0x3C, 0x60, 0xC6, 0x58};
    SpsInfo i0; CHECK(parseH264Sps(x264, sizeof(x264), i0));
    CHECK(i0.profile_idc == 100 && i0.level_idc == 40 && i0.width == 1920 && i0.height == 1080 && i0.vui_present);
    std::vector<uint8_t> o, o2;
    CHECK(patchH264Sps(x264, sizeof(x264), o));
    SpsInfo i1; CHECK(parseH264Sps(o.data(), o.size(), i1));
    CHECK(i1.bitstream_restriction && i1.max_num_reorder_frames == 0 &&
          i1.max_dec_frame_buffering == std::max(1u, i0.num_ref_frames) && i1.width == 1920 && i1.height == 1080);
    CHECK(patchH264Sps(o.data(), o.size(), o2) && o2 == o);
    // typical Android baseline SPS without VUI (1280x720 Baseline 3.1)
    const uint8_t droid[] = {0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16, 0xE8, 0x06, 0xD0, 0xA1, 0x35};
    SpsInfo d0;
    if (parseH264Sps(droid, sizeof(droid), d0)) {
      CHECK(d0.profile_idc == 66);
      CHECK(patchH264Sps(droid, sizeof(droid), o));
      CHECK(parseH264Sps(o.data(), o.size(), i1) && i1.bitstream_restriction && i1.width == d0.width &&
            i1.height == d0.height && i1.num_ref_frames == d0.num_ref_frames);
    }
  }
  // HEVC
  {
    for (unsigned maxsub : {0u, 1u, 3u}) {
      for (int variant = 0; variant < 4; ++variant) {
        HSpec h; h.maxsub = maxsub; h.conf = variant % 2 == 1;
        h.ordering_present = variant < 2;
        size_t ne = h.ordering_present ? maxsub + 1 : 1;
        for (size_t i = 0; i < ne; ++i)
          h.entries.push_back({static_cast<uint32_t>(4 + i), static_cast<uint32_t>(2 + i % 2), static_cast<uint32_t>(i)});
        h.tail_bits = 13 + static_cast<unsigned>(variant) * 11;
        auto in = buildHevc(h);
        SpsInfo i0; CHECK(parseHevcSps(in.data(), in.size(), i0));
        CHECK(i0.max_num_reorder_frames == h.entries.back()[1] && i0.max_dec_frame_buffering == h.entries.back()[0] + 1);
        CHECK(i0.profile_idc == 1 && i0.level_idc == 120);
        std::vector<uint8_t> out;
        CHECK(patchHevcSps(in.data(), in.size(), out));
        HSpec e = h;
        for (auto& en : e.entries) { en[1] = 0; en[2] = 0; }
        CHECK(out == buildHevc(e));
        SpsInfo i1; CHECK(parseHevcSps(out.data(), out.size(), i1));
        CHECK(i1.max_num_reorder_frames == 0 && i1.max_latency_increase_plus1 == 0 &&
              i1.max_dec_frame_buffering == i0.max_dec_frame_buffering && i1.profile_idc == 1);
        std::vector<uint8_t> out2;
        CHECK(patchHevcSps(out.data(), out.size(), out2) && out2 == out);
        CHECK(patchHevcSps(in.data(), in.size(), out2, true) && out2 == in);
        for (size_t n = 0; n < in.size(); ++n) { std::vector<uint8_t> t; SpsInfo ti; (void)patchHevcSps(in.data(), n, t); (void)parseHevcSps(in.data(), n, ti); }
      }
    }
  }
  // Annex-B walker
  {
    Spec s; s.num_ref = 3; s.vui = s.timing = true;
    auto sps = build(s);
    const uint8_t pps[] = {0x68, 0xCE, 0x3C, 0x80};
    std::vector<uint8_t> idr(300); Rng rng(4); rng.fill(idr); idr[0] = 0x65;
    for (auto& b : idr) if (b == 0) b = 1;  // no accidental start codes
    std::vector<uint8_t> in = {0, 0, 0, 1};
    in.insert(in.end(), sps.begin(), sps.end());
    in.insert(in.end(), {0, 0, 1});
    in.insert(in.end(), pps, pps + 4);
    in.insert(in.end(), {0, 0, 0, 1});
    in.insert(in.end(), idr.begin(), idr.end());
    in.insert(in.end(), {0, 0, 0, 1});     // second SPS in-band (as in IDR access units)
    in.insert(in.end(), sps.begin(), sps.end());
    std::vector<uint8_t> out;
    size_t n = patchAnnexBSps(in.data(), in.size(), Codec::H264, out);
    CHECK(n == 2);
    std::vector<uint8_t> psps; CHECK(patchH264Sps(sps.data(), sps.size(), psps));
    std::vector<uint8_t> exp = {0, 0, 0, 1};
    exp.insert(exp.end(), psps.begin(), psps.end());
    exp.insert(exp.end(), {0, 0, 1}); exp.insert(exp.end(), pps, pps + 4);
    exp.insert(exp.end(), {0, 0, 0, 1}); exp.insert(exp.end(), idr.begin(), idr.end());
    exp.insert(exp.end(), {0, 0, 0, 1}); exp.insert(exp.end(), psps.begin(), psps.end());
    CHECK(out == exp);
    CHECK(patchAnnexBSps(in.data(), in.size(), Codec::H264, out, true) == 2 && out == in);
    // junk before the first start code, trailing zero bytes, empty input, no start code
    std::vector<uint8_t> j = {9, 9};
    j.insert(j.end(), in.begin(), in.end());
    j.push_back(0); j.push_back(0);
    CHECK(patchAnnexBSps(j.data(), j.size(), Codec::H264, out) == 2);
    CHECK(out.size() == exp.size() + 4 && out[0] == 9 && out[out.size() - 1] == 0);
    CHECK(patchAnnexBSps(nullptr, 0, Codec::H264, out) == 0 && out.empty());
    const uint8_t nosc[] = {1, 2, 3};
    CHECK(patchAnnexBSps(nosc, 3, Codec::H264, out) == 0 && out.size() == 3);
    // HEVC
    HSpec h; h.entries.push_back({4, 2, 1});
    auto hs = buildHevc(h);
    std::vector<uint8_t> hin = {0, 0, 0, 1, 0x40, 0x01, 0x0C, 0x01};  // VPS-ish, untouched
    hin.insert(hin.end(), {0, 0, 0, 1}); hin.insert(hin.end(), hs.begin(), hs.end());
    CHECK(patchAnnexBSps(hin.data(), hin.size(), Codec::Hevc, out) == 1);
    CHECK(std::equal(hin.begin(), hin.begin() + 8, out.begin()));
    SpsInfo hi; CHECK(parseHevcSps(out.data() + 12, out.size() - 12, hi) && hi.max_num_reorder_frames == 0);
    // an H.264 SPS is not touched when asking for HEVC
    CHECK(patchAnnexBSps(in.data(), in.size(), Codec::Hevc, out) == 0 && out == in);
    // unparsable SPS copied unchanged
    std::vector<uint8_t> bad = {0, 0, 0, 1, 0x67, 0x64, 0x00};
    CHECK(patchAnnexBSps(bad.data(), bad.size(), Codec::H264, out) == 0 && out == bad);
  }
}

static void run(const char* name, void (*fn)()) {
  int before = g_fails;
  ++g_tests;
  fn();
  bool ok = g_fails == before;
  if (!ok) ++g_test_fails;
  std::printf("[%s] %s\n", ok ? " OK " : "FAIL", name);
}

int main() {
  run("messages", testMessages);
  run("sha256", testSha256);
  run("hmac-sha256", testHmac);
  run("auth mac", testAuthMac);
  run("ct-equal + hex", testCtEqualHex);
  run("pairing messages", testPairMessages);
  run("hello v2", testHelloV2);
  run("hello-ack v2", testHelloAckV2);
  run("tcp framing", testTcp);
  run("udp header + packetize", testUdp);
  run("frame assembler", testAssembler);
  run("fuzz", testFuzz);
  run("end to end", testEndToEnd);
  run("fec layout", testFecLayout);
  run("fec", testFec);
  run("fec perf", testFecPerf);
  run("sps patch", testSps);
  std::printf("%d tests, %d failed; %d checks, %d failed\n", g_tests, g_test_fails, g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
