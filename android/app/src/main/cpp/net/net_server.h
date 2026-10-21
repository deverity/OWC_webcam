// Net thread: one poll() loop over
//   - TCP listener (controlPort, SO_REUSEADDR). One active client; a second
//     client immediately gets HelloAck{Busy} and is closed. USB = adb forward,
//     i.e. a TCP client from 127.0.0.1.
//   - the active client's TCP socket (read side; TcpFrameParser). Ping is
//     answered right here (Pong stamped by the sender); everything else is
//     forwarded to the worker via Callbacks.
//   - UDP video socket (udpPort): Punch -> validate session_id, remember the
//     source address, reply PunchAck. The same fd is used by the Sender for video.
//   - UDP discovery socket (discoveryPort): DiscoveryRequest -> DiscoveryReply.
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
  std::atomic<uint32_t> sessionId{0};  // set by the worker after HelloAck{Ok}
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
  bool start(uint16_t controlPort, uint16_t udpPort, uint16_t discoveryPort, Callbacks* cb,
             std::string& err);
  void stop();
  int udpFd() const { return udpFd_; }

 private:
  void loop();
  void acceptClient();
  void readClient();
  void dropClient(const char* why);
  void readUdpVideo();
  void readDiscovery();

  Callbacks* cb_ = nullptr;
  int listenFd_ = -1, udpFd_ = -1, discFd_ = -1, wakeFd_ = -1;
  uint16_t controlPort_ = 0, udpPort_ = 0;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  // Net-thread-only state.
  std::shared_ptr<ClientConn> client_;
  TcpFrameParser parser_;
  uint64_t nextGen_ = 1;
  std::vector<uint8_t> rxBuf_;
};

}  // namespace owc::net
