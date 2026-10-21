// Persistent pairing store (protocol.h "PAIRING"): this phone's identity
// (phone_id, generated once) and the PCs paired with it {client_id, name, key,
// paired_at}. Lives in the app's files dir (set from Kotlin through
// nativeSetDataDir before nativeStart) and is usable while the core is stopped:
// the UI lists/removes paired PCs at any time.
//
// Thread-safety: every public method locks mu_; callers are the JNI thread
// (list/remove/open) and the worker thread (lookup/add). File I/O happens under
// the lock (the file is a few hundred bytes).
//
// File "owc_pairing.txt" (text, written atomically: temp file + fsync + rename,
// mode 0600 inside the app-private dir):
//   OWC-PAIRING 1
//   phone_id <32 hex>
//   client <32 hex id> <64 hex key> <paired_at unix s> <name: rest of the line>
// Names are sanitized on insert (valid UTF-8 without 4-byte sequences — JNI
// NewStringUTF takes modified UTF-8 —, no control characters, <= 64 bytes), so
// a line never contains '\n'. Unknown/malformed lines are skipped.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "owc/protocol.h"

namespace owc {

struct PairedClient {
  Id16 id{};
  std::string name;
  Key32 key{};
  int64_t pairedAt = 0;  // unix seconds
};

class PairingStore {
 public:
  static constexpr size_t kMaxClients = 32;  // oldest pairing dropped beyond this
  static constexpr const char* kFileName = "owc_pairing.txt";

  // Loads the store from `dir` (creating phone_id + file on first use).
  // Idempotent for the same dir; a different dir reloads. False + err when the
  // file cannot be created (the store then stays closed).
  bool open(const std::string& dir, std::string& err);
  bool isOpen() const;

  Id16 phoneId() const;
  bool contains(const Id16& client) const;
  bool keyFor(const Id16& client, Key32& key) const;
  // Inserts or replaces the pairing for `client` and persists. Returns false if
  // the file could not be written (the pairing is still kept in memory).
  bool add(const Id16& client, const std::string& name, const Key32& key, int64_t pairedAt);
  // idHex = "" removes every PC. Returns false on a bad id or if the file could
  // not be written; `removed` = number of entries dropped.
  bool remove(const std::string& idHex, int& removed);
  // [{"id":"<hex>","name":"...","pairedAt":<unix s>}, ...] — never includes keys.
  std::string listJson() const;
  size_t size() const;

 private:
  bool loadLocked(std::string& err);
  bool saveLocked(std::string& err) const;

  mutable std::mutex mu_;
  std::string dir_, path_;
  bool open_ = false;
  Id16 phoneId_{};
  std::vector<PairedClient> clients_;
};

// Valid UTF-8 (3-byte max; others -> '?'), no control chars, trimmed, <= maxBytes
// at a character boundary; "PC" if nothing is left.
std::string sanitizePeerName(const std::string& in, size_t maxBytes = 64);

}  // namespace owc
