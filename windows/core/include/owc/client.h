// owc_core — PC side of the OWC phone-webcam link.
//
// One owc::Client = one session with one phone:
//   connect -> Hello/HelloAck -> Capabilities/ParamState cache -> StartStream
//   -> receive H.264/HEVC over TCP (VideoFrame) or UDP (fragments)
//   -> Media Foundation decoder (D3D11/DXVA hardware, software fallback)
//   -> optional rotate/mirror on NV12 -> owc::ShmWriter (virtual cam / OBS)
//                                      -> preview callback / latest-frame copy
//
// Companion headers: owc/discovery.h (LAN discovery), owc/adb.h (USB via adb
// forward), owc/presets.h (latency/quality presets).
//
// ============================================================== THREADING
// Every public method of owc::Client is thread-safe and NON-BLOCKING (it only
// posts a request or copies a snapshot under a short lock), so it may be called
// from the UI thread at 60 Hz or from inside any ClientEvents callback.
// The ONLY exceptions: the destructor (joins the internal threads; never
// destroy a Client from inside one of its own callbacks).
//
// Internal threads (created in the constructor, joined in the destructor):
//   "owc-net"    sockets, handshake, ping/receiver-report timers, reconnect.
//                Invokes every ClientEvents callback EXCEPT onFrame.
//   "owc-decode" Media Foundation decoder, transform, shm publish, jitter
//                buffer pacing. Invokes ClientEvents::onFrame.
//   "owc-udp"    (UDP sessions only, high priority) only drains the UDP socket
//                into the decode queue; FrameAssembler/FEC run on owc-decode.
//                Never invokes callbacks.
// Callbacks must return quickly (they stall the thread they run on — a slow
// onFrame directly adds latency; a slow net callback delays ping replies and
// UDP draining). Do not block on UI-thread work inside a callback; copy and
// post instead.
//
// PREVIEW PRESENTATION (note for the UI, research sec. 5.1/5.4): present the
// newest frame only; D3D11 swap chain DXGI_SWAP_EFFECT_FLIP_DISCARD with
// BufferCount 4-5, Present(0, DXGI_PRESENT_ALLOW_TEARING) when
// DXGI_FEATURE_PRESENT_ALLOW_TEARING is supported (swap chain created with
// DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING), and do NOT call
// SetMaximumFrameLatency(1) (it makes Present(0) block on DWM). Upload in
// onFrame (copy into your own buffer) or poll copyLatestFrame() per render.
//
// No exceptions cross this API. Errors are reported via return values,
// ClientState/DisconnectReason and the log ring buffer.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <owc/protocol.h>

namespace owc {

// ------------------------------------------------------------------ enums
enum class ClientState : uint8_t {
  Idle = 0,          // not connected and not trying (initial / after disconnect())
  Connecting,        // TCP connect in progress
  Handshaking,       // Hello sent, waiting HelloAck
  Connected,         // session up, not streaming (call startStream())
  Streaming,         // StartStream sent and VideoFormat received
  Reconnecting,      // lost/failed; waiting ClientOptions::reconnect_delay_ms to retry
  Failed,            // gave up (auto_reconnect off). See lastDisconnectReason().
};

enum class DisconnectReason : uint8_t {
  None = 0,
  UserRequested,     // disconnect() called
  ConnectFailed,     // TCP connect refused/unreachable/timeout
  NoResponse,        // TCP accepted but no HelloAck after handshake_attempts tries:
                     // the OWC app is not running/listening on the phone (over USB,
                     // `adb forward` accepts the TCP connection even if nothing listens)
  Busy,              // phone already serves another PC (HelloStatus::Busy)
  VersionMismatch,   // protocol version differs (HelloStatus::VersionMismatch)
  BadMagic,          // peer is not an OWC phone
  PeerBye,           // phone sent Bye
  PeerClosed,        // TCP closed by peer
  Timeout,           // no message from phone for ClientOptions::idle_timeout_ms
  ProtocolError,     // malformed stream
  SocketError,       // any other socket failure
};

enum class DecoderMode : uint8_t {
  Auto = 0,      // default: starts on D3D11/DXVA hardware, measures ms/frame (decode +
                 // GPU->CPU read-back) on the live stream, then tries the software
                 // MFT and keeps the faster (see ClientStats::decoder_selection).
                 // Measured on Intel Iris Xe: HW 11/21 ms vs SW 2.5/5.5 ms at 720p/1080p.
  Hardware = 1,  // hardware only; if unavailable the stream is not decoded (error logged)
  Software = 2,  // CPU decoding (MF software MFT). +CPU load, usually +2..10 ms per frame at 1080p
};

enum class LogSource : uint8_t { Client = 0, Phone = 1 };

// Which codec the client asks the phone for (ClientOptions::codec_preference).
//   Auto: first of AV1 > HEVC that the phone advertises (HelloAck codec_mask)
//         AND this PC can decode IN HARDWARE; otherwise H.264 (always works).
//   H264/Hevc/Av1: that codec if the phone advertises it and this PC can decode
//         it (hardware or software), else H.264.
// The choice is sent as SetParam(VideoCodec) before StartStream, only when it
// differs from the phone's current value. If the phone rejects it, or decoding
// it fails at runtime (decoder can't open / repeated errors / no output), the
// client falls back to H.264 automatically and remembers that codec as
// unusable for the rest of this Client's life (until disconnect()).
enum class CodecPreference : uint8_t { Auto = 0, H264 = 1, Hevc = 2, Av1 = 3 };

// What this PC can decode (Media Foundation), probed once per process.
struct DecoderCapabilities {
  struct Entry {
    bool hardware = false;   // D3D11-accelerated MFT available
    bool software = false;   // software MFT available
    std::string mft_name;    // e.g. "Microsoft H264 Video Decoder MFT"
    bool any() const { return hardware || software; }
  };
  Entry h264, hevc, av1;     // HEVC needs "HEVC Video Extensions", AV1 "AV1 Video Extension"
  std::string adapter;       // default D3D11 adapter description
  bool adapter_discrete = false;  // dedicated video memory >= 512 MB
  uint64_t dedicated_video_memory = 0;
  const Entry& get(Codec c) const { return c == Codec::Hevc ? hevc : c == Codec::Av1 ? av1 : h264; }
};

const char* toString(ClientState);
const char* toString(DisconnectReason);
const char* toString(DecoderMode);

// ------------------------------------------------------------------ options
// User transform applied on the PC to decoded NV12 before publishing.
// Final image = mirror_h( rotate_cw( decoded, total_rotation ) ) where
//   total_rotation = rotation_deg + (apply_phone_hints ? VideoFormat::rotation_deg : 0)
//   mirror_h       = mirror XOR (apply_phone_hints && VideoFormat::mirror)
// Identity (0°, no mirror) costs nothing. Otherwise one CPU pass over the frame
// (~1-3 ms at 1080p for 90/270, <1 ms for 180/mirror).
struct OutputTransform {
  int rotation_deg = 0;          // 0, 90, 180, 270 clockwise (other values snap to nearest)
  bool mirror = false;           // horizontal mirror of the final (rotated) image
  bool apply_phone_hints = true; // combine with VideoFormat rotation/mirror hints
  bool operator==(const OutputTransform& o) const {
    return rotation_deg == o.rotation_deg && mirror == o.mirror &&
           apply_phone_hints == o.apply_phone_hints;
  }
  bool operator!=(const OutputTransform& o) const { return !(*this == o); }
};

// PC-side tunables. Every field documents WHEN a change takes effect and its
// LATENCY IMPACT (meant for UI tooltips). Change at runtime with setOptions().
struct ClientOptions {
  // Presentation jitter buffer. 0 = off: every frame is published the instant
  // it is decoded (lowest latency, motion may stutter on Wi-Fi).
  // >0: decoded frames are held and published paced by their capture
  // timestamps, at (capture time + base path latency + jitter_buffer_ms).
  // Latency impact: + exactly this many ms. Smoothness: absorbs network/encoder
  // jitter up to this amount. Typical 30..150 ms. Effect: immediate.
  uint32_t jitter_buffer_ms = 0;

  // Decoder selection. Effect: decoder is re-created on the next keyframe
  // (a keyframe is requested automatically). Latency: Hardware/Auto lowest.
  DecoderMode decoder_mode = DecoderMode::Auto;

  // CODECAPI_AVLowLatencyMode / MF_LOW_LATENCY on the decoder: output each
  // frame as soon as it is decodable. Measured (MS H.264 MFT, HW and SW):
  // ON = 0 frames of decoder delay without B-frames, 1-3 frames with
  // B-frames (output is still in correct display order). OFF = the decoder
  // buffers its whole DPB (14-19 frames, ~500 ms at 30 fps) with no image
  // quality benefit. Keep ON. Effect: decoder re-created on the next keyframe.
  bool low_latency_decode = true;

  // Minimum interval between RequestKeyframe commands sent after loss/decoder
  // errors. Lower = faster recovery after loss, but more bitrate spikes
  // (keyframes are 5-20x bigger than P-frames). Effect: immediate.
  uint32_t keyframe_request_min_interval_ms = 250;

  // UDP receive socket buffer (SO_RCVBUF). Too small = bursts (keyframes)
  // overflow it and fragments are lost. No latency cost by itself (the
  // receiver drains it continuously). Effect: immediate (setsockopt).
  uint32_t udp_recv_buffer_bytes = 4u * 1024u * 1024u;

  // FrameAssembler max_pending: how many incomplete UDP frames may be in
  // flight before the oldest is declared lost. Higher tolerates reordering
  // on the network, but waits longer before declaring loss and requesting a
  // keyframe. Range 1..32. Effect: assembler re-created (state reset) at once.
  uint32_t udp_max_pending_frames = 4;

  // PC transform (rotation/mirror). Effect: next frame.
  OutputTransform transform;

  // Reconnect automatically after a lost/failed connection (also re-sends
  // StartStream if the stream was running). Effect: immediate.
  bool auto_reconnect = true;
  uint32_t reconnect_delay_ms = 1000;

  // Send StartStream as soon as the session is up (after Capabilities, so the
  // codec can be negotiated first; at most 500 ms later). Effect: next connect.
  bool auto_start_stream = true;

  // Codec negotiation (see CodecPreference). Effect: next connect / next
  // Capabilities from the phone. Latency: HEVC/AV1 save bitrate at equal
  // quality; on hardware decoders latency is the same as H.264.
  CodecPreference codec_preference = CodecPreference::Auto;

  // Publish decoded frames to the shared-memory bus (owc_virtualcam / OBS
  // plugin read it). Only ONE producer per Windows session should publish.
  // Bus name: owc::shmName() (OWC_SHM_NAME env var overrides, for tests).
  // Only one producer may own the bus: if another process (e.g. a second OWC
  // app) owns it, ClientStats::shm_error says so, a Warn log is emitted and
  // the client retries every 2 s; decoding/preview keep working.
  // Effect: immediate.
  bool publish_to_shm = true;

  // Keep a copy of the newest output frame for copyLatestFrame() (one memcpy
  // per frame, ~0.3 ms at 1080p). Effect: immediate.
  bool keep_latest_frame = true;

  // Connection timing. Effect: next connect / immediate for idle timeout.
  uint32_t connect_timeout_ms = 3000;
  uint32_t handshake_timeout_ms = 1500;  // per attempt (wait for HelloAck)
  uint32_t handshake_attempts = 3;       // reconnect + resend Hello this many times
  uint32_t idle_timeout_ms = 5000;   // disconnect if the phone is silent this long

  // Rewrite the H.264/HEVC SPS (csd and in-band) to declare
  // max_num_reorder_frames=0 / max_dec_frame_buffering so hardware decoders
  // don't hold frames waiting for a DPB that never fills (research sec. 5.3).
  // Only applied when the stream has no B-frames. Latency: -0..N frames on
  // decoders that buffer by DPB size; no cost otherwise. Effect: next keyframe.
  // Uses owc::patchAnnexBSps from protocol/.
  bool patch_sps = true;

  // Local address the UDP video socket binds to ("0.0.0.0" = all interfaces).
  // Effect: next connect.
  std::string udp_bind_address = "0.0.0.0";
};

// Parameters of connect().
struct ConnectParams {
  std::string host = "127.0.0.1";          // IPv4 literal or hostname
  uint16_t port = kDefaultControlPort;     // TCP control port (USB: adb-forwarded local port)
  VideoTransport transport = VideoTransport::Tcp;  // USB must use Tcp
  std::string client_name = "OWC PC";      // shown on the phone
};

// Phone identity from HelloAck.
struct DeviceInfo {
  uint32_t session_id = 0;
  std::string device_name, model;
  uint16_t android_api = 0;
  uint32_t codec_mask = 0;          // codecBit(Codec::H264) | codecBit(Codec::Hevc)
  uint16_t udp_port = 0;
};

// ------------------------------------------------------------------ frames
// A decoded + transformed NV12 frame handed to ClientEvents::onFrame.
// LIFETIME: the pointers are valid ONLY during the onFrame call (they point
// into an internal buffer reused for the next frame). Copy what you need.
struct DecodedFrame {
  const uint8_t* y = nullptr;    // width x height luma, row pitch y_stride
  const uint8_t* uv = nullptr;   // width x height/2 bytes (interleaved U,V), pitch uv_stride
  uint32_t y_stride = 0, uv_stride = 0;
  uint32_t width = 0, height = 0;      // after transform (even)
  uint32_t epoch = 0;                   // VideoFormat epoch
  uint32_t frame_seq = 0;               // phone frame_seq (decode order)
  uint64_t capture_ts_us = 0;           // phone clock
  int64_t capture_qpc = 0;              // capture time mapped to PC QPC (0 = clock not synced yet)
  int64_t publish_qpc = 0;              // QPC when published
  int rotation_applied = 0;             // degrees applied by the PC transform
  bool mirrored = false;
  // YUV->RGB conversion for the preview shader (also written to the shm slot):
  // color_matrix = OWC_COLOR_* from owc/shm.h: 0 unknown (pick 709 for >= 720p,
  // else 601), 1 BT.601, 2 BT.709. full_range: true = 0..255, false = 16..235.
  // Source: H.264 SPS VUI (matrix_coefficients / video_full_range_flag), else the
  // decoder's MF output type, else unknown/limited.
  uint32_t color_matrix = 0;
  bool full_range = false;
};

// Tightly packed copy of the newest frame (copyLatestFrame()).
struct FrameCopy {
  std::vector<uint8_t> nv12;     // width*height*3/2 bytes, Y then UV, stride == width
  uint32_t width = 0, height = 0;
  uint64_t frame_number = 0;     // increments per published frame (pass back as lastSeen)
  uint64_t capture_ts_us = 0;
  int64_t capture_qpc = 0, publish_qpc = 0;
};

// ------------------------------------------------------------------ logs
struct LogEntry {
  uint64_t id = 0;               // increasing; pass the last seen id to logs()
  int64_t pc_time_ms = 0;        // wall clock, ms since Unix epoch (UTC)
  LogSource source = LogSource::Client;
  LogLevel level = LogLevel::Info;
  std::string text;
};

// ------------------------------------------------------------------ stats
// Rolling statistics. Rates are measured over ~1 s windows; latency
// percentiles over the last ~120 samples.
struct ClientStats {
  ClientState state = ClientState::Idle;
  DisconnectReason last_reason = DisconnectReason::None;
  VideoTransport transport = VideoTransport::Tcp;
  std::string peer;              // "ip:port"
  uint64_t session_uptime_ms = 0;
  uint32_t reconnect_count = 0;

  // clock sync (Ping/Pong 2 Hz)
  bool clock_synced = false;
  double rtt_ms = 0;             // last RTT
  double rtt_min_ms = 0;         // min RTT in the filter window (≈8 s)
  int64_t clock_offset_us = 0;   // phone_clock - pc_clock (µs), from the min-RTT sample

  // network
  double rx_fps = 0;             // complete frames received / s
  double rx_bitrate_bps = 0;     // video payload bits / s
  uint64_t frames_received = 0;
  uint64_t frames_lost = 0;      // seq gaps + abandoned UDP frames + decode-queue drops
  uint64_t fragments_received = 0;  // UDP only
  uint64_t fragments_lost = 0;      // UDP only (estimate from frag_count)
  uint64_t frames_recovered_fec = 0;  // UDP frames that needed >=1 FEC-rebuilt fragment
  uint64_t datagrams_dropped_queue = 0;  // UDP datagrams dropped: receive queue full
  double jitter_ms = 0;          // RFC 3550 inter-arrival jitter (capture ts vs arrival)
  double delay_gradient_ms = 0;  // least-squares slope of one-way delay over the last 20
                                 // frames, as ms of delay change across that span (>0 = queueing)
  uint32_t keyframe_requests = 0;
  bool udp_punched = false;      // PunchAck received

  // video
  bool has_format = false;
  VideoFormat format;            // last VideoFormat (csd included)
  uint32_t output_width = 0, output_height = 0;  // after transform

  // decoder
  bool decoder_hardware = false;
  bool decoder_low_latency = false;  // effective low-latency setting of the decoder
  bool stream_has_bframes = false;   // reordering detected (or BFrames param > 0)
  bool sps_patched = false;          // SPS rewritten for low latency (patch_sps)
  std::string decoder_name;
  std::string decoder_error;     // e.g. "HEVC decoder not installed (HEVC Video Extensions)"
  std::string decoder_selection; // Auto-mode benchmark result, e.g. "auto: software 5.4 ms vs hardware 21.0 ms -> software"
  std::string codec_selection;   // e.g. "auto: HEVC (hardware decoder present)",
                                 // "auto: H.264 (no HEVC decoder: install 'HEVC Video Extensions')"
  Codec requested_codec = Codec::H264;  // what the client asked the phone for
  double decoded_fps = 0;
  double published_fps = 0;
  uint64_t frames_decoded = 0;
  uint64_t frames_published = 0;
  double decode_ms_avg = 0;      // ProcessInput -> NV12 in CPU memory (includes read-back)
  double readback_ms_avg = 0;    // GPU texture -> CPU copy part of decode_ms (0 in software)
  double decoder_delay_frames_avg = 0;  // inputs fed after a frame before it came out (0 = 1-in/1-out)
  uint32_t decoder_delay_frames_max = 0;

  // latency (needs clock sync for capture-based values)
  double arrival_to_decoded_ms_avg = 0, arrival_to_decoded_ms_p95 = 0;
  double capture_to_publish_ms_avg = 0, capture_to_publish_ms_p95 = 0;  // end-to-end estimate
  double capture_to_arrival_ms_avg = 0;  // phone encode + send + network
  uint32_t jitter_buffer_frames = 0;     // frames currently held by the jitter buffer
  // Latency-first overload handling (PC slower than the stream, e.g. 1080p120):
  uint64_t frames_not_presented = 0;     // decoded but not published (a newer frame was ready)
  uint64_t backlog_drops = 0;            // times the backlog (> 100 ms + jitter buffer) was
                                         // dropped to the newest keyframe / an IDR request

  // shared-memory output
  bool shm_publishing = false;   // this client owns the bus and publishes
  std::string shm_error;         // e.g. another OWC producer (pid N) owns the bus; retried every 2 s
  uint32_t color_matrix = 0;     // OWC_COLOR_* of the current stream (see DecodedFrame)
  bool full_range = false;

  // phone (Stats message, 1 Hz): fps, bitrate (measured + target_bitrate_bps),
  // dropped frames, send queue, encode/ISP latency, FEC %, battery, thermal.
  bool has_phone_stats = false;
  Stats phone;
};

// ------------------------------------------------------------------ events
// Override what you need. See THREADING at the top of this file: all methods
// except onFrame run on "owc-net"; onFrame runs on "owc-decode".
class ClientEvents {
 public:
  virtual ~ClientEvents() = default;
  // Every state transition. reason is meaningful for Reconnecting/Failed/Idle.
  virtual void onStateChanged(ClientState /*state*/, DisconnectReason /*reason*/,
                              const std::string& /*message*/) {}
  virtual void onConnected(const DeviceInfo&) {}
  virtual void onCapabilities(const Capabilities&) {}
  // Phone reply to setParam() (status != Ok = rejected) or unsolicited change.
  virtual void onParamState(const ParamState&) {}
  virtual void onVideoFormat(const VideoFormat&) {}
  virtual void onPhoneStats(const Stats&) {}
  // Phone Log messages and client-side log lines (also kept in logs()).
  virtual void onLog(const LogEntry&) {}
  // "owc-decode" thread. Pointers valid only during the call.
  virtual void onFrame(const DecodedFrame&) {}
};

// ------------------------------------------------------------------ client
class Client {
 public:
  // `events` may be null (poll the getters instead). It must outlive the Client.
  explicit Client(ClientEvents* events = nullptr, const ClientOptions& options = ClientOptions());
  ~Client();  // disconnects (sends Bye) and joins threads. NOT from a callback.
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // ---- session control (asynchronous; progress via onStateChanged/state())
  // Starts connecting (drops any current session first). Returns false only
  // on invalid arguments (empty host, port 0).
  bool connect(const ConnectParams& params);
  // Sends Bye, closes sockets, stops reconnecting, state -> Idle.
  void disconnect();

  ClientState state() const;
  DisconnectReason lastDisconnectReason() const;
  ConnectParams connectParams() const;

  // ---- options
  ClientOptions options() const;
  void setOptions(const ClientOptions& options);  // see per-field "Effect"
  void setTransform(const OutputTransform& t);    // shortcut, next frame

  // ---- phone control. All return false if no session is up (Connected or
  // Streaming); true means "sent", the phone answers asynchronously.
  bool setParam(ParamId id, const ParamValue& value);  // reply: onParamState
  bool startStream();          // also remembered across auto-reconnects
  bool stopStream();
  bool requestKeyframe();      // honours keyframe_request_min_interval_ms
  bool focusAt(float x, float y);  // normalized 0..1, sensor orientation
  bool resetParams();
  bool setDeviceName(const std::string& name);
  bool sendCommand(const Command& cmd);  // raw escape hatch

  // ---- snapshots (thread-safe copies)
  bool deviceInfo(DeviceInfo& out) const;          // false before HelloAck
  bool capabilities(Capabilities& out) const;      // false before Capabilities
  // Current value of a param (from Capabilities, updated by ParamState).
  bool paramValue(ParamId id, ParamValue& out) const;
  bool paramDesc(ParamId id, ParamDesc& out) const;
  bool videoFormat(VideoFormat& out) const;        // false before VideoFormat
  ClientStats stats() const;
  // Log lines with id > sinceId (ring buffer of the last 1000 entries).
  std::vector<LogEntry> logs(uint64_t sinceId = 0) const;
  void clearLogs();
  // Copies the newest output frame if its frame_number != lastSeen.
  // Requires options().keep_latest_frame. Returns false if nothing new.
  bool copyLatestFrame(FrameCopy& out, uint64_t lastSeen) const;

  // What this PC can decode (probed once per process on first call, ~100-300
  // ms; thread-safe; later calls return the cached result).
  static DecoderCapabilities decoderCapabilities();
  // TEST HOOK: replaces the probed capabilities (nullptr = back to the real
  // probe). Affects codec negotiation of every Client in the process.
  static void overrideDecoderCapabilitiesForTest(const DecoderCapabilities* caps);

  // Converts a phone capture timestamp to PC QPC using the current clock
  // offset (0 if not synced). Useful to compute latency at display time.
  int64_t phoneTimeToQpc(uint64_t phone_us) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace owc
