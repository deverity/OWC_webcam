// OWC wire protocol v1 — single source of truth shared by the Android NDK core
// and the Windows client. Portable C++17, no platform headers.
//
// All multi-byte integers are LITTLE-ENDIAN on the wire. Floats are IEEE-754
// binary32 LE. Strings are u16 byte-length + UTF-8 bytes (no terminator).
// Arrays are u16 count + elements. Booleans are u8 (0/1).
//
// Topology: the PHONE is the server.
//   TCP  kDefaultControlPort   control channel (+ video when transport == Tcp)
//   UDP  kDefaultVideoUdpPort  phone listens for Punch; sends video fragments
//                              back to the punch source address (firewall friendly)
//   UDP  kDefaultDiscoveryPort phone answers DiscoveryRequest broadcasts
//   USB  = `adb forward tcp:<local> tcp:47800`, then plain TCP to 127.0.0.1
//         (UDP is not possible over adb; USB always uses Tcp transport).
//
// Session flow:
//   PC  -> Hello            (magic, version, desired transport)
//   PH  -> HelloAck         (status, session_id, device info, codec mask)
//   PH  -> Capabilities     (cameras + param descriptors of active camera)
//   PH  -> ParamState*      (optional, current values)
//   [Udp only] PC sends UdpType::Punch {session_id} every 1 s to phone UDP port;
//              phone replies PunchAck and starts sending fragments to that addr.
//   PC  -> Command{StartStream}
//   PH  -> VideoFormat      (on every encoder (re)configure, epoch++)
//   PH  -> VideoFrame*      (TCP) or UDP fragments
//   both: Ping/Pong any time (PC pings 2 Hz for RTT + clock offset)
//   PH  -> Stats            (1 Hz)
//   either -> Bye, then close.
//
// Latency policy (phone side): never queue more than ~2 frames. When the send
// queue overflows, drop everything up to the next keyframe and request an IDR.
// PC side: decode immediately, present newest frame, never buffer for smoothness.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace owc {

// ---------------------------------------------------------------- constants
constexpr uint32_t kMagic = 0x3143574Fu;            // "OWC1" read as LE u32
constexpr uint16_t kProtocolVersion = 1;
constexpr uint16_t kDefaultControlPort = 47800;
constexpr uint16_t kDefaultVideoUdpPort = 47801;
constexpr uint16_t kDefaultDiscoveryPort = 47802;
constexpr uint32_t kMaxTcpPayload = 16u * 1024u * 1024u;  // reject larger frames
constexpr size_t kUdpMaxDatagram = 1400;                   // header + payload
constexpr uint16_t kUdpMagic = 0x574Fu;                    // bytes 'O','W'
constexpr uint8_t kUdpVersion = 1;

// ---------------------------------------------------------------- TCP framing
// Every TCP message: TcpHeader (8 bytes) followed by `length` payload bytes.
//   u16 type (MsgType) | u16 flags (reserved, 0) | u32 length
constexpr size_t kTcpHeaderSize = 8;

enum class MsgType : uint16_t {
  Hello = 1,         // PC -> phone
  HelloAck = 2,      // phone -> PC
  Capabilities = 3,  // phone -> PC (resent after camera change)
  SetParam = 10,     // PC -> phone
  ParamState = 11,   // phone -> PC (reply to SetParam, or unsolicited change)
  Command = 12,      // PC -> phone
  VideoFormat = 20,  // phone -> PC
  VideoFrame = 21,   // phone -> PC (Tcp transport only)
  Ping = 30,         // PC -> phone
  Pong = 31,         // phone -> PC
  Stats = 32,        // phone -> PC
  ReceiverReport = 33,  // PC -> phone (≈4 Hz) feedback for adaptive bitrate
  Log = 40,         // phone -> PC (level + text, for the PC log panel)
  Bye = 50,          // either
};

enum class VideoTransport : uint8_t { Tcp = 0, Udp = 1 };
enum class Codec : uint8_t { H264 = 0, Hevc = 1 };
constexpr uint32_t codecBit(Codec c) { return 1u << static_cast<uint8_t>(c); }

// ---------------------------------------------------------------- messages
// Wire layouts (all LE; "str" = u16 len + UTF-8 bytes, truncated to 65535 on
// encode; "arr" = u16 count). Decoders ignore trailing bytes (forward compat)
// and return false on truncation. Hello/HelloAck decode do NOT validate
// magic/version (the receiver decides).
//
// Hello:    u32 magic, u16 version, u8 transport, str client_name
struct Hello {
  uint32_t magic = kMagic;
  uint16_t version = kProtocolVersion;
  VideoTransport transport = VideoTransport::Tcp;
  std::string client_name;
};

enum class HelloStatus : uint8_t { Ok = 0, Busy = 1, VersionMismatch = 2, BadMagic = 3 };

// HelloAck: u32 magic, u16 version, u8 status, u32 session_id, u16 udp_port,
//           str device_name, str model, u16 android_api, u32 codec_mask
struct HelloAck {
  uint32_t magic = kMagic;
  uint16_t version = kProtocolVersion;
  HelloStatus status = HelloStatus::Ok;
  uint32_t session_id = 0;     // random, nonzero; used in UDP headers
  uint16_t udp_port = kDefaultVideoUdpPort;
  std::string device_name;     // user-editable name on phone
  std::string model;           // Build.MODEL
  uint16_t android_api = 0;
  uint32_t codec_mask = 0;     // codecBit(...) of supported HW encoders
};

enum class Facing : uint8_t { Back = 0, Front = 1, External = 2 };

struct VideoSize {
  uint16_t width = 0;
  uint16_t height = 0;
  uint16_t max_fps = 0;        // derived from min frame duration
};

// Wire: str id, u8 facing, u8 hw_level, i32 sensor_orientation, f32 max_zoom,
//       u8 has_flash, arr<VideoSize{u16 width, u16 height, u16 max_fps}> sizes,
//       arr<{u16 lo, u16 hi}> fps_ranges
struct CameraInfo {
  std::string id;              // ACamera id string
  Facing facing = Facing::Back;
  uint8_t hw_level = 0;        // ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL value
  int32_t sensor_orientation = 0;  // degrees, 0/90/180/270
  float max_zoom = 1.0f;
  bool has_flash = false;
  std::vector<VideoSize> sizes;               // PRIVATE-format output sizes
  std::vector<std::pair<uint16_t, uint16_t>> fps_ranges;  // AE target ranges
};

enum class ValueType : uint8_t { Bool = 0, Int32 = 1, Int64 = 2, Float = 3, Enum = 4 };

// Wire: u8 type, then Bool->u8, Int32/Enum->i32, Int64->i64, Float->f32.
struct ParamValue {
  ValueType type = ValueType::Int32;
  int64_t i = 0;   // Bool/Int32/Int64/Enum
  float f = 0.0f;  // Float

  static ParamValue Bool(bool v) { return {ValueType::Bool, v ? 1 : 0, 0.0f}; }
  static ParamValue Int(int32_t v) { return {ValueType::Int32, v, 0.0f}; }
  static ParamValue Long(int64_t v) { return {ValueType::Int64, v, 0.0f}; }
  static ParamValue Real(float v) { return {ValueType::Float, 0, v}; }
  static ParamValue EnumV(int32_t v) { return {ValueType::Enum, v, 0.0f}; }
  bool operator==(const ParamValue& o) const {
    return type == o.type && (type == ValueType::Float ? f == o.f : i == o.i);
  }
};

// Every remotely controllable knob. IDs are stable wire values — append only.
enum class ParamId : uint16_t {
  // stream (changing any of these reconfigures camera/encoder; epoch++)
  CameraIndex = 1,          // Int32, index into Capabilities::cameras
  VideoWidth = 2,           // Int32
  VideoHeight = 3,          // Int32
  Fps = 4,                  // Int32
  VideoCodec = 5,           // Enum Codec
  Bitrate = 6,              // Int32 bps (applied live, no reconfigure)
  KeyframeIntervalMs = 7,   // Int32 (0 = only on request)
  BitrateMode = 8,          // Enum 0=CBR 1=VBR 2=CQ
  IntraRefreshFrames = 9,   // Int32 0=off, else KEY_INTRA_REFRESH_PERIOD (smooth loss recovery)
  FecPercent = 10,          // Int32 -1..100 redundancy for UDP (0=off, -1=auto/adaptive)
  AdaptiveBitrate = 11,     // Bool: phone adapts bitrate from ReceiverReport/queue (Bitrate = ceiling)
  EncoderProfile = 12,      // Enum 0=Baseline 1=Main 2=High (HEVC: 0/1=Main 2=Main10 if supported)
  MaxSendQueueFrames = 13,  // Int32 1..60 (default 2). Larger = smoother on bad links, more delay
  BFrames = 14,             // Int32 0..4 (default 0, "max-bframes"; needs Main/High; adds delay)
  QpMin = 15,               // Int32 0=auto ("video-qp-min"/vendor keys when supported)
  QpMax = 16,               // Int32 0=auto
  LatencyDropPolicy = 17,   // Enum 0=drop-to-keyframe on overflow (latency) 1=block/queue (quality)
  HighSpeedMode = 18,       // Enum 0=normal session 1=constrained high-speed (e.g. 120 fps; experimental,
                            //   lower ISP latency, limited manual controls). Changes Fps/size ranges.
  // camera controls (applied live on the repeating request)
  AfMode = 20,              // Enum 0=off(manual) 1=continuous-video 2=continuous-picture 3=auto
  FocusDistance = 21,       // Float diopters, 0..LENS_INFO_MINIMUM_FOCUS_DISTANCE (AfMode=0)
  AeMode = 22,              // Enum 0=off(manual ISO/exposure) 1=on
  AeLock = 23,              // Bool
  ExposureCompensation = 24,// Int32 steps (CONTROL_AE_COMPENSATION_RANGE)
  Iso = 25,                 // Int32 (SENSOR_SENSITIVITY, AeMode=0)
  ExposureTimeNs = 26,      // Int64 (SENSOR_EXPOSURE_TIME, AeMode=0)
  AwbMode = 27,             // Enum ACAMERA_CONTROL_AWB_MODE_* values
  AwbLock = 28,             // Bool
  Zoom = 29,                // Float 1.0..max_zoom (crop region)
  Torch = 30,               // Bool
  AntiBanding = 31,         // Enum 0=off 1=50Hz 2=60Hz 3=auto
  VideoStabilization = 32,  // Bool (EIS)
  OpticalStabilization = 33,// Bool (OIS)
  NoiseReduction = 34,      // Enum ACAMERA_NOISE_REDUCTION_MODE_*
  EdgeMode = 35,            // Enum ACAMERA_EDGE_MODE_*
  // device
  ScreenMode = 60,          // Enum 0=normal 1=black(min brightness, OLED ~off)
};

enum ParamFlags : uint8_t {
  kParamSupported = 1 << 0,
  kParamReadOnly = 1 << 1,
  kParamReconfigures = 1 << 2,  // causes stream restart (new epoch)
};

struct EnumOption {
  int32_t value = 0;
  std::string label;
};

// Wire: u16 id, u8 type, u8 flags, f64 min, f64 max, f64 step,
//       ParamValue current, array<EnumOption{i32 value, string label}>
struct ParamDesc {
  ParamId id = ParamId::CameraIndex;
  ValueType type = ValueType::Int32;
  uint8_t flags = 0;
  double min = 0, max = 0, step = 0;
  ParamValue current;
  std::vector<EnumOption> options;  // only for Enum
};

// Wire: u16 active_camera, arr<CameraInfo> cameras, arr<ParamDesc> params
struct Capabilities {
  uint16_t active_camera = 0;
  std::vector<CameraInfo> cameras;
  std::vector<ParamDesc> params;  // for active camera
};

// Wire: u16 id, ParamValue value
struct SetParam {
  ParamId id = ParamId::CameraIndex;
  ParamValue value;
};

enum class ParamStatus : uint8_t { Ok = 0, Unsupported = 1, OutOfRange = 2, Failed = 3 };

// Wire: u16 id, u8 status, ParamValue value
struct ParamState {
  ParamId id = ParamId::CameraIndex;
  ParamStatus status = ParamStatus::Ok;
  ParamValue value;  // value now in effect
};

enum class CommandId : uint16_t {
  StartStream = 1,
  StopStream = 2,
  RequestKeyframe = 3,
  FocusAt = 4,       // x,y normalized 0..1 in sensor-oriented frame (tap to focus)
  ResetParams = 5,   // back to camera defaults
  SetDeviceName = 6, // text
};

// Wire: u16 cmd, i64 arg, f32 x, f32 y, string text
struct Command {
  CommandId cmd = CommandId::StartStream;
  int64_t arg = 0;
  float x = 0, y = 0;
  std::string text;
};

// Wire: u32 epoch, u8 codec, u16 width, u16 height, u16 fps,
//       i16 rotation_deg, u8 mirror, bytes(u32 len) csd
struct VideoFormat {
  uint32_t epoch = 0;          // increments on every encoder (re)configure
  Codec codec = Codec::H264;
  uint16_t width = 0, height = 0, fps = 0;
  int16_t rotation_deg = 0;    // clockwise rotation PC should apply for upright
  bool mirror = false;         // front camera hint
  std::vector<uint8_t> csd;    // Annex-B SPS/PPS (+VPS for HEVC)
};

// kFrameConfig = the AU CONTAINS codec config (SPS/PPS/VPS). It may be config
// only, or an IDR with the parameter sets prepended (the Android encoder sends
// kFrameKey|kFrameConfig for those): receivers must refresh their csd from it
// AND decode its picture NALs if present.
enum FrameFlags : uint8_t { kFrameKey = 1 << 0, kFrameConfig = 1 << 1 };

// TCP VideoFrame wire: u32 epoch, u32 frame_seq, u64 capture_ts_us, u8 flags,
//                      then the rest of the payload is the Annex-B access unit.
struct VideoFrameHeader {
  uint32_t epoch = 0;
  uint32_t frame_seq = 0;
  uint64_t capture_ts_us = 0;  // phone CLOCK_MONOTONIC (same clock as Pong)
  uint8_t flags = 0;
};
constexpr size_t kVideoFrameHeaderSize = 17;

// Ping wire: u64 t0_us.  Pong wire: u64 t0_us, u64 phone_us.
struct Ping { uint64_t t0_us = 0; };                       // PC clock
struct Pong { uint64_t t0_us = 0; uint64_t phone_us = 0; };  // phone CLOCK_MONOTONIC

// Wire: f32 camera_fps, f32 encode_fps, f32 send_fps, u32 bitrate_bps,
//       u32 dropped_frames, u32 send_queue, f32 encode_latency_ms,
//       i8 battery_pct, f32 battery_temp_c, u8 charging, u8 thermal_status,
//       [v1.1 trailer, optional on decode] u32 target_bitrate_bps,
//       f32 isp_latency_ms, i8 fec_percent
struct Stats {
  float camera_fps = 0, encode_fps = 0, send_fps = 0;
  uint32_t bitrate_bps = 0;        // measured output
  uint32_t dropped_frames = 0;     // cumulative, latency-policy drops
  uint32_t send_queue = 0;         // frames currently queued
  float encode_latency_ms = 0;     // capture timestamp -> encoder output
  int8_t battery_pct = -1;
  float battery_temp_c = 0;
  bool charging = false;
  uint8_t thermal_status = 0;      // PowerManager thermal status 0..6
  uint32_t target_bitrate_bps = 0; // current encoder target (ABR output)
  float isp_latency_ms = 0;        // sensor timestamp -> frame handed to encoder
  int8_t fec_percent = 0;          // FEC % currently applied (0 = off)
};

// PC -> phone feedback (WebRTC-RR-like). Counters are cumulative since the
// session started; the phone diffs consecutive reports.
// Wire: u32 x4, f32 jitter_ms, f32 delay_gradient_ms, u32 recv_bitrate_bps,
//       f32 decode_ms
struct ReceiverReport {
  uint32_t frames_received = 0, frames_lost = 0;
  uint32_t fragments_received = 0, fragments_lost = 0;
  float jitter_ms = 0;          // inter-arrival jitter (RFC 3550 style)
  float delay_gradient_ms = 0;  // trend of one-way delay; >0 = queues building up
  uint32_t recv_bitrate_bps = 0;
  float decode_ms = 0;          // avg PC decode time
};

enum class LogLevel : uint8_t { Debug = 0, Info = 1, Warn = 2, Error = 3 };
// Wire: u8 level, str text
struct LogMsg { LogLevel level = LogLevel::Info; std::string text; };

// ---------------------------------------------------------------- UDP
// Fec = Reed-Solomon parity datagram, see the FEC section below.
enum class UdpType : uint8_t { Video = 1, Punch = 2, PunchAck = 3, Fec = 4 };

// Fixed 32-byte header on every video/punch datagram:
//   u16 magic | u8 version | u8 type | u32 session_id | u32 frame_seq |
//   u16 frag_idx | u16 frag_count | u32 frame_size | u64 capture_ts_us |
//   u16 epoch (low 16 bits) | u8 flags (FrameFlags) | u8 fec_total
// fec_total: on Fec datagrams = total parity shards M of the frame (1..255); on
// Video datagrams it is informational (0 or M); receivers ignore it there.
// Punch/PunchAck: only magic..session_id meaningful, rest zero, no payload.
struct UdpHeader {
  UdpType type = UdpType::Video;
  uint32_t session_id = 0;
  uint32_t frame_seq = 0;
  uint16_t frag_idx = 0, frag_count = 0;
  uint32_t frame_size = 0;
  uint64_t capture_ts_us = 0;
  uint16_t epoch = 0;
  uint8_t flags = 0;
  uint8_t fec_total = 0;  // wire: the former reserved byte (offset 31)
};
constexpr size_t kUdpHeaderSize = 32;
constexpr size_t kUdpMaxPayload = kUdpMaxDatagram - kUdpHeaderSize;

// ---------------------------------------------------------------- discovery
// PC broadcasts DiscoveryRequest to 255.255.255.255:47802 (and subnet bcasts).
// Phone unicasts DiscoveryReply to the source address.
// Request wire: u32 magic, u16 version, u8 kind=1.
// Reply wire:   u32 magic, u16 version, u8 kind=2, u16 control_port,
//               u16 udp_port, string device_name, string model, u8 busy
struct DiscoveryReply {
  uint16_t control_port = kDefaultControlPort;
  uint16_t udp_port = kDefaultVideoUdpPort;
  std::string device_name, model;
  bool busy = false;
};

// ================================================================ codec API
// Every encode* returns the PAYLOAD only (no TCP header). Every decode*
// returns false on truncated/malformed input and never reads out of bounds.
std::vector<uint8_t> encode(const Hello&);
std::vector<uint8_t> encode(const HelloAck&);
std::vector<uint8_t> encode(const Capabilities&);
std::vector<uint8_t> encode(const SetParam&);
std::vector<uint8_t> encode(const ParamState&);
std::vector<uint8_t> encode(const Command&);
std::vector<uint8_t> encode(const VideoFormat&);
std::vector<uint8_t> encode(const Ping&);
std::vector<uint8_t> encode(const Pong&);
std::vector<uint8_t> encode(const Stats&);
std::vector<uint8_t> encode(const LogMsg&);
std::vector<uint8_t> encode(const ReceiverReport&);

bool decode(const uint8_t* p, size_t n, Hello&);
bool decode(const uint8_t* p, size_t n, HelloAck&);
bool decode(const uint8_t* p, size_t n, Capabilities&);
bool decode(const uint8_t* p, size_t n, SetParam&);
bool decode(const uint8_t* p, size_t n, ParamState&);
bool decode(const uint8_t* p, size_t n, Command&);
bool decode(const uint8_t* p, size_t n, VideoFormat&);
bool decode(const uint8_t* p, size_t n, Ping&);
bool decode(const uint8_t* p, size_t n, Pong&);
bool decode(const uint8_t* p, size_t n, Stats&);
bool decode(const uint8_t* p, size_t n, LogMsg&);
bool decode(const uint8_t* p, size_t n, ReceiverReport&);

// VideoFrame: header + access unit, written into `out` (cleared first).
void encodeVideoFrame(const VideoFrameHeader&, const uint8_t* au, size_t au_len,
                      std::vector<uint8_t>& out);
// On success `au`/`au_len` point INTO p (no copy).
bool decodeVideoFrame(const uint8_t* p, size_t n, VideoFrameHeader&,
                      const uint8_t*& au, size_t& au_len);

// Discovery
std::vector<uint8_t> encodeDiscoveryRequest();
bool isDiscoveryRequest(const uint8_t* p, size_t n);
std::vector<uint8_t> encode(const DiscoveryReply&);
bool decode(const uint8_t* p, size_t n, DiscoveryReply&);

// --------------------------------------------------------------- TCP framing
// Appends header+payload to `out` (does not clear).
void appendTcpMessage(std::vector<uint8_t>& out, MsgType type,
                      const uint8_t* payload, size_t len);
inline void appendTcpMessage(std::vector<uint8_t>& out, MsgType type,
                             const std::vector<uint8_t>& payload) {
  appendTcpMessage(out, type, payload.data(), payload.size());
}
void writeTcpHeader(uint8_t out[kTcpHeaderSize], MsgType type, uint32_t len);

// Incremental parser for a TCP byte stream. Feed arbitrary chunks; pull whole
// messages. Unknown types are still returned (caller ignores them).
class TcpFrameParser {
 public:
  struct Message {
    MsgType type;
    std::vector<uint8_t> payload;
  };
  // Returns false (and enters error state permanently) if a header announces
  // length > kMaxTcpPayload.
  bool feed(const uint8_t* data, size_t len);
  // Pops the next complete message, if any.
  bool next(Message& out);
  bool error() const { return error_; }
  size_t buffered() const { return buf_.size() - pos_; }

 private:
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  bool error_ = false;
};

// --------------------------------------------------------------- UDP
void writeUdpHeader(const UdpHeader&, uint8_t out[kUdpHeaderSize]);
bool readUdpHeader(const uint8_t* p, size_t n, UdpHeader&);  // checks magic+version

// Splits one access unit into datagrams of at most kUdpMaxDatagram bytes
// (header included). Calls sink(datagram, len) for each, in order. frag_count
// is ceil(au_len / max_payload) (min 1). Returns datagram count.
template <class Sink>
size_t packetize(const UdpHeader& base, const uint8_t* au, size_t au_len, Sink&& sink,
                 size_t max_payload = kUdpMaxPayload);
// (max_payload clamped to [1,kUdpMaxPayload]; more than 65535 fragments -> returns 0.)

// ------------------------------------------------------------------ FEC
// Per-frame systematic Reed-Solomon over GF(2^8) (poly 0x11d, generator 2) with a
// Cauchy parity matrix: parity[j] = XOR_i gfmul(inv(x_j ^ y_i), data[i]),
// x_j = j, y_i = m + i (j,i local to the FEC block; m = parity shards of the block).
// Any k of the (k+m) shards of a block reconstruct it. Requires k+m <= 255.
//
// Shards: data shard i = Video fragment i's payload, zero-padded to the shard size S.
//   S = max_payload used for fragmentation (kUdpMaxPayload by default), except for
//   single-fragment frames where S = au_len. Every Fec datagram payload is exactly
//   S bytes. Frames with au_len == 0 carry no FEC.
// Fec datagram header: type=Fec, frame_seq/epoch/capture_ts_us/frame_size/flags/
//   session_id copied from the frame, frag_count = number of DATA fragments,
//   frag_idx = global parity index p (0..M-1), fec_total = M.
// Block layout (derived identically by sender and receiver, see fecLayout): the
//   frame is split into nblocks (1..8) blocks; block b covers data fragments
//   [data_first, data_first+data_count) and parity indices
//   [parity_first, parity_first+parity_count). Data and parity are spread as evenly
//   as possible (first `count % n` blocks get one extra).
constexpr unsigned kFecMaxBlocks = 8;
struct FecBlock {
  uint16_t data_first = 0, data_count = 0;
  uint16_t parity_first = 0, parity_count = 0;
};
struct FecLayout {
  uint8_t nblocks = 0;  // 0 = FEC not possible for these parameters
  FecBlock block[kFecMaxBlocks];
};
// Smallest nblocks in 1..min(8,parity_total,frag_count) such that for every block
// data_count + parity_count <= 255; nblocks = 0 if none (or frag_count==0 or
// parity_total==0).
FecLayout fecLayout(uint16_t frag_count, uint8_t parity_total);
// M = max(2, ceil(frag_count * percent / 100)), capped at 255; 0 if percent <= 0.
uint8_t fecParityCount(uint16_t frag_count, int percent);
// Shard size used for a frame (0 when au_len == 0).
size_t fecShardSize(size_t au_len, size_t max_payload = kUdpMaxPayload);
// Computes all parity shards: `parity` is resized to M*S, shard p at p*S. Returns
// false (parity cleared) if FEC is not possible for the frame.
bool fecEncode(const uint8_t* au, size_t au_len, uint8_t parity_total,
               std::vector<uint8_t>& parity, size_t max_payload = kUdpMaxPayload);
// Emits the Fec datagrams for one AU (call after/before packetize with the same
// base/au/max_payload). Returns the number of datagrams (0 = FEC off for the frame).
template <class Sink>
size_t packetizeFec(const UdpHeader& base, const uint8_t* au, size_t au_len,
                    uint8_t parity_total, Sink&& sink, size_t max_payload = kUdpMaxPayload);

// ------------------------------------------------------------- SPS patching
// Low-latency SPS rewriting (see docs/LOW_LATENCY_RESEARCH.md 5.3). Inputs are NAL
// units WITHOUT start code, including the NAL header (1 byte H.264, 2 bytes HEVC).
// Output is a complete NAL (header + escaped RBSP).
//  H.264: sets vui_parameters_present_flag=1, bitstream_restriction_flag=1,
//    max_num_reorder_frames=0, max_dec_frame_buffering=max(1,num_ref_frames). All
//    other SPS/VUI fields (timing, colour, HRD, ...) are preserved bit-exactly;
//    num_ref_frames is never changed. If a bitstream_restriction already exists its
//    mv/denom fields are kept; otherwise motion_vectors_over_pic_boundaries=1,
//    max_bytes_per_pic_denom=0, max_bits_per_mb_denom=0, log2_max_mv_length_*=15.
//  HEVC: sps_max_num_reorder_pics[i]=0, sps_max_latency_increase_plus1[i]=0 for all
//    sub-layers; everything else preserved.
//  has_bframes=true => stream signals B-frames/reordering: nothing is changed (out =
//    copy of input, returns true).
// Returns false (out untouched) when the SPS cannot be parsed/is unsupported.
bool patchH264Sps(const uint8_t* nal, size_t len, std::vector<uint8_t>& out,
                  bool has_bframes = false);
bool patchHevcSps(const uint8_t* nal, size_t len, std::vector<uint8_t>& out,
                  bool has_bframes = false);
// Walks an Annex-B buffer (3/4-byte start codes), copies it to `out` (cleared first)
// and replaces every SPS NAL (H.264 type 7 / HEVC type 33) with its patched form.
// SPS that fail to parse are copied unchanged. Returns the number of SPS patched.
size_t patchAnnexBSps(const uint8_t* data, size_t len, Codec codec, std::vector<uint8_t>& out,
                      bool has_bframes = false);

struct SpsInfo {
  uint8_t profile_idc = 0, level_idc = 0;
  uint32_t width = 0, height = 0;          // after cropping (H.264 only; 0 for HEVC)
  uint32_t num_ref_frames = 0;             // H.264 max_num_ref_frames
  uint32_t pic_order_cnt_type = 0;         // H.264
  bool vui_present = false;
  bool bitstream_restriction = false;      // H.264 VUI flag
  uint32_t max_num_reorder_frames = 0;     // H.264 VUI / HEVC sps_max_num_reorder_pics[highest]
  uint32_t max_dec_frame_buffering = 0;    // H.264 VUI / HEVC sps_max_dec_pic_buffering_minus1+1
  uint32_t max_latency_increase_plus1 = 0; // HEVC
  // H.264 VUI video_signal_type (HEVC: VUI not parsed, fields stay default).
  bool video_signal_type_present = false;
  bool video_full_range = false;           // video_full_range_flag (0 = limited 16..235)
  bool colour_description_present = false;
  uint8_t colour_primaries = 2;            // 2 = unspecified (1 = BT.709, 5/6 = BT.601)
  uint8_t transfer_characteristics = 2;
  uint8_t matrix_coefficients = 2;         // 1 = BT.709, 5/6 = BT.601, 2 = unspecified
};
bool parseH264Sps(const uint8_t* nal, size_t len, SpsInfo& info);
bool parseHevcSps(const uint8_t* nal, size_t len, SpsInfo& info);

// Reassembles fragments into access units. Designed for LOW LATENCY, not
// completeness: frames are delivered as soon as complete; an incomplete frame
// is abandoned as soon as a fragment of a NEWER frame_seq completes a frame or
// when more than `max_pending` frames are in flight. Fragments of frames older
// than the last delivered frame are discarded. Duplicates are ignored.
class FrameAssembler {
 public:
  struct Frame {
    uint32_t frame_seq = 0;
    uint16_t epoch = 0;
    uint8_t flags = 0;
    uint64_t capture_ts_us = 0;
    std::vector<uint8_t> data;
  };
  struct Counters {
    uint64_t frames_complete = 0;
    uint64_t frames_lost = 0;      // abandoned or never seen (seq gaps)
    uint64_t fragments_received = 0;
    uint64_t fragments_dropped = 0; // late/duplicate/invalid (Video and invalid Fec)
    // FEC (additive):
    uint64_t frames_recovered = 0;     // delivered frames that needed >=1 RS-recovered fragment
    uint64_t fragments_recovered = 0;  // data fragments rebuilt by RS
    uint64_t fec_received = 0;         // parity datagrams stored
    uint64_t fec_unused = 0;           // parity that arrived late (frame already done) or duplicate
  };

  explicit FrameAssembler(size_t max_pending = 4);
  ~FrameAssembler();
  FrameAssembler(const FrameAssembler&) = delete;
  FrameAssembler& operator=(const FrameAssembler&) = delete;

  // Feed one datagram's header + payload. Returns true and fills `out` when a
  // frame became complete and is decodable. After any loss, delta frames are
  // DROPPED until the next keyframe (decoding them only shows corruption) and
  // needKeyframe() turns true; the caller sends Command{RequestKeyframe}
  // (rate-limited) and calls clearNeedKeyframe(). Initially waits for a
  // keyframe too. A different epoch resets all state.
  // Fec datagrams (UdpType::Fec) are accepted too: a frame completes as soon as every
  // FEC block has >= data_count shards (data+parity); recovered fragments are rebuilt
  // in place. push() returns true only when a frame is delivered.
  bool push(const UdpHeader& h, const uint8_t* payload, size_t len, Frame& out);

  // NOTE: starts TRUE (the assembler waits for a first keyframe) and only has
  // meaning for UDP sessions; TCP clients must not act on it.
  bool needKeyframe() const;
  void clearNeedKeyframe();
  const Counters& counters() const;
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// --------------------------------------------------------------- helpers
// Seq comparison with wraparound: true if a is newer than b.
inline bool seqNewer(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b) > 0; }

const char* toString(MsgType);
const char* toString(ParamId);

}  // namespace owc

#include "protocol_inl.h"  // packetize<> template definition
