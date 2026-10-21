// FakePhone: Windows implementation of the PHONE side of the OWC protocol,
// for testing owc_core / the desktop app without a real Android device.
//   TCP control server (+ VideoFrame in Tcp mode), UDP punch/video, discovery.
//   Synthetic NV12 pattern with the frame-counter strip (see pattern.h),
//   encoded by the Media Foundation H.264 encoder.
//   Protocol v3 authentication + encryption (PAIRING in owc/protocol.h,
//   owc/crypto.h), mirroring the Android phone: ephemeral X25519 key exchange,
//   cipher chosen from the Hello mask (chooseCipher), AuthProof key
//   confirmation, CPace pairing with codes (fixed for tests or random + logged;
//   the code never travels), the phone's own confirmation in AuthResult Ok,
//   every TCP message after Ok sealed both ways, UDP video/PunchAck sealed and
//   only sealed punches answered. Wrong codes counted GLOBALLY until
//   lockout/success (forgotten after 10 min, so a new code may get fewer
//   attempts), lockout, expiry, "cancel on the phone" (rejectPairing()), zero
//   client_id -> Rejected, removing the connected PC (setPairedClients) -> Bye +
//   close, preloaded pairings. Test attacks: forged confirmation, tampered or
//   plaintext TCP record, tampered/replayed UDP datagrams.
#pragma once

#include <owc/crypto.h>
#include <owc/protocol.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace owc {
namespace fake {

struct FakePhoneConfig {
  std::string bind_address = "0.0.0.0";
  uint16_t control_port = 47800;   // 0 = ephemeral (see controlPort())
  uint16_t udp_port = 47801;       // 0 = ephemeral
  uint16_t discovery_port = 47802; // 0 = ephemeral
  bool enable_discovery = true;
  // Unsolicited announcements (DiscoveryReply kind=3) like the real phone,
  // sent every announce_interval_ms while no client is connected.
  // "host:port", e.g. "255.255.255.255:47803" or "127.0.0.1:47803"; "" = off.
  std::string announce_target;
  uint32_t announce_interval_ms = 2000;
  uint32_t width = 1280, height = 720, fps = 30;
  uint32_t bitrate = 4000000;
  uint32_t bframes = 0;            // >0 => Main profile with B-frames
  int profile = 0;                 // 0 Baseline 1 Main 2 High
  uint32_t keyframe_interval_ms = 2000;  // 0 = only on request
  double loss_percent = 0;         // UDP datagram drop probability (%)
  int fec_percent = 0;             // UDP Reed-Solomon parity (%), 0 = off
  int64_t clock_offset_us = 3600LL * 1000000;  // phone clock = PC clock + offset
  std::string device_name = "FakePhone";
  std::string model = "OWC-Fake";
  // Like the Android encoder: IDRs keep their SPS/PPS inline and are flagged
  // kFrameKey|kFrameConfig. false = strip them (csd only via VideoFormat).
  bool inline_param_sets = true;
  int16_t rotation_deg = 0;        // VideoFormat rotation hint
  // Replay a recorded Annex-B H.264 file (e.g. from a real phone) instead of
  // encoding the pattern. Leading SPS/PPS = csd; loops; a keyframe request
  // restarts from the first AU (the file must start with an IDR).
  std::string replay_file;
  uint32_t replay_fps = 30;
  // Codec negotiation tests. codec_mask goes into HelloAck (default H.264 only).
  // hevc_mode: 0 = SetParam(VideoCodec=HEVC) rejected (Unsupported);
  //            1 = accepted, but the stream is still H.264 labelled as HEVC in
  //                VideoFormat (forces a decoder failure on the PC: tests the
  //                runtime fallback). The encoder is always H.264.
  uint32_t codec_mask = 1;  // codecBit(Codec::H264)
  int hevc_mode = 0;
  // Stats::encode_fps is multiplied by this while the codec is HEVC (tests
  // the client's "HEVC can't sustain the fps -> H.264" fallback).
  float hevc_stats_fps_scale = 1.0f;
  bool verbose = false;
  std::function<void(const std::string&)> logger;  // default: stdout

  // ---- authentication / pairing (protocol v2). Every connection must
  // authenticate before anything else is accepted (Ping, SetParam, UDP punch...).
  struct PairedClient {
    Id16 client_id{};
    Key32 key{};
    std::string name;  // Hello::client_name at pairing time
  };
  Id16 phone_id{};                  // all zeros = derived from device_name (stable across runs)
  std::string pair_code;            // code shown on PairBegin; "" = random 6 digits (logged)
  std::vector<PairedClient> paired_clients;  // preloaded pairings (e.g. tests)
  std::string pairings_file;        // load at start / save on change ("" = memory only)
  uint8_t pair_attempts = kPairCodeAttempts;      // wrong codes (global) before Locked
  uint16_t pair_code_lifetime_s = kPairCodeLifetimeS;
  uint16_t pair_lockout_s = kPairLockoutS;        // PairBegin answered Locked this long after a lock
  uint16_t auth_timeout_s = kAuthTimeoutS;        // unauthenticated connection closed after this

  // ---- protocol v3 encryption
  uint32_t cipher_mask = 0;          // ciphers this phone runs (cipherBit); 0 = localCipherMask()
  // Test attacks (integration tests; all off by default):
  bool forge_phone_confirm = false;      // AuthResult Ok carries a wrong confirmation (impostor/relay)
  uint32_t tcp_tamper_after_frames = 0;  // >0: flip a ciphertext bit in the record of TCP video frame N
  uint32_t tcp_plain_after_frames = 0;   // >0: send one PLAINTEXT Log right after TCP video frame N
  uint32_t udp_attack_every = 0;         // >0: after every Nth video/FEC datagram also send a tampered
                                         //     copy of it and a replay of the previous datagram
};

class FakePhone {
 public:
  struct Counters {
    uint64_t frames_encoded = 0, frames_sent = 0, keyframes = 0;
    uint64_t datagrams_sent = 0, datagrams_dropped = 0, fec_datagrams = 0;
    uint64_t keyframe_requests = 0, pings = 0, receiver_reports = 0, sessions = 0;
    uint64_t busy_rejects = 0;
    // authentication
    uint64_t auth_proofs = 0, auth_ok = 0, auth_not_paired = 0;
    uint64_t pair_begins = 0, pair_codes = 0, pair_wrong = 0, pair_ok = 0;
    uint64_t pair_locked = 0, pair_expired = 0, pair_rejected = 0;
    uint64_t unauth_msgs = 0;  // non-auth messages received before authentication (ignored)
    // encryption
    uint8_t last_cipher = 0;             // owc::Cipher of the last session
    uint64_t tcp_open_failures = 0;      // PC records that failed to open / plaintext after Ok (fatal)
    uint64_t punches_rejected = 0;       // punches that did not open with the session key (ignored)
    uint64_t udp_tampered_sent = 0, udp_replays_sent = 0;  // udp_attack_every
    float last_rr_jitter_ms = 0, last_rr_delay_gradient_ms = 0, last_rr_decode_ms = 0;
    uint32_t last_rr_frames_lost = 0;
  };

  FakePhone();
  ~FakePhone();
  bool start(const FakePhoneConfig& cfg, std::string& err);
  void stop();
  uint16_t controlPort() const;
  uint16_t udpPort() const;
  uint16_t discoveryPort() const;
  Counters counters() const;
  bool streaming() const;

  // ---- authentication
  Id16 phoneId() const;
  std::string lastPairCode() const;  // code of the last PairBegin ("" = none yet)
  std::vector<FakePhoneConfig::PairedClient> pairedClients() const;
  // Replaces the pairing list (e.g. {} = "the user removed every PC on the phone").
  // If the connected PC is no longer listed, it gets Bye and is disconnected.
  void setPairedClients(const std::vector<FakePhoneConfig::PairedClient>& v);
  // Simulates "cancel" tapped on the phone: a pending pairing gets AuthResult Rejected.
  void rejectPairing();
  // Drops the current connection without Bye (tests auto-reconnect).
  void kickClient();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace fake
}  // namespace owc
