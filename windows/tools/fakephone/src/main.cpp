// owc_fakephone: emulates the Android phone side of the OWC protocol.
//   owc_fakephone [--port 47800] [--udp-port 47801] [--discovery-port 47802]
//                 [--bind 0.0.0.0] [--width 1280] [--height 720] [--fps 30]
//                 [--bitrate 4000000] [--bframes 0] [--profile 0] [--gop-ms 2000]
//                 [--loss 0] [--fec 0] [--name FakePhone] [--no-discovery] [--verbose]
//                 [--pair-code 123456] [--pairings-file F] [--paired CLIENT_HEX:KEY_HEX] ...
//                 [--ciphers aes,chacha,none] [--forge-confirm] [--udp-attack N]
// Protocol v3: every client must authenticate (CPace pairing / key
// confirmation) and, unless both sides agree on "none", all traffic after it is
// encrypted. A new PC gets a pairing code, printed on the console (like the
// phone's notification); type it in OWC.exe.
// Runs until Ctrl+C / Enter.
#include "fake_phone.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
std::atomic<bool> g_quit{false};
BOOL WINAPI onCtrl(DWORD) {
  g_quit = true;
  return TRUE;
}
void usage() {
  printf(
      "owc_fakephone - OWC phone emulator\n"
      "  --port N           TCP control port (default 47800, 0 = any)\n"
      "  --udp-port N       UDP video port (default 47801)\n"
      "  --discovery-port N UDP discovery port (default 47802)\n"
      "  --bind ADDR        bind address (default 0.0.0.0)\n"
      "  --width/--height/--fps/--bitrate N   initial stream settings\n"
      "  --bframes N        B-frames (0..2, implies Main profile)\n"
      "  --profile N        0 Baseline, 1 Main, 2 High\n"
      "  --gop-ms N         periodic keyframe interval (0 = on request only)\n"
      "  --loss P           drop P%% of UDP datagrams\n"
      "  --fec P            Reed-Solomon parity P%% on UDP (-1 = auto 20%%)\n"
      "  --name S           device name\n"
      "  --no-discovery     don't answer discovery broadcasts\n"
      "  --replay FILE      stream a recorded Annex-B H.264 file instead of the pattern\n"
      "  --announce H:P     broadcast kind=3 announcements (e.g. 255.255.255.255:47803)\n"
      "  --announce-ms N    announcement period (default 2000)\n"
      "  --replay-fps N     replay frame rate (default 30)\n"
      "  --rotation DEG     VideoFormat rotation hint\n"
      "  --strip-ps         send IDRs without inline SPS/PPS (csd only)\n"
      "  pairing (protocol v3; every client must authenticate, the code never travels):\n"
      "  --pair-code NNNNNN fixed pairing code (default: random, printed when a PC pairs)\n"
      "  --pairings-file F  keep pairings across runs in this file\n"
      "  --paired C:K       preload a pairing: client_id hex (32) : key hex (64)\n"
      "  --phone-id HEX     phone_id (32 hex; default derived from --name)\n"
      "  --pair-attempts N  wrong codes before Locked (default 3)\n"
      "  --pair-lifetime S  code lifetime in seconds (default 120)\n"
      "  --pair-lockout S   lockout after Locked (default 30)\n"
      "  encryption (protocol v3):\n"
      "  --ciphers LIST     ciphers this phone runs: aes,chacha,none (default: all this CPU can run)\n"
      "  --forge-confirm    TEST: answer AuthResult Ok with a wrong confirmation (impostor)\n"
      "  --udp-attack N     TEST: after every Nth datagram send a tampered copy + a replay\n"
      "  --verbose\n");
}
}  // namespace

int main(int argc, char** argv) {
  owc::fake::FakePhoneConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", name);
        exit(2);
      }
      return argv[++i];
    };
    auto u16 = [&](const char* name) { return static_cast<uint16_t>(atoi(next(name))); };
    auto u32 = [&](const char* name) { return static_cast<uint32_t>(strtoul(next(name), nullptr, 10)); };
    if (a == "--port") cfg.control_port = u16("--port");
    else if (a == "--udp-port") cfg.udp_port = u16("--udp-port");
    else if (a == "--discovery-port") cfg.discovery_port = u16("--discovery-port");
    else if (a == "--bind") cfg.bind_address = next("--bind");
    else if (a == "--width") cfg.width = u32("--width");
    else if (a == "--height") cfg.height = u32("--height");
    else if (a == "--fps") cfg.fps = u32("--fps");
    else if (a == "--bitrate") cfg.bitrate = u32("--bitrate");
    else if (a == "--bframes") cfg.bframes = u32("--bframes");
    else if (a == "--profile") cfg.profile = atoi(next("--profile"));
    else if (a == "--gop-ms") cfg.keyframe_interval_ms = u32("--gop-ms");
    else if (a == "--loss") cfg.loss_percent = atof(next("--loss"));
    else if (a == "--fec") cfg.fec_percent = atoi(next("--fec"));
    else if (a == "--name") cfg.device_name = next("--name");
    else if (a == "--no-discovery") cfg.enable_discovery = false;
    else if (a == "--replay") cfg.replay_file = next("--replay");
    else if (a == "--announce") cfg.announce_target = next("--announce");
    else if (a == "--announce-ms") cfg.announce_interval_ms = u32("--announce-ms");
    else if (a == "--replay-fps") cfg.replay_fps = u32("--replay-fps");
    else if (a == "--rotation") cfg.rotation_deg = static_cast<int16_t>(atoi(next("--rotation")));
    else if (a == "--strip-ps") cfg.inline_param_sets = false;
    else if (a == "--pair-code") cfg.pair_code = next("--pair-code");
    else if (a == "--pairings-file") cfg.pairings_file = next("--pairings-file");
    else if (a == "--pair-attempts") cfg.pair_attempts = static_cast<uint8_t>(u32("--pair-attempts"));
    else if (a == "--pair-lifetime") cfg.pair_code_lifetime_s = u16("--pair-lifetime");
    else if (a == "--pair-lockout") cfg.pair_lockout_s = u16("--pair-lockout");
    else if (a == "--phone-id") {
      if (!owc::fromHex(next("--phone-id"), cfg.phone_id.data(), cfg.phone_id.size())) {
        fprintf(stderr, "--phone-id: expected 32 hex digits\n");
        return 2;
      }
    } else if (a == "--paired") {
      const std::string v = next("--paired");
      const size_t c = v.find(':');
      owc::fake::FakePhoneConfig::PairedClient pc;
      if (c == std::string::npos || !owc::fromHex(v.substr(0, c), pc.client_id.data(), pc.client_id.size()) ||
          !owc::fromHex(v.substr(c + 1), pc.key.data(), pc.key.size())) {
        fprintf(stderr, "--paired: expected CLIENT_ID_HEX(32):KEY_HEX(64)\n");
        return 2;
      }
      pc.name = "preloaded";
      cfg.paired_clients.push_back(pc);
    }
    else if (a == "--ciphers") {
      const std::string v = next("--ciphers");
      cfg.cipher_mask = 0;
      if (v.find("aes") != std::string::npos) cfg.cipher_mask |= owc::cipherBit(owc::Cipher::Aes128Gcm);
      if (v.find("chacha") != std::string::npos) cfg.cipher_mask |= owc::cipherBit(owc::Cipher::ChaCha20Poly1305);
      if (v.find("none") != std::string::npos) cfg.cipher_mask |= owc::cipherBit(owc::Cipher::None);
      if (!cfg.cipher_mask) {
        fprintf(stderr, "--ciphers: expected a list of aes, chacha, none\n");
        return 2;
      }
      if ((cfg.cipher_mask & owc::cipherBit(owc::Cipher::Aes128Gcm)) && !owc::hardwareAesAvailable())
        fprintf(stderr, "warning: --ciphers aes but this CPU has no AES-NI/PCLMULQDQ\n");
    }
    else if (a == "--forge-confirm") cfg.forge_phone_confirm = true;
    else if (a == "--udp-attack") cfg.udp_attack_every = u32("--udp-attack");
    else if (a == "--verbose") cfg.verbose = true;
    else if (a == "--help" || a == "-h") { usage(); return 0; }
    else { fprintf(stderr, "unknown argument %s\n", a.c_str()); usage(); return 2; }
  }
  owc::fake::FakePhone phone;
  std::string err;
  if (!phone.start(cfg, err)) {
    fprintf(stderr, "start failed: %s\n", err.c_str());
    return 1;
  }
  printf("fakephone '%s': TCP %u, UDP %u, discovery %u, %ux%u@%u %u bps, bframes %u, loss %.1f%%, fec %d%%\n",
         cfg.device_name.c_str(), phone.controlPort(), phone.udpPort(), phone.discoveryPort(),
         cfg.width, cfg.height, cfg.fps, cfg.bitrate, cfg.bframes, cfg.loss_percent, cfg.fec_percent);
  {
    const owc::Id16 id = phone.phoneId();
    printf("phone_id %s, %zu paired PC(s), pairing code: %s\n", owc::toHex(id.data(), id.size()).c_str(),
           phone.pairedClients().size(), cfg.pair_code.empty() ? "random (printed on PairBegin)" : cfg.pair_code.c_str());
    const uint32_t mask = cfg.cipher_mask ? cfg.cipher_mask : owc::localCipherMask();
    printf("ciphers:%s%s%s (hardware AES %s)\n", (mask & owc::cipherBit(owc::Cipher::Aes128Gcm)) ? " aes" : "",
           (mask & owc::cipherBit(owc::Cipher::ChaCha20Poly1305)) ? " chacha" : "",
           (mask & owc::cipherBit(owc::Cipher::None)) ? " none" : "", owc::hardwareAesAvailable() ? "yes" : "no");
  }
  printf("Ctrl+C to quit\n");
  fflush(stdout);
  SetConsoleCtrlHandler(onCtrl, TRUE);
  uint64_t lastFrames = 0;
  while (!g_quit) {
    Sleep(1000);
    if (cfg.verbose) {
      auto c = phone.counters();
      if (c.frames_sent != lastFrames)
        printf("sent %llu frames (%llu key), %llu dgrams (%llu dropped, %llu fec), kf req %llu, rr jitter %.2f ms grad %.2f ms\n",
               static_cast<unsigned long long>(c.frames_sent), static_cast<unsigned long long>(c.keyframes),
               static_cast<unsigned long long>(c.datagrams_sent), static_cast<unsigned long long>(c.datagrams_dropped),
               static_cast<unsigned long long>(c.fec_datagrams), static_cast<unsigned long long>(c.keyframe_requests),
               c.last_rr_jitter_ms, c.last_rr_delay_gradient_ms);
      lastFrames = c.frames_sent;
      fflush(stdout);
    }
  }
  phone.stop();
  return 0;
}
