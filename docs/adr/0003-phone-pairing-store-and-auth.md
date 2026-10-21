# ADR 0003: Phone side of mandatory pairing (store, auth state machine, rate limits)

Status: accepted (2026-10-21)
Contract: `protocol/include/owc/protocol.h` "PAIRING" (wire v2).
Code: `android/app/src/main/cpp/pairing_store.{h,cpp}`, `session.{h,cpp}` (Worker auth),
`net/net_server.cpp` (Ping gate), `jni_bridge.cpp`, Kotlin `Pairing.kt`, `PairingActivity.kt`,
`WebcamService.kt`, `MainActivity.kt`. Self-test: `cpp/selftest/selftest.cpp` (`full`).

## Context
Any PC on the network (or any local app via 127.0.0.1) could connect and take the
camera. The contract adds HelloAck{AuthRequired} + AuthProof / PairBegin-PairCode
before a session starts. The phone must persist its identity and the paired PCs,
let the user list/remove them while the core is stopped, and show a 6-digit code.

## Decision
1. **Store**: one text file `filesDir/owc_pairing.txt` (header, `phone_id`, one
   `client <id> <key> <paired_at> <name>` line per PC, max 32), written atomically
   (temp + fsync + rename + dir fsync, mode 0600). Owned by `AppCore` (shared_ptr,
   own mutex) so JNI list/remove work without a running worker. Set via
   `nativeSetDataDir`; `nativeStart` fails without it (fail loudly instead of a
   silently non-persistent pairing). Corrupt file -> renamed `.corrupt`, new phone_id.
2. **Auth state in the Worker** (single-threaded, no new locks): `helloSeen_`
   (HelloAck sent) and `helloDone_` (= authenticated, session up). The UDP
   `session_id` is generated at Hello but published to `ClientConn::sessionId`
   only after AuthResult{Ok}; the net thread answers Punch and Ping only when it
   is non-zero. PC log sink, Capabilities, Stats, CLIENT_CONNECTED, camera
   refresh: all moved to `authenticated()`.
3. **Timeouts**: Hello 5 s (unchanged); after Hello no silence timeout, instead
   `kAuthTimeoutS`; once a code is shown the deadline becomes the code expiry
   (`kPairCodeLifetimeS`) -> AuthResult{Expired} + close.
4. **Rate limiting**: lockout (`kPairLockoutS`) is global in the Worker. Wrong
   codes are counted globally until a lockout or a success (forgotten after
   10 min), so a new code may start with fewer than `kPairCodeAttempts`
   attempts; `attempts_left` in CodeShown carries the real budget. Repeated
   PairBegin on one connection returns the same code (no fresh attempts).
5. **Unpair / cancel** go through the worker queue (`Event::PairingChanged`,
   `Event::CancelPairing`). On PairingChanged the worker re-checks
   `store.contains(clientId_)` rather than comparing ids, so a removal racing
   with a re-pairing resolves to the store's final state. A removed connected PC
   gets Bye + close (CLIENT_DISCONNECTED arg 1).
6. **App events** `PAIRING_CODE` (8: int = lifetime s, str = "code\nname") and
   `PAIRING_ENDED` (9: int = paired/failed/expired/rejected/disconnected).
   Kotlin keeps the pending code in `Pairing` (main thread) and fans it out to a
   heads-up notification (high-importance channel, VISIBILITY_PUBLIC, full-screen
   intent to `PairingActivity` when allowed), the MainActivity dialog, and, if the
   black screen was active, closes it and opens `PairingActivity`.
7. Codes and keys are never logged (logcat or PC); PC names are sanitized
   (valid 1-3 byte UTF-8 for JNI `NewStringUTF`, no controls, <= 64 bytes).

## Trade-offs
- Plain-text keys in app-private storage (no Android Keystore): simple, survives
  Keystore resets on old devices; protected by the app sandbox and
  `allowBackup=false`, not against root. Keystore wrapping can be added later
  without wire changes.
- Global failure counter is stricter than "attempts per code": a legitimate user
  after two typos and a reconnect gets 1 attempt; in exchange online brute force
  drops from ~2 guesses per reconnect (unbounded) to 3 guesses per 30 s.
- An unauthenticated connection still occupies the single client slot for up to
  10 s (120 s with a code shown). A LAN attacker can keep the phone "busy" (DoS)
  but cannot get video; accepting parallel unauthenticated connections would
  need a multi-client net loop.
- Showing the code on the lock screen (VISIBILITY_PUBLIC) favours usability: the
  threat is a network attacker, not someone holding the phone.
