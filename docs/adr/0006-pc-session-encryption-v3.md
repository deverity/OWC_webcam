# ADR 0006: PC side of protocol v3 (owc_core handshake, record layer, cipher option)

Status: accepted (2026-10-21)
Contract: `protocol/include/owc/crypto.h`, `protocol/include/owc/protocol.h` "PAIRING".
Code: `windows/core/src/client.cpp` (runSession / authenticate / udpMain),
`windows/core/include/owc/client.h` (CipherPreference, ENCRYPTION, stats),
`windows/app/src/*` (option "Criptografia", HUD / status / stats),
`windows/tools/fakephone` (v3 phone + test attacks), `windows/core/tests/integration_test.cpp`.
Phone side: ADR 0005.

## Context
Same goal as ADR 0005, PC side. owc_core has three threads (client.h THREADING):
`owc-net` (sockets, handshake, TCP parsing), `owc-udp` (drains the UDP socket into
the decode queue) and `owc-decode` (FrameAssembler/FEC + Media Foundation). The
video path must not get an extra thread, queue hop or copy.

## Decision
1. **Handshake per connection attempt** on `owc-net`: fresh BCryptGenRandom
   ephemeral secret, `Handshake(Pc)`, Hello encoded once and those bytes both sent
   and kept; the HelloAck payload kept as received. Checks in order: magic,
   version (same version + forced cipher -> "phone does not support the chosen
   encryption"), Busy, AuthRequired, chosen cipher must be in what we offered
   (refuses a downgrade to None), `begin()` false -> ProtocolError.
2. **Mutual confirmation**: AuthProof = `pcConfirm()` after `usePairingKey`;
   pairing = `cpaceStart(typed code, fresh rnd)` + `cpaceFinish(Yb)` and PairCode
   is sent even if finish fails (the phone counts it as a wrong code). Yb is
   replaced by every CodeShown/WrongCode. AuthResult Ok is accepted only with a
   proof of ours in flight and `confirm == phoneConfirm()` (constant time); else
   `DisconnectReason::PhoneNotAuthentic`: Failed, no auto-reconnect, key kept, no
   onKeyRejected. A pairing key is only stored (`newPairingKey()`) after that check.
3. **Where crypto runs** (no new thread, no new copy):
   - TCP phone->PC: `TcpOpener` on `owc-net`, wrapped around `parser.next()`
     (records buffered together with the AuthResult are opened the same way).
     Plaintext or a failing record -> ProtocolError.
   - TCP PC->phone: `TcpSealer` inside `sendMsg()` under `sendMx`; seal and send
     are one critical section because messages come from several threads
     (owc-net, UI calls to setParam()/sendCommand()).
   - UDP phone->PC: `UdpOpener` on `owc-udp`, in place in the 2 KB receive buffer
     right after recvfrom (still in L1), before the single copy into the work
     item. Failures are dropped before they reach the queue and counted
     (`ClientStats::udp_auth_failures`): a flood of forged datagrams cannot fill
     the decode queue.
   - UDP PC->phone: punches sealed with `UdpSealer` on `owc-net` under `udpMx`.
4. **Option** `ClientOptions::cipher` (Auto default = localCipherMask() minus
   None; Aes128Gcm / ChaCha20Poly1305 force one; Off = None only). Forcing AES on a
   CPU without AES-NI offers ChaCha (warned) instead of failing. Persisted by
   OWC.exe as `opts.cipher`; missing in v2 settings = Auto.
5. **Observability**: `DeviceInfo::cipher`, `ClientStats::cipher`,
   `decrypt_us_avg/max` per video frame (TCP: one open; UDP: sum over all
   datagrams of the frame, FEC included) from two QPC reads per record/datagram,
   one stats-lock per frame.
6. **Secrets**: the Handshake is destroyed right after the record layer is built,
   SessionKeys copies and pairing-key/code/confirmation copies are wiped,
   sealers/openers are destroyed at session end (Aead wipes its schedule).

## Trade-offs
- UDP open on `owc-udp` instead of `owc-decode`: adds ~10-20 us (AES) / ~70-200 us
  (ChaCha, portable) of work per frame to the thread that must keep draining the
  socket; with a 4 MB SO_RCVBUF that is far from overflow, and it keeps
  decryption in parallel with decoding and rejects forgeries before queuing.
  Moving it to `owc-decode` would serialize it with the decoder.
- TCP open on `owc-net` delays Pong handling behind a large keyframe by up to
  ~0.2 ms (AES) / ~1 ms (ChaCha). Accepted: clock sync uses the min-RTT sample.
- `TcpOpener::open` strips the 2-byte inner type with an erase (memmove of the
  payload) and `handleMsg` then copies the AU into the decode work item: two
  passes over a TCP frame instead of one. A few us per P-frame; a span-returning
  open would remove it (protocol change, not done here).
- Off is kept for diagnostics/very old hardware; the UI marks it red everywhere.

## Measurements (loopback, Intel Iris Xe laptop, 1080p30 8 Mbps, SW decoder)
| transport | cipher | capture->publish avg | decrypt avg / max per frame |
|-----------|--------|----------------------|-----------------------------|
| TCP | off | 11.61 ms | - |
| TCP | ChaCha20-Poly1305 | 12.19 ms | 178 / 367 us |
| TCP | AES-128-GCM | 12.29 ms | 30 / 78 us |
| UDP | off | 11.04 ms | - |
| UDP | ChaCha20-Poly1305 | 12.12 ms | 200 / 479 us |
| UDP | AES-128-GCM | 10.80 ms | 21 / 74 us |
Differences between rows are within run-to-run noise except ChaCha's ~0.2 ms.
