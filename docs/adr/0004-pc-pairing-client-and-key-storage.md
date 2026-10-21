# ADR 0004: PC side of mandatory pairing (owc_core auth phase, OWC.exe key storage)

Status: accepted (2026-10-21)
Contract: `protocol/include/owc/protocol.h` "PAIRING" (wire v2). Phone side: ADR 0003.
Code: `windows/core/include/owc/client.h` (PAIRING section, `PairingInfo`, `ClientState::Pairing`,
`DisconnectReason::Pairing*/NotAuthorized/NotPaired`), `windows/core/src/client.cpp`
(`Impl::authenticate`), `windows/core/src/pairing.{h,cpp}`, `windows/app/src/settings.cpp`
(DPAPI), `app.cpp`, `ui.cpp` (modal), `ui_params.cpp` ("Celulares pareados"),
`windows/tools/fakephone` (phone emulation), `windows/core/tests/integration_test.cpp`.

## Decision
1. **Auth is a separate phase of `runSession`** (`authenticate()`), between the HelloAck
   checks and "session up". It never pings, never applies `idle_timeout_ms`; it is
   bounded by `kAuthTimeoutS` per request and, in state `Pairing`, by the code lifetime
   from CodeShown + 5 s. Session state (DeviceInfo, onConnected, punches, StartStream)
   starts only after AuthResult{Ok}; messages after Ok stay queued in the parser.
2. **owc_core stores nothing**: `ConnectParams{client_id, paired[]}` in, events out
   (`onPairing`, `onPaired`, `onKeyRejected`). The client keeps its own copy of
   `paired` (updated on pairing / rejection / `forgetPairing`) so auto-reconnects use
   the current key without the app re-calling `connect()`.
3. **Who may start a pairing**: only a `connect()`, once, in its first authentication
   (attempts that end before an AuthRequired HelloAck, e.g. connect refused or Busy,
   do not count). Auto-reconnects only send AuthProof; if the phone no longer knows
   the PC they end in `Failed/NotPaired` (after `onKeyRejected`). Any session that
   ends after a PairBegin, and every pairing failure, is never auto-reconnected.
4. **Version check before status**: a v1 HelloAck (status Ok, zero nonce) is
   `VersionMismatch`; a v2 status other than AuthRequired is `ProtocolError`.
5. **OWC.exe storage**: `client_id` (newClientId = BCryptGenRandom) and
   `paired_phones{phone_id hex -> DPAPI(key) hex, label, paired_at}` in settings.json.
   DPAPI current-user with per-entry entropy ("OWC pairing key v1|<phone hex>");
   undecryptable entries are dropped on load. Pairing changes are saved on the next
   UI tick (DPAPI + file I/O never run on owc-net).

## Trade-offs
- (3) A user who clicks Connect while the phone is unreachable still gets the code once
  it appears (first AuthRequired HelloAck), but a dropped *authenticated* session whose
  PC was removed on the phone stops instead of popping a code: re-pairing needs a click.
- Failing without auto-reconnect after any post-PairBegin drop (Wi-Fi blip while the
  dialog is open) costs one extra click, but guarantees no automatic code loops.
- DPAPI ties keys to the Windows user/machine: copying settings.json elsewhere (or a
  roaming profile on another machine) loses the pairings (the user re-pairs), which is
  the intended security property.
- `connect()` rejects an all-zero client_id: an API break for callers, but a shared
  zero identity would make every PC "the same" paired PC.
