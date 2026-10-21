// Net thread: one poll() loop over
//   - TCP listener (controlPort, SO_REUSEADDR). One active client; a second
//     client immediately gets HelloAck{Busy} and is closed. USB = adb forward,
//     i.e. a TCP client from 127.0.0.1.
//   - the active client's TCP socket (read side; TcpFrameParser). Ping is
//     answered right here once the client is authenticated (Pong stamped by
//     the sender); everything else is forwarded to the worker via Callbacks.
//     A connection that has not authenticated yet still occupies the single
//     client slot (others get Busy); the worker closes it after
//     kAuthTimeoutS / when its pairing code expires.
//   - UDP video socket (udpPort): Punch -> validate session_id, remember the
//     source address, reply PunchAck. The same fd is used by the Sender for video.
//   - UDP discovery socket (discoveryPort): DiscoveryRequest -> DiscoveryReply.
//   - Announce (DiscoveryReply kind=3) broadcast to announcePort every 2 s while
//     idle, every 5 s (busy=1) while a client is connected: phones in doze drop
//     incoming broadcasts (APF), but can still send them, so the PC listens.
// The client socket is shared (shared_ptr<Socket>) with the Sender; whoever
// wants to drop the client calls Socket::shutdownBoth(), this loop sees EOF
// and reports onClientDisconnected() exactly once.
#pragma once

#include <netinet/in.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "net/sender.h"
#include "net/socket_util.h"
#include "owc/protocol.h"

namespace owc::net {

struct ClientConn {
  uint64_t gen = 0;
  std::shared_ptr<Socket> sock;
  sockaddr_in peer{};
  std::string peerStr;
  std::shared_ptr<Sender> sender;
  // Set by the worker only after AuthResult{Ok}; 0 = not authenticated: UDP
  // punches are ignored and Pings are not answered.
  std::atomic<uint32_t> sessionId{0};
  std::atomic<int64_t> lastRxMs{0};
};

class NetServer {
 public:
  class Callbacks {
   public:
    virtual ~Callbacks() = default;
    virtual void onClientConnected(std::shared_ptr<ClientConn> c) = 0;
    virtual void onClientMessage(uint64_t gen, MsgType type, std::vector<uint8_t>&& payload) = 0;
    virtual void onClientDisconnected(uint64_t gen) = 0;
    virtual void onPunch(uint64_t gen, const sockaddr_in& from) = 0;
    virtual DiscoveryReply discoveryInfo() = 0;  // busy flag filled by NetServer
    virtual HelloAck busyAck() = 0;
  };

  NetServer() = default;
  ~NetServer();
  NetServer(const NetServer&) = delete;
  NetServer& operator=(const NetServer&) = delete;

  // Binds everything synchronously (so the caller learns about port errors),
  // then starts the thread.
  bool start(uint16_t controlPort, uint16_t udpPort, uint16_t discoveryPort, uint16_t announcePort,
             Callbacks* cb, std::string& err);
  void stop();
  int udpFd() const { return udpFd_; }
  // Phone->PC keepalive while a client is connected and idle (no video): a
  // tiny UDP datagram with DSCP EF keeps the Wi-Fi radio from DTIM-skipping
  // power save. Thread-safe.
  void setIdle(bool idle) { idle_ = idle; }

 private:
  void loop();
  void acceptClient();
  void readClient();
  void dropClient(const char* why);
  void readUdpVideo();
  void readDiscovery();
  void sendAnnounce();
  void sendKeepalive();

  Callbacks* cb_ = nullptr;
  int listenFd_ = -1, udpFd_ = -1, discFd_ = -1, wakeFd_ = -1, announceFd_ = -1;
  uint16_t controlPort_ = 0, udpPort_ = 0, announcePort_ = 0;
  int64_t nextAnnounceMs_ = 0;
  int64_t nextKeepaliveMs_ = 0;
  int keepaliveHz_ = 0;
  uint32_t announceErrors_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> idle_{true};
  // Net-thread-only state.
  std::shared_ptr<ClientConn> client_;
  TcpFrameParser parser_;
  uint64_t nextGen_ = 1;
  std::vector<uint8_t> rxBuf_;
};

}  // namespace owc::net
