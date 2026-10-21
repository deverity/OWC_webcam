#include "owc/protocol.h"

#include <algorithm>
#include <cstring>

namespace owc {
namespace {

// ------------------------------------------------------------------ writer
struct Writer {
  std::vector<uint8_t>& b;
  void u8(uint32_t v) { b.push_back(static_cast<uint8_t>(v & 0xFFu)); }
  void u16(uint32_t v) { u8(v); u8(v >> 8); }
  void u32(uint32_t v) { u8(v); u8(v >> 8); u8(v >> 16); u8(v >> 24); }
  void u64(uint64_t v) {
    u32(static_cast<uint32_t>(v & 0xFFFFFFFFu));
    u32(static_cast<uint32_t>(v >> 32));
  }
  void i8(int v) { u8(static_cast<uint32_t>(v)); }
  void i16(int v) { u16(static_cast<uint32_t>(v) & 0xFFFFu); }
  void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
  void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
  void f32(float v) { uint32_t x; std::memcpy(&x, &v, 4); u32(x); }
  void f64(double v) { uint64_t x; std::memcpy(&x, &v, 8); u64(x); }
  void str(const std::string& s) {
    size_t n = std::min<size_t>(s.size(), 0xFFFFu);
    u16(static_cast<uint32_t>(n));
    b.insert(b.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
  }
};

// ------------------------------------------------------------------ reader
struct Reader {
  const uint8_t* p;
  size_t n;
  size_t pos = 0;
  bool ok = true;
  Reader(const uint8_t* d, size_t len) : p(d), n(d ? len : 0) {}
  size_t left() const { return n - pos; }
  bool need(size_t k) {
    if (!ok || left() < k) { ok = false; return false; }
    return true;
  }
  uint8_t u8() { if (!need(1)) return 0; return p[pos++]; }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = static_cast<uint16_t>(p[pos] | (p[pos + 1] << 8));
    pos += 2;
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = static_cast<uint32_t>(p[pos]) | (static_cast<uint32_t>(p[pos + 1]) << 8) |
                 (static_cast<uint32_t>(p[pos + 2]) << 16) |
                 (static_cast<uint32_t>(p[pos + 3]) << 24);
    pos += 4;
    return v;
  }
  uint64_t u64() {
    uint64_t lo = u32();
    uint64_t hi = u32();
    return lo | (hi << 32);
  }
  int8_t i8() { return static_cast<int8_t>(u8()); }
  int16_t i16() { return static_cast<int16_t>(u16()); }
  int32_t i32() { return static_cast<int32_t>(u32()); }
  int64_t i64() { return static_cast<int64_t>(u64()); }
  float f32() { uint32_t x = u32(); float v; std::memcpy(&v, &x, 4); return v; }
  double f64() { uint64_t x = u64(); double v; std::memcpy(&v, &x, 8); return v; }
  bool str(std::string& s) {
    size_t len = u16();
    if (!need(len)) return false;
    s.assign(reinterpret_cast<const char*>(p + pos), len);
    pos += len;
    return true;
  }
};

void putValue(Writer& w, const ParamValue& v) {
  w.u8(static_cast<uint8_t>(v.type));
  switch (v.type) {
    case ValueType::Bool: w.u8(v.i != 0 ? 1 : 0); break;
    case ValueType::Int32:
    case ValueType::Enum: w.i32(static_cast<int32_t>(v.i)); break;
    case ValueType::Int64: w.i64(v.i); break;
    case ValueType::Float: w.f32(v.f); break;
    default: break;  // unknown type: nothing follows (undecodable by design)
  }
}

bool getValue(Reader& r, ParamValue& v) {
  uint8_t t = r.u8();
  v = ParamValue();
  switch (t) {
    case 0: v.type = ValueType::Bool; v.i = r.u8() != 0 ? 1 : 0; break;
    case 1: v.type = ValueType::Int32; v.i = r.i32(); break;
    case 2: v.type = ValueType::Int64; v.i = r.i64(); break;
    case 3: v.type = ValueType::Float; v.f = r.f32(); break;
    case 4: v.type = ValueType::Enum; v.i = r.i32(); break;
    default: r.ok = false; break;
  }
  return r.ok;
}

constexpr uint8_t kDiscoveryKindRequest = 1;
constexpr uint8_t kDiscoveryKindReply = 2;

template <class T>
size_t clampCount(const std::vector<T>& v) {
  return std::min<size_t>(v.size(), 0xFFFFu);
}

void putCamera(Writer& w, const CameraInfo& c) {
  w.str(c.id); w.u8(static_cast<uint8_t>(c.facing)); w.u8(c.hw_level);
  w.i32(c.sensor_orientation); w.f32(c.max_zoom); w.u8(c.has_flash ? 1 : 0);
  size_t ns = clampCount(c.sizes);
  w.u16(static_cast<uint32_t>(ns));
  for (size_t i = 0; i < ns; ++i) {
    w.u16(c.sizes[i].width); w.u16(c.sizes[i].height); w.u16(c.sizes[i].max_fps);
  }
  size_t nf = clampCount(c.fps_ranges);
  w.u16(static_cast<uint32_t>(nf));
  for (size_t i = 0; i < nf; ++i) { w.u16(c.fps_ranges[i].first); w.u16(c.fps_ranges[i].second); }
}

bool getCamera(Reader& r, CameraInfo& c) {
  r.str(c.id);
  c.facing = static_cast<Facing>(r.u8()); c.hw_level = r.u8();
  c.sensor_orientation = r.i32(); c.max_zoom = r.f32(); c.has_flash = r.u8() != 0;
  c.sizes.clear(); c.fps_ranges.clear();
  size_t ns = r.u16();
  if (!r.ok || ns * 6 > r.left()) { r.ok = false; return false; }
  c.sizes.reserve(ns);
  for (size_t i = 0; i < ns; ++i) {
    VideoSize s; s.width = r.u16(); s.height = r.u16(); s.max_fps = r.u16();
    c.sizes.push_back(s);
  }
  size_t nf = r.u16();
  if (!r.ok || nf * 4 > r.left()) { r.ok = false; return false; }
  c.fps_ranges.reserve(nf);
  for (size_t i = 0; i < nf; ++i) {
    uint16_t a = r.u16();
    uint16_t b = r.u16();
    c.fps_ranges.emplace_back(a, b);
  }
  return r.ok;
}

void putDesc(Writer& w, const ParamDesc& d) {
  w.u16(static_cast<uint16_t>(d.id)); w.u8(static_cast<uint8_t>(d.type)); w.u8(d.flags);
  w.f64(d.min); w.f64(d.max); w.f64(d.step);
  putValue(w, d.current);
  size_t no = clampCount(d.options);
  w.u16(static_cast<uint32_t>(no));
  for (size_t i = 0; i < no; ++i) { w.i32(d.options[i].value); w.str(d.options[i].label); }
}

bool getDesc(Reader& r, ParamDesc& d) {
  d.id = static_cast<ParamId>(r.u16()); d.type = static_cast<ValueType>(r.u8()); d.flags = r.u8();
  d.min = r.f64(); d.max = r.f64(); d.step = r.f64();
  if (!getValue(r, d.current)) return false;
  d.options.clear();
  size_t no = r.u16();
  if (!r.ok || no * 6 > r.left()) { r.ok = false; return false; }
  d.options.reserve(no);
  for (size_t i = 0; i < no; ++i) {
    EnumOption o; o.value = r.i32();
    if (!r.str(o.label)) return false;
    d.options.push_back(std::move(o));
  }
  return r.ok;
}

}  // namespace

// ================================================================== messages
std::vector<uint8_t> encode(const Hello& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u32(m.magic); w.u16(m.version); w.u8(static_cast<uint8_t>(m.transport)); w.str(m.client_name);
  return b;
}
bool decode(const uint8_t* p, size_t n, Hello& m) {
  Reader r(p, n);
  m.magic = r.u32(); m.version = r.u16();
  m.transport = static_cast<VideoTransport>(r.u8());
  r.str(m.client_name);
  return r.ok;
}

std::vector<uint8_t> encode(const HelloAck& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u32(m.magic); w.u16(m.version); w.u8(static_cast<uint8_t>(m.status));
  w.u32(m.session_id); w.u16(m.udp_port); w.str(m.device_name); w.str(m.model);
  w.u16(m.android_api); w.u32(m.codec_mask);
  return b;
}
bool decode(const uint8_t* p, size_t n, HelloAck& m) {
  Reader r(p, n);
  m.magic = r.u32(); m.version = r.u16();
  m.status = static_cast<HelloStatus>(r.u8());
  m.session_id = r.u32(); m.udp_port = r.u16();
  r.str(m.device_name); r.str(m.model);
  m.android_api = r.u16(); m.codec_mask = r.u32();
  return r.ok;
}

std::vector<uint8_t> encode(const Capabilities& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u16(m.active_camera);
  size_t nc = clampCount(m.cameras);
  w.u16(static_cast<uint32_t>(nc));
  for (size_t i = 0; i < nc; ++i) putCamera(w, m.cameras[i]);
  size_t np = clampCount(m.params);
  w.u16(static_cast<uint32_t>(np));
  for (size_t i = 0; i < np; ++i) putDesc(w, m.params[i]);
  return b;
}
bool decode(const uint8_t* p, size_t n, Capabilities& m) {
  Reader r(p, n);
  m.active_camera = r.u16();
  m.cameras.clear(); m.params.clear();
  size_t nc = r.u16();
  if (!r.ok || nc * 20 > r.left()) return false;  // a camera is >= 20 bytes
  for (size_t i = 0; i < nc; ++i) {
    CameraInfo c;
    if (!getCamera(r, c)) return false;
    m.cameras.push_back(std::move(c));
  }
  size_t np = r.u16();
  if (!r.ok || np * 30 > r.left()) return false;  // a ParamDesc is >= 30 bytes
  for (size_t i = 0; i < np; ++i) {
    ParamDesc d;
    if (!getDesc(r, d)) return false;
    m.params.push_back(std::move(d));
  }
  return r.ok;
}

std::vector<uint8_t> encode(const SetParam& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u16(static_cast<uint16_t>(m.id)); putValue(w, m.value);
  return b;
}
bool decode(const uint8_t* p, size_t n, SetParam& m) {
  Reader r(p, n);
  m.id = static_cast<ParamId>(r.u16());
  return getValue(r, m.value);
}

std::vector<uint8_t> encode(const ParamState& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u16(static_cast<uint16_t>(m.id)); w.u8(static_cast<uint8_t>(m.status)); putValue(w, m.value);
  return b;
}
bool decode(const uint8_t* p, size_t n, ParamState& m) {
  Reader r(p, n);
  m.id = static_cast<ParamId>(r.u16());
  m.status = static_cast<ParamStatus>(r.u8());
  return getValue(r, m.value);
}

std::vector<uint8_t> encode(const Command& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u16(static_cast<uint16_t>(m.cmd)); w.i64(m.arg); w.f32(m.x); w.f32(m.y); w.str(m.text);
  return b;
}
bool decode(const uint8_t* p, size_t n, Command& m) {
  Reader r(p, n);
  m.cmd = static_cast<CommandId>(r.u16()); m.arg = r.i64(); m.x = r.f32(); m.y = r.f32();
  r.str(m.text);
  return r.ok;
}

std::vector<uint8_t> encode(const VideoFormat& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u32(m.epoch); w.u8(static_cast<uint8_t>(m.codec)); w.u16(m.width); w.u16(m.height);
  w.u16(m.fps); w.i16(m.rotation_deg); w.u8(m.mirror ? 1 : 0);
  w.u32(static_cast<uint32_t>(m.csd.size()));
  b.insert(b.end(), m.csd.begin(), m.csd.end());
  return b;
}
bool decode(const uint8_t* p, size_t n, VideoFormat& m) {
  Reader r(p, n);
  m.epoch = r.u32(); m.codec = static_cast<Codec>(r.u8());
  m.width = r.u16(); m.height = r.u16(); m.fps = r.u16();
  m.rotation_deg = r.i16(); m.mirror = r.u8() != 0;
  uint32_t len = r.u32();
  if (!r.ok || len > r.left()) return false;
  m.csd.assign(p + r.pos, p + r.pos + len);
  return true;
}

std::vector<uint8_t> encode(const Ping& m) {
  std::vector<uint8_t> b; Writer w{b}; w.u64(m.t0_us); return b;
}
bool decode(const uint8_t* p, size_t n, Ping& m) {
  Reader r(p, n); m.t0_us = r.u64(); return r.ok;
}
std::vector<uint8_t> encode(const Pong& m) {
  std::vector<uint8_t> b; Writer w{b}; w.u64(m.t0_us); w.u64(m.phone_us); return b;
}
bool decode(const uint8_t* p, size_t n, Pong& m) {
  Reader r(p, n); m.t0_us = r.u64(); m.phone_us = r.u64(); return r.ok;
}

std::vector<uint8_t> encode(const Stats& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.f32(m.camera_fps); w.f32(m.encode_fps); w.f32(m.send_fps);
  w.u32(m.bitrate_bps); w.u32(m.dropped_frames); w.u32(m.send_queue);
  w.f32(m.encode_latency_ms); w.i8(m.battery_pct); w.f32(m.battery_temp_c);
  w.u8(m.charging ? 1 : 0); w.u8(m.thermal_status);
  w.u32(m.target_bitrate_bps); w.f32(m.isp_latency_ms); w.i8(m.fec_percent);
  return b;
}
bool decode(const uint8_t* p, size_t n, Stats& m) {
  Reader r(p, n);
  m.camera_fps = r.f32(); m.encode_fps = r.f32(); m.send_fps = r.f32();
  m.bitrate_bps = r.u32(); m.dropped_frames = r.u32(); m.send_queue = r.u32();
  m.encode_latency_ms = r.f32(); m.battery_pct = r.i8(); m.battery_temp_c = r.f32();
  m.charging = r.u8() != 0; m.thermal_status = r.u8();
  if (r.ok && r.left() >= 9) {  // v1.1 trailer
    m.target_bitrate_bps = r.u32(); m.isp_latency_ms = r.f32(); m.fec_percent = r.i8();
  }
  return r.ok;
}

std::vector<uint8_t> encode(const LogMsg& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u8(static_cast<uint8_t>(m.level)); w.str(m.text);
  return b;
}
bool decode(const uint8_t* p, size_t n, LogMsg& m) {
  Reader r(p, n);
  m.level = static_cast<LogLevel>(r.u8());
  r.str(m.text);
  return r.ok;
}

std::vector<uint8_t> encode(const ReceiverReport& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u32(m.frames_received); w.u32(m.frames_lost); w.u32(m.fragments_received);
  w.u32(m.fragments_lost); w.f32(m.jitter_ms); w.f32(m.delay_gradient_ms);
  w.u32(m.recv_bitrate_bps); w.f32(m.decode_ms);
  return b;
}
bool decode(const uint8_t* p, size_t n, ReceiverReport& m) {
  Reader r(p, n);
  m.frames_received = r.u32(); m.frames_lost = r.u32(); m.fragments_received = r.u32();
  m.fragments_lost = r.u32(); m.jitter_ms = r.f32(); m.delay_gradient_ms = r.f32();
  m.recv_bitrate_bps = r.u32(); m.decode_ms = r.f32();
  return r.ok;
}

// ------------------------------------------------------------- video frame
void encodeVideoFrame(const VideoFrameHeader& h, const uint8_t* au, size_t au_len,
                      std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(kVideoFrameHeaderSize + au_len);
  Writer w{out};
  w.u32(h.epoch); w.u32(h.frame_seq); w.u64(h.capture_ts_us); w.u8(h.flags);
  if (au_len && au) out.insert(out.end(), au, au + au_len);
}
bool decodeVideoFrame(const uint8_t* p, size_t n, VideoFrameHeader& h, const uint8_t*& au,
                      size_t& au_len) {
  Reader r(p, n);
  h.epoch = r.u32(); h.frame_seq = r.u32(); h.capture_ts_us = r.u64(); h.flags = r.u8();
  if (!r.ok) return false;
  au = p + r.pos;
  au_len = r.left();
  return true;
}

// --------------------------------------------------------------- discovery
std::vector<uint8_t> encodeDiscoveryRequest() {
  std::vector<uint8_t> b; Writer w{b};
  w.u32(kMagic); w.u16(kProtocolVersion); w.u8(kDiscoveryKindRequest);
  return b;
}
bool isDiscoveryRequest(const uint8_t* p, size_t n) {
  Reader r(p, n);
  uint32_t magic = r.u32(); r.u16(); uint8_t kind = r.u8();
  return r.ok && magic == kMagic && kind == kDiscoveryKindRequest;
}
std::vector<uint8_t> encode(const DiscoveryReply& m) {
  std::vector<uint8_t> b; Writer w{b};
  w.u32(kMagic); w.u16(kProtocolVersion); w.u8(kDiscoveryKindReply);
  w.u16(m.control_port); w.u16(m.udp_port); w.str(m.device_name); w.str(m.model);
  w.u8(m.busy ? 1 : 0);
  return b;
}
bool decode(const uint8_t* p, size_t n, DiscoveryReply& m) {
  Reader r(p, n);
  uint32_t magic = r.u32(); r.u16(); uint8_t kind = r.u8();
  if (!r.ok || magic != kMagic || kind != kDiscoveryKindReply) return false;
  m.control_port = r.u16(); m.udp_port = r.u16();
  r.str(m.device_name); r.str(m.model);
  m.busy = r.u8() != 0;
  return r.ok;
}

// --------------------------------------------------------------- TCP framing
void writeTcpHeader(uint8_t out[kTcpHeaderSize], MsgType type, uint32_t len) {
  uint16_t t = static_cast<uint16_t>(type);
  out[0] = static_cast<uint8_t>(t & 0xFFu); out[1] = static_cast<uint8_t>(t >> 8);
  out[2] = 0; out[3] = 0;
  out[4] = static_cast<uint8_t>(len & 0xFFu); out[5] = static_cast<uint8_t>((len >> 8) & 0xFFu);
  out[6] = static_cast<uint8_t>((len >> 16) & 0xFFu); out[7] = static_cast<uint8_t>(len >> 24);
}
void appendTcpMessage(std::vector<uint8_t>& out, MsgType type, const uint8_t* payload,
                      size_t len) {
  uint8_t h[kTcpHeaderSize];
  writeTcpHeader(h, type, static_cast<uint32_t>(len));
  out.insert(out.end(), h, h + kTcpHeaderSize);
  if (len && payload) out.insert(out.end(), payload, payload + len);
}

namespace {
// Payload length of the header at b[pos..], or -1 if fewer than 8 bytes buffered.
int64_t peekLen(const std::vector<uint8_t>& b, size_t pos) {
  if (b.size() - pos < kTcpHeaderSize) return -1;
  const uint8_t* h = b.data() + pos;
  return static_cast<int64_t>(static_cast<uint32_t>(h[4]) | (static_cast<uint32_t>(h[5]) << 8) |
                              (static_cast<uint32_t>(h[6]) << 16) |
                              (static_cast<uint32_t>(h[7]) << 24));
}
}  // namespace

bool TcpFrameParser::feed(const uint8_t* data, size_t len) {
  if (error_) return false;
  if (len && data) {
    // Compact the consumed prefix so the buffer cannot grow without bound.
    if (pos_ > 0 && (pos_ >= buf_.size() || pos_ >= 4096)) {
      buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(pos_));
      pos_ = 0;
    }
    buf_.insert(buf_.end(), data, data + len);
  }
  if (peekLen(buf_, pos_) > static_cast<int64_t>(kMaxTcpPayload)) {
    error_ = true; buf_.clear(); pos_ = 0;
    return false;
  }
  return true;
}

bool TcpFrameParser::next(Message& out) {
  if (error_) return false;
  int64_t l = peekLen(buf_, pos_);
  if (l < 0) return false;
  if (l > static_cast<int64_t>(kMaxTcpPayload)) {
    error_ = true; buf_.clear(); pos_ = 0;
    return false;
  }
  size_t total = kTcpHeaderSize + static_cast<size_t>(l);
  if (buf_.size() - pos_ < total) return false;
  const uint8_t* h = buf_.data() + pos_;
  out.type = static_cast<MsgType>(static_cast<uint16_t>(h[0] | (h[1] << 8)));
  out.payload.assign(h + kTcpHeaderSize, h + total);
  pos_ += total;
  if (pos_ == buf_.size()) { buf_.clear(); pos_ = 0; }
  return true;
}

// ------------------------------------------------------------------- UDP
void writeUdpHeader(const UdpHeader& h, uint8_t out[kUdpHeaderSize]) {
  std::vector<uint8_t> tmp;
  tmp.reserve(kUdpHeaderSize);
  Writer w{tmp};
  w.u16(kUdpMagic); w.u8(kUdpVersion); w.u8(static_cast<uint8_t>(h.type));
  w.u32(h.session_id); w.u32(h.frame_seq); w.u16(h.frag_idx); w.u16(h.frag_count);
  w.u32(h.frame_size); w.u64(h.capture_ts_us); w.u16(h.epoch); w.u8(h.flags); w.u8(h.fec_total);
  std::memcpy(out, tmp.data(), kUdpHeaderSize);
}
bool readUdpHeader(const uint8_t* p, size_t n, UdpHeader& h) {
  if (!p || n < kUdpHeaderSize) return false;
  Reader r(p, n);
  if (r.u16() != kUdpMagic) return false;
  if (r.u8() != kUdpVersion) return false;
  h.type = static_cast<UdpType>(r.u8());
  h.session_id = r.u32(); h.frame_seq = r.u32(); h.frag_idx = r.u16(); h.frag_count = r.u16();
  h.frame_size = r.u32(); h.capture_ts_us = r.u64(); h.epoch = r.u16(); h.flags = r.u8();
  h.fec_total = r.u8();
  return r.ok;
}

#include "fec.inc.h"
#include "sps.inc.h"

// ----------------------------------------------------------- FrameAssembler
// Fragment layout rule: every fragment except the last carries the same payload
// length P (learned from the first non-last fragment seen, or from the parity shard
// size S); fragment i starts at i*P. The last fragment starts at frame_size-len and
// must satisfy (frag_count-1)*P == frame_size-len and 1 <= len <= P. frame_size and
// frag_count must be identical across all datagrams of a frame. frag_count==1 needs
// len==frame_size. FEC: see protocol.h.
struct FrameAssembler::Impl {
  struct Slot {
    bool used = false;
    uint32_t seq = 0;
    uint16_t count = 0;
    uint32_t size = 0;
    uint32_t P = 0;  // 0 = unknown
    uint32_t last_len = 0;
    bool have_last = false;
    uint16_t received = 0;  // data fragments present (received or recovered)
    uint16_t rec = 0;       // of which recovered by RS
    uint8_t flags = 0;
    uint64_t ts = 0;
    std::vector<uint8_t> buf;
    std::vector<uint8_t> got;  // one byte per data fragment
    // FEC
    uint8_t fec_m = 0;
    uint32_t fec_s = 0;
    uint16_t parity_recv = 0;
    FecLayout lay;
    std::vector<uint8_t> pbuf, pgot;
  };
  std::vector<Slot> slots;
  Counters c;
  bool have_epoch = false;
  uint16_t epoch = 0;
  bool have_floor = false;  // highest seq completed/evicted: older-or-equal is "late"
  uint32_t floor_seq = 0;
  bool have_last_complete = false;
  uint32_t last_complete = 0;
  bool waiting = true;  // drop deltas until a keyframe is delivered
  bool need = true;     // caller-visible request flag

  explicit Impl(size_t mp) : slots(mp ? mp : 1) {}

  void clearState() {
    for (auto& s : slots) s.used = false;
    have_floor = have_last_complete = false;
    waiting = true;
    need = true;
  }
  void raiseFloor(uint32_t s) {
    if (!have_floor || seqNewer(s, floor_seq)) { floor_seq = s; have_floor = true; }
  }

  bool place(Slot& s, uint16_t idx, const uint8_t* p, size_t len) {
    size_t off;
    if (s.count == 1) {
      if (len != s.size) return false;
      off = 0;
    } else if (idx == s.count - 1) {
      if (len == 0 || len > s.size) return false;
      uint64_t o = s.size - len;
      if (s.P != 0 && (len > s.P || o != static_cast<uint64_t>(s.count - 1) * s.P)) return false;
      off = static_cast<size_t>(o);
      s.have_last = true;
      s.last_len = static_cast<uint32_t>(len);
    } else {
      if (len == 0 || len > s.size) return false;
      uint32_t l32 = static_cast<uint32_t>(len);
      if (s.P != 0) {
        if (l32 != s.P) return false;
      } else {
        if (static_cast<uint64_t>(s.count - 1) * l32 >= s.size) return false;
        if (static_cast<uint64_t>(s.count) * l32 < s.size) return false;
        if (s.have_last && (s.last_len > l32 ||
                            s.size - s.last_len != static_cast<uint64_t>(s.count - 1) * l32))
          return false;
        s.P = l32;
      }
      off = static_cast<size_t>(idx) * s.P;
    }
    if (len) std::memcpy(s.buf.data() + off, p, len);
    return true;
  }

  enum class Par { Ok, Invalid, Dup };
  Par acceptParity(Slot& s, const UdpHeader& h, const uint8_t* p, size_t len) {
    if (s.fec_m == 0) {
      FecLayout L = fecLayout(s.count, h.fec_total);
      if (L.nblocks == 0) return Par::Invalid;
      const uint64_t S = len;
      if (s.count == 1) {
        if (s.size > S) return Par::Invalid;
      } else {
        if (static_cast<uint64_t>(s.count - 1) * S >= s.size) return Par::Invalid;
        if (static_cast<uint64_t>(s.count) * S < s.size) return Par::Invalid;
        if (s.P != 0 && s.P != S) return Par::Invalid;
        if (s.have_last &&
            (s.last_len > S || s.size - s.last_len != static_cast<uint64_t>(s.count - 1) * S))
          return Par::Invalid;
        s.P = static_cast<uint32_t>(S);
      }
      s.fec_m = h.fec_total;
      s.fec_s = static_cast<uint32_t>(len);
      s.lay = L;
      s.parity_recv = 0;
      s.pbuf.assign(static_cast<size_t>(s.fec_m) * len, 0);
      s.pgot.assign(s.fec_m, 0);
    } else if (s.fec_m != h.fec_total || s.fec_s != len) {
      return Par::Invalid;
    }
    if (s.pgot[h.frag_idx]) return Par::Dup;
    std::memcpy(s.pbuf.data() + static_cast<size_t>(h.frag_idx) * len, p, len);
    s.pgot[h.frag_idx] = 1;
    ++s.parity_recv;
    return Par::Ok;
  }

  size_t shardLen(const Slot& s, size_t g) const {
    if (s.count == 1) return s.size;
    if (g + 1 < s.count) return s.fec_s;
    return s.size - static_cast<size_t>(s.count - 1) * s.fec_s;
  }

  void recover(Slot& s) {
    const size_t S = s.fec_s, W = (S + 7) / 8;
    for (unsigned b = 0; b < s.lay.nblocks; ++b) {
      const FecBlock& B = s.lay.block[b];
      std::vector<uint8_t> hd(B.data_count), hp(B.parity_count);
      size_t miss = 0, avail = 0;
      for (size_t i = 0; i < B.data_count; ++i) {
        hd[i] = s.got[B.data_first + i];
        miss += hd[i] ? 0 : 1;
      }
      for (size_t j = 0; j < B.parity_count; ++j) {
        hp[j] = s.pgot[B.parity_first + j];
        avail += hp[j] ? 1 : 0;
      }
      if (miss == 0 || avail < miss) continue;
      fecimpl::recoverBlock(
          B.data_count, B.parity_count, W, hd, hp,
          [&](size_t i, uint64_t* dst) {
            size_t g = B.data_first + i, n = shardLen(s, g);
            std::memset(dst, 0, W * sizeof(uint64_t));
            std::memcpy(dst, s.buf.data() + g * S, n);
          },
          [&](size_t j, uint64_t* dst) {
            std::memset(dst, 0, W * sizeof(uint64_t));
            std::memcpy(dst, s.pbuf.data() + (B.parity_first + j) * S, S);
          },
          [&](size_t i, const uint64_t* w) {
            size_t g = B.data_first + i, n = shardLen(s, g);
            std::memcpy(s.buf.data() + g * S, w, n);
            s.got[g] = 1;
            ++s.received;
            ++s.rec;
            ++c.fragments_recovered;
          });
    }
  }

  bool push(const UdpHeader& h, const uint8_t* payload, size_t len, Frame& out) {
    const bool isFec = h.type == UdpType::Fec;
    if (h.type != UdpType::Video && !isFec) { ++c.fragments_dropped; return false; }
    // For Fec datagrams frag_idx is a PARITY index (< fec_total), not a fragment index.
    if (h.frag_count == 0 || h.frame_size > kMaxTcpPayload ||
        (h.frag_count > 1 && h.frag_count > h.frame_size) || len > kUdpMaxDatagram * 64u ||
        (len && !payload) ||
        (!isFec && h.frag_idx >= h.frag_count) ||
        (isFec && (h.fec_total == 0 || h.frag_idx >= h.fec_total || len == 0 ||
                   len > kUdpMaxPayload || h.frame_size == 0))) {
      ++c.fragments_dropped;
      return false;
    }
    if (have_epoch && static_cast<int16_t>(static_cast<uint16_t>(h.epoch - epoch)) < 0) {
      // Straggler from a previous encoder configuration: must not reset the new epoch.
      if (isFec) ++c.fec_unused; else ++c.fragments_dropped;
      return false;
    }
    if (!have_epoch || h.epoch != epoch) {
      clearState();
      have_epoch = true;
      epoch = h.epoch;
    }
    if (have_floor && !seqNewer(h.frame_seq, floor_seq)) {
      if (isFec) ++c.fec_unused; else ++c.fragments_dropped;
      return false;
    }

    bool fresh = false;
    Slot* s = nullptr;
    Slot* free_slot = nullptr;
    Slot* oldest = nullptr;
    for (auto& x : slots) {
      if (!x.used) { if (!free_slot) free_slot = &x; continue; }
      if (x.seq == h.frame_seq) { s = &x; break; }
      if (!oldest || seqNewer(oldest->seq, x.seq)) oldest = &x;
    }
    if (!s) {
      if (!free_slot) {
        // Too many frames in flight: evict the oldest (unless the newcomer is older still).
        if (seqNewer(oldest->seq, h.frame_seq)) {
          if (isFec) ++c.fec_unused; else ++c.fragments_dropped;
          return false;
        }
        raiseFloor(oldest->seq);
        oldest->used = false;
        waiting = true;
        need = true;
        free_slot = oldest;
      }
      s = free_slot;
      s->used = true; s->seq = h.frame_seq; s->count = h.frag_count; s->size = h.frame_size;
      s->P = 0; s->have_last = false; s->last_len = 0; s->received = 0; s->rec = 0;
      s->fec_m = 0; s->fec_s = 0; s->parity_recv = 0;
      s->flags = h.flags; s->ts = h.capture_ts_us;
      s->buf.resize(h.frame_size);
      s->got.assign(h.frag_count, 0);
      fresh = true;
    } else if (s->count != h.frag_count || s->size != h.frame_size) {
      ++c.fragments_dropped;
      return false;
    }
    if (isFec) {
      Par r = acceptParity(*s, h, payload, len);
      if (r != Par::Ok) {
        if (fresh) s->used = false;
        if (r == Par::Dup) ++c.fec_unused; else ++c.fragments_dropped;
        return false;
      }
      ++c.fec_received;
    } else {
      if (s->got[h.frag_idx]) { ++c.fragments_dropped; return false; }  // duplicate
      if (!place(*s, h.frag_idx, payload, len)) {
        if (fresh) s->used = false;  // do not let a bogus first fragment occupy/poison a slot
        ++c.fragments_dropped;
        return false;
      }
      s->got[h.frag_idx] = 1;
      ++s->received;
      ++c.fragments_received;
    }
    if (s->fec_m && s->received < s->count &&
        static_cast<size_t>(s->received) + s->parity_recv >= s->count)
      recover(*s);
    if (s->received != s->count) return false;

    // ---- frame complete
    uint32_t seq = s->seq;
    // Abandon all older incomplete frames (they are counted via the seq gap below).
    for (auto& x : slots)
      if (x.used && x.seq != seq && seqNewer(seq, x.seq)) x.used = false;
    bool gap = false;
    if (have_last_complete) {
      uint32_t missing = seq - last_complete - 1u;
      if (missing) { c.frames_lost += missing; gap = true; }
    }
    have_last_complete = true;
    last_complete = seq;
    raiseFloor(seq);
    if (gap) { waiting = true; need = true; }

    bool key = (s->flags & kFrameKey) != 0;
    if (waiting && !key) {
      need = true;  // keep asking; the caller rate-limits
      s->used = false;
      return false;
    }
    if (key) { waiting = false; need = false; }
    out.frame_seq = seq; out.epoch = epoch; out.flags = s->flags; out.capture_ts_us = s->ts;
    out.data.swap(s->buf);
    if (s->rec) ++c.frames_recovered;
    s->used = false;
    ++c.frames_complete;
    return true;
  }
};

FrameAssembler::FrameAssembler(size_t max_pending) : impl_(new Impl(max_pending)) {}
FrameAssembler::~FrameAssembler() = default;
bool FrameAssembler::push(const UdpHeader& h, const uint8_t* payload, size_t len, Frame& out) {
  return impl_->push(h, payload, len, out);
}
bool FrameAssembler::needKeyframe() const { return impl_->need; }
void FrameAssembler::clearNeedKeyframe() { impl_->need = false; }
const FrameAssembler::Counters& FrameAssembler::counters() const { return impl_->c; }
void FrameAssembler::reset() {
  impl_->clearState();
  impl_->have_epoch = false;
  impl_->c = Counters();
}

// ----------------------------------------------------------------- strings
const char* toString(MsgType t) {
  switch (t) {
    case MsgType::Hello: return "Hello";
    case MsgType::HelloAck: return "HelloAck";
    case MsgType::Capabilities: return "Capabilities";
    case MsgType::SetParam: return "SetParam";
    case MsgType::ParamState: return "ParamState";
    case MsgType::Command: return "Command";
    case MsgType::VideoFormat: return "VideoFormat";
    case MsgType::VideoFrame: return "VideoFrame";
    case MsgType::Ping: return "Ping";
    case MsgType::Pong: return "Pong";
    case MsgType::Stats: return "Stats";
    case MsgType::ReceiverReport: return "ReceiverReport";
    case MsgType::Log: return "Log";
    case MsgType::Bye: return "Bye";
  }
  return "Unknown";
}

const char* toString(ParamId id) {
  switch (id) {
    case ParamId::CameraIndex: return "CameraIndex";
    case ParamId::VideoWidth: return "VideoWidth";
    case ParamId::VideoHeight: return "VideoHeight";
    case ParamId::Fps: return "Fps";
    case ParamId::VideoCodec: return "VideoCodec";
    case ParamId::Bitrate: return "Bitrate";
    case ParamId::KeyframeIntervalMs: return "KeyframeIntervalMs";
    case ParamId::BitrateMode: return "BitrateMode";
    case ParamId::IntraRefreshFrames: return "IntraRefreshFrames";
    case ParamId::FecPercent: return "FecPercent";
    case ParamId::AdaptiveBitrate: return "AdaptiveBitrate";
    case ParamId::EncoderProfile: return "EncoderProfile";
    case ParamId::MaxSendQueueFrames: return "MaxSendQueueFrames";
    case ParamId::BFrames: return "BFrames";
    case ParamId::QpMin: return "QpMin";
    case ParamId::QpMax: return "QpMax";
    case ParamId::LatencyDropPolicy: return "LatencyDropPolicy";
    case ParamId::HighSpeedMode: return "HighSpeedMode";
    case ParamId::AfMode: return "AfMode";
    case ParamId::FocusDistance: return "FocusDistance";
    case ParamId::AeMode: return "AeMode";
    case ParamId::AeLock: return "AeLock";
    case ParamId::ExposureCompensation: return "ExposureCompensation";
    case ParamId::Iso: return "Iso";
    case ParamId::ExposureTimeNs: return "ExposureTimeNs";
    case ParamId::AwbMode: return "AwbMode";
    case ParamId::AwbLock: return "AwbLock";
    case ParamId::Zoom: return "Zoom";
    case ParamId::Torch: return "Torch";
    case ParamId::AntiBanding: return "AntiBanding";
    case ParamId::VideoStabilization: return "VideoStabilization";
    case ParamId::OpticalStabilization: return "OpticalStabilization";
    case ParamId::NoiseReduction: return "NoiseReduction";
    case ParamId::EdgeMode: return "EdgeMode";
    case ParamId::ScreenMode: return "ScreenMode";
  }
  return "Unknown";
}

}  // namespace owc
