# ADR 0005: Phone side of protocol v3 (X25519 + CPace handshake, AEAD on every channel)

Status: accepted (2026-10-21)
Contract: `protocol/include/owc/crypto.h` (KEY SCHEDULE, TCP RECORDS, UDP DATAGRAMS),
`protocol/include/owc/protocol.h` "PAIRING" (wire v3).
Code: `android/app/src/main/cpp/session.{h,cpp}` (Worker handshake), `net/sender.{h,cpp}`
(sealing), `net/net_server.{h,cpp}` (opening), `app_core.{h,cpp}` (status), Kotlin
`MainActivity.kt`. Self-test: `cpp/selftest/selftest.cpp` (`full`).

## Context
v2 authenticated the PC but sent the pairing code and the pairing key in clear,
and all video/control traffic was plain. v3 adds an ephemeral X25519 exchange,
CPace pairing (the code never travels), mutual key confirmation and AEAD on all
TCP and UDP traffic after AuthResult{Ok}. Latency is the product's first
priority: no extra thread, queue or copy may be added to the video path, and the
encoder output thread (`owc-enc-out`) must not pay for crypto.

## Decision
1. **Handshake in the Worker** (`owc-worker`, single-threaded as in ADR 0003).
   Hello: fresh `arc4random_buf` ephemeral secret, `chooseCipher(Hello mask,
   localCipherMask())` (empty -> VersionMismatch + close), HelloAck encoded once
   and those exact bytes both sent and fed to `Handshake::begin`. A **fresh
   Handshake object per authentication attempt** (AuthProof, each CPace share)
   rebuilt from the kept Hello/HelloAck bytes + ephemeral secret, so a failed
   proof or a wrong code never leaves state behind. CodeShown/WrongCode carry
   `Yb` (new randomness after every wrong code; a repeated PairBegin resends the
   same code and share). Wrong confirmation or invalid share = wrong code (same
   global accounting, lockout, expiry, cancel as ADR 0003). Success stores
   `newPairingKey()`; v2 keys keep working (same store).
2. **One owner thread per AEAD direction**, handed over once:
   - c2p TCP + UDP openers -> `ClientConn::rx`, published with
     `rxSealed.store(release)`; the net thread (`owc-net`) opens every TCP record
     before the Ping fast path / worker forwarding and every punch.
   - p2c TCP + UDP sealers -> `Sender::setSessionKeys`, adopted by `owc-send`
     under its mutex together with the next item it pops.
   - Order in `Worker::authenticated()`: openers -> sealers -> `sessionId`
     (release; the net thread reads it first for punches, so a non-zero id
     implies the opener is visible) -> AuthResult{Ok, phoneConfirm} queued with
     `startSealing`. The Ok itself is plain; the sender seals everything written
     after it. Pongs/PunchAcks never jump ahead of a queued Ok.
3. **Sealing on the sender thread, in the existing buffers**: TCP VideoFrame =
   `TcpSealer::seal2(header, AU)` into a reused `sealBuf_` (encrypt after one
   copy, replacing the plain path's writev); UDP datagrams are sealed in place in
   the sendmmsg batch store right before sending (fragmentation with
   `kUdpMaxPayloadSealed`, FEC shard size follows); PunchAck moved from the net
   thread to the sender because only the sender owns the p2c UDP counter.
4. **Failure policy**: a record that does not open, or any non-Sealed message
   after the switch, drops the connection (logged locally, never keys); a punch
   that does not open is ignored (nobody can redirect the stream).
5. **Secrets**: Handshake/Aead destructors wipe; the Worker wipes the ephemeral
   secret, CPace share, confirmations, SessionKeys copies and the code
   (`secureWipe`) at authentication / session end.
6. **Observability**: status JSON `cipher`, `sealUsAvg`/`sealUsMax` (AEAD time per
   video frame measured with steady_clock around the seal calls only: one pair
   of clock reads per TCP frame or per sendmmsg batch of <= 40 datagrams); the
   5 s latency breakdown log adds "seal N us/frame". MainActivity shows
   "Criptografia: ..." when connected.

## Trade-offs
- Sealed TCP video costs one copy of the AU into `sealBuf_` (plain uses writev
  from the encoder buffer). The contract's `seal2` fuses this with the record
  layout; true zero-copy would need in-place encryption of the encoder buffer
  plus a scatter AEAD API, not worth it next to the encode time.
- `UdpSealer::seal` memmoves each payload by 8 bytes for the counter (~1.4 KB per
  datagram). A `sealInto(header, payload)` API could fold this into the batch
  copy; left to the protocol owner.
- PunchAck now waits behind whatever the sender is writing (at most one batch or
  one TCP frame); punches repeat every second, so this does not affect setup.
- A PC that sends anything between its proof and our AuthResult may have it
  arrive after the opener is installed and lose the connection. The contract
  forbids it; the Windows client does not ping before authentication.
- Fresh Handshake per attempt costs one extra X25519 per AuthProof / CPace try
  (sub-millisecond on the target phones) in exchange for no cross-attempt state.
