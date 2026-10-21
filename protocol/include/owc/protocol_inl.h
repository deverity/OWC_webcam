// packetize<> template definition. Included at the end of protocol.h.
#pragma once

#include <cstring>

#include "protocol.h"

namespace owc {

// Header fields frag_idx/frag_count/frame_size are filled per datagram; all other
// fields (type, session_id, frame_seq, capture_ts_us, epoch, flags) are copied from
// `base`. max_payload is clamped to [1, kUdpMaxPayload]. If the AU would need more
// than 65535 fragments nothing is sent and 0 is returned.
template <class Sink>
size_t packetize(const UdpHeader& base, const uint8_t* au, size_t au_len, Sink&& sink,
                 size_t max_payload) {
  if (max_payload == 0) max_payload = 1;
  if (max_payload > kUdpMaxPayload) max_payload = kUdpMaxPayload;
  if (static_cast<uint64_t>(au_len) > 0xFFFFFFFFull) return 0;
  size_t count = au_len == 0 ? 1 : (au_len + max_payload - 1) / max_payload;
  if (count > 0xFFFFu) return 0;
  uint8_t dgram[kUdpMaxDatagram];
  UdpHeader h = base;
  h.frag_count = static_cast<uint16_t>(count);
  h.frame_size = static_cast<uint32_t>(au_len);
  size_t off = 0;
  for (size_t i = 0; i < count; ++i) {
    size_t n = au_len - off;
    if (n > max_payload) n = max_payload;
    h.frag_idx = static_cast<uint16_t>(i);
    writeUdpHeader(h, dgram);
    if (n) std::memcpy(dgram + kUdpHeaderSize, au + off, n);
    sink(static_cast<const uint8_t*>(dgram), kUdpHeaderSize + n);
    off += n;
  }
  return count;
}

template <class Sink>
size_t packetizeFec(const UdpHeader& base, const uint8_t* au, size_t au_len,
                    uint8_t parity_total, Sink&& sink, size_t max_payload) {
  if (max_payload == 0) max_payload = 1;
  if (max_payload > kUdpMaxPayload) max_payload = kUdpMaxPayload;
  std::vector<uint8_t> parity;
  if (!fecEncode(au, au_len, parity_total, parity, max_payload)) return 0;
  const size_t S = fecShardSize(au_len, max_payload);
  const size_t count = au_len <= max_payload ? 1 : (au_len + max_payload - 1) / max_payload;
  uint8_t dgram[kUdpMaxDatagram];
  UdpHeader h = base;
  h.type = UdpType::Fec;
  h.frag_count = static_cast<uint16_t>(count);
  h.frame_size = static_cast<uint32_t>(au_len);
  h.fec_total = parity_total;
  for (size_t p = 0; p < parity_total; ++p) {
    h.frag_idx = static_cast<uint16_t>(p);
    writeUdpHeader(h, dgram);
    std::memcpy(dgram + kUdpHeaderSize, parity.data() + p * S, S);
    sink(static_cast<const uint8_t*>(dgram), kUdpHeaderSize + S);
  }
  return parity_total;
}

}  // namespace owc
