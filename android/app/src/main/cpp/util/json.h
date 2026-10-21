#pragma once

#include <cstdio>
#include <string>

namespace owc::json {

inline std::string quote(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  o.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof(b), "\\u%04x", c);
          o += b;
        } else {
          o.push_back(static_cast<char>(c));
        }
    }
  }
  o.push_back('"');
  return o;
}

}  // namespace owc::json
