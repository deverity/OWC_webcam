#include "pairing_store.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "util/json.h"
#include "util/log.h"

namespace owc {
namespace {

constexpr const char* kHeader = "OWC-PAIRING 1";

bool writeAll(int fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

bool isCont(unsigned char c) { return (c & 0xC0) == 0x80; }

}  // namespace

std::string sanitizePeerName(const std::string& in, size_t maxBytes) {
  std::string out;
  out.reserve(std::min(in.size(), maxBytes));
  const auto* p = reinterpret_cast<const unsigned char*>(in.data());
  const size_t n = in.size();
  size_t i = 0;
  while (i < n) {
    const unsigned char c = p[i];
    size_t len = 0;
    bool ok = false;
    if (c < 0x80) {
      len = 1;
      ok = c >= 0x20 && c != 0x7F;
      if (!ok && (c == '\t')) {  // tabs become spaces, other controls vanish
        if (out.size() + 1 > maxBytes) break;
        out.push_back(' ');
        ++i;
        continue;
      }
    } else if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      ok = i + 1 < n && isCont(p[i + 1]);
      if (ok && c == 0xC2 && p[i + 1] < 0xA0) {  // U+0080..U+009F (C1 controls): dropped
        i += 2;
        continue;
      }
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      ok = i + 2 < n && isCont(p[i + 1]) && isCont(p[i + 2]);
      if (ok && c == 0xE0 && p[i + 1] < 0xA0) ok = false;   // overlong
      if (ok && c == 0xED && p[i + 1] >= 0xA0) ok = false;  // surrogates
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;  // valid but not representable in JNI modified UTF-8: '?'
      ok = false;
      const bool wellFormed = i + 3 < n && isCont(p[i + 1]) && isCont(p[i + 2]) && isCont(p[i + 3]);
      if (wellFormed) {
        if (out.size() + 1 > maxBytes) break;
        out.push_back('?');
        i += 4;
        continue;
      }
      len = 1;
    } else {
      len = 1;  // stray continuation / invalid lead byte
    }
    if (!ok) {
      if (c >= 0x80) {
        if (out.size() + 1 > maxBytes) break;
        out.push_back('?');
      }
      i += 1;  // resync on the next byte
      continue;
    }
    if (out.size() + len > maxBytes) break;
    out.append(in, i, len);
    i += len;
  }
  // trim spaces
  size_t b = out.find_first_not_of(' ');
  if (b == std::string::npos) return "PC";
  size_t e = out.find_last_not_of(' ');
  return out.substr(b, e - b + 1);
}

bool PairingStore::open(const std::string& dir, std::string& err) {
  std::lock_guard<std::mutex> lk(mu_);
  if (dir.empty()) {
    err = "empty data dir";
    return false;
  }
  if (open_ && dir == dir_) return true;
  dir_ = dir;
  path_ = dir + "/" + kFileName;
  open_ = false;
  clients_.clear();
  phoneId_ = Id16{};
  if (!loadLocked(err)) return false;
  open_ = true;
  return true;
}

bool PairingStore::isOpen() const {
  std::lock_guard<std::mutex> lk(mu_);
  return open_;
}

bool PairingStore::loadLocked(std::string& err) {
  std::ifstream f(path_);
  bool haveId = false;
  if (f) {
    std::string line;
    bool headerOk = std::getline(f, line) && line == kHeader;
    if (headerOk) {
      while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("phone_id ", 0) == 0) {
          haveId = fromHex(line.substr(9), phoneId_.data(), kIdSize);
          continue;
        }
        if (line.rfind("client ", 0) != 0) continue;
        // client <id> <key> <pairedAt> <name...>
        std::istringstream is(line.substr(7));
        std::string idHex, keyHex;
        long long at = 0;
        if (!(is >> idHex >> keyHex >> at)) continue;
        std::string name;
        std::getline(is, name);
        PairedClient c;
        if (!fromHex(idHex, c.id.data(), kIdSize) || !fromHex(keyHex, c.key.data(), kKeySize)) continue;
        c.pairedAt = at;
        c.name = sanitizePeerName(name);
        auto dup = std::find_if(clients_.begin(), clients_.end(), [&](const PairedClient& o) { return o.id == c.id; });
        if (dup != clients_.end()) *dup = std::move(c);
        else clients_.push_back(std::move(c));
      }
    }
    if (!headerOk || !haveId) {
      // Unreadable store: keep a copy for diagnosis and start over. Every PC
      // will have to pair again (it sees a new phone_id).
      LOGE_LOCAL("pairing store %s is corrupt: starting a new one", path_.c_str());
      f.close();
      ::rename(path_.c_str(), (path_ + ".corrupt").c_str());
      clients_.clear();
      haveId = false;
    }
  }
  if (haveId) {
    LOGI("pairing store loaded: %zu paired PC(s)", clients_.size());
    return true;
  }
  arc4random_buf(phoneId_.data(), phoneId_.size());
  if (!saveLocked(err)) return false;
  LOGI("pairing store created (new phone id)");
  return true;
}

bool PairingStore::saveLocked(std::string& err) const {
  std::string s = std::string(kHeader) + "\nphone_id " + toHex(phoneId_.data(), kIdSize) + "\n";
  for (const auto& c : clients_) {
    char at[32];
    snprintf(at, sizeof(at), "%lld", static_cast<long long>(c.pairedAt));
    s += "client " + toHex(c.id.data(), kIdSize) + " " + toHex(c.key.data(), kKeySize) + " " + at + " " +
         c.name + "\n";
  }
  const std::string tmp = path_ + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    err = "cannot create " + tmp + ": " + strerror(errno);
    return false;
  }
  bool ok = writeAll(fd, s) && ::fsync(fd) == 0;
  const int werr = errno;
  ::close(fd);
  if (!ok || ::rename(tmp.c_str(), path_.c_str()) != 0) {
    err = "cannot write " + path_ + ": " + strerror(ok ? errno : werr);
    ::unlink(tmp.c_str());
    return false;
  }
  int dfd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {  // make the rename itself durable
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

Id16 PairingStore::phoneId() const {
  std::lock_guard<std::mutex> lk(mu_);
  return phoneId_;
}

bool PairingStore::contains(const Id16& client) const {
  std::lock_guard<std::mutex> lk(mu_);
  return std::any_of(clients_.begin(), clients_.end(), [&](const PairedClient& c) { return c.id == client; });
}

bool PairingStore::keyFor(const Id16& client, Key32& key) const {
  std::lock_guard<std::mutex> lk(mu_);
  for (const auto& c : clients_) {
    if (c.id == client) {
      key = c.key;
      return true;
    }
  }
  return false;
}

bool PairingStore::add(const Id16& client, const std::string& name, const Key32& key, int64_t pairedAt) {
  std::lock_guard<std::mutex> lk(mu_);
  clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](const PairedClient& c) { return c.id == client; }),
                 clients_.end());
  while (clients_.size() >= kMaxClients) {
    auto oldest = std::min_element(clients_.begin(), clients_.end(),
                                   [](const PairedClient& a, const PairedClient& b) { return a.pairedAt < b.pairedAt; });
    clients_.erase(oldest);
  }
  PairedClient c;
  c.id = client;
  c.name = sanitizePeerName(name);
  c.key = key;
  c.pairedAt = pairedAt;
  clients_.push_back(std::move(c));
  std::string err;
  if (!open_ || !saveLocked(err)) {
    LOGE_LOCAL("pairing store: %s", open_ ? err.c_str() : "not open");
    return false;
  }
  return true;
}

bool PairingStore::remove(const std::string& idHex, int& removed) {
  std::lock_guard<std::mutex> lk(mu_);
  removed = 0;
  if (!open_) return false;
  const size_t before = clients_.size();
  if (idHex.empty()) {
    clients_.clear();
  } else {
    Id16 id{};
    if (!fromHex(idHex, id.data(), kIdSize)) return false;
    clients_.erase(std::remove_if(clients_.begin(), clients_.end(), [&](const PairedClient& c) { return c.id == id; }),
                   clients_.end());
  }
  removed = static_cast<int>(before - clients_.size());
  if (removed == 0) return true;
  std::string err;
  if (!saveLocked(err)) {
    LOGE_LOCAL("pairing store: %s", err.c_str());
    return false;
  }
  return true;
}

std::string PairingStore::listJson() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::string o = "[";
  for (size_t i = 0; i < clients_.size(); ++i) {
    const auto& c = clients_[i];
    char at[32];
    snprintf(at, sizeof(at), "%lld", static_cast<long long>(c.pairedAt));
    if (i) o += ",";
    o += "{\"id\":" + json::quote(toHex(c.id.data(), kIdSize)) + ",\"name\":" + json::quote(c.name) +
         ",\"pairedAt\":" + at + "}";
  }
  o += "]";
  return o;
}

size_t PairingStore::size() const {
  std::lock_guard<std::mutex> lk(mu_);
  return clients_.size();
}

}  // namespace owc
