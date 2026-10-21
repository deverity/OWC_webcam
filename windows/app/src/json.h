// Minimal JSON value / parser / writer (UTF-8). Enough for settings.json and
// presets.json; not a general-purpose library.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace owcapp {

struct Json {
  enum class Type { Null, Bool, Num, Str, Arr, Obj };
  Type type = Type::Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;

  Json() = default;
  static Json object() { Json j; j.type = Type::Obj; return j; }
  static Json array() { Json j; j.type = Type::Arr; return j; }
  static Json boolean(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }
  static Json number(double v) { Json j; j.type = Type::Num; j.n = v; return j; }
  static Json string(std::string v) { Json j; j.type = Type::Str; j.s = std::move(v); return j; }

  bool isObj() const { return type == Type::Obj; }
  bool isArr() const { return type == Type::Arr; }

  Json& set(const std::string& k, Json v) {
    type = Type::Obj;
    for (auto& kv : o)
      if (kv.first == k) { kv.second = std::move(v); return kv.second; }
    o.emplace_back(k, std::move(v));
    return o.back().second;
  }
  void push(Json v) { type = Type::Arr; a.push_back(std::move(v)); }
  const Json* find(const std::string& k) const {
    if (type != Type::Obj) return nullptr;
    for (const auto& kv : o)
      if (kv.first == k) return &kv.second;
    return nullptr;
  }
  double num(const std::string& k, double def) const {
    const Json* j = find(k);
    return (j && j->type == Type::Num) ? j->n : def;
  }
  bool flag(const std::string& k, bool def) const {
    const Json* j = find(k);
    return (j && j->type == Type::Bool) ? j->b : def;
  }
  std::string str(const std::string& k, const std::string& def = std::string()) const {
    const Json* j = find(k);
    return (j && j->type == Type::Str) ? j->s : def;
  }

  std::string dump(int indent = 0) const {
    std::string out;
    write(out, indent, 0);
    out += '\n';
    return out;
  }

  static bool parse(const std::string& text, Json& out) {
    Parser p{text.data(), text.data() + text.size()};
    p.ws();
    if (!p.value(out, 0)) return false;
    p.ws();
    return p.cur == p.end;
  }

 private:
  static void esc(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
      switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
          else out += static_cast<char>(c);
      }
    }
    out += '"';
  }
  void write(std::string& out, int indent, int level) const {
    auto nl = [&](int lv) {
      if (indent > 0) { out += '\n'; out.append(static_cast<size_t>(lv * indent), ' '); }
    };
    switch (type) {
      case Type::Null: out += "null"; break;
      case Type::Bool: out += b ? "true" : "false"; break;
      case Type::Num: {
        char buf[40];
        if (n == static_cast<double>(static_cast<long long>(n)) && n > -1e15 && n < 1e15)
          std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(n));
        else
          std::snprintf(buf, sizeof buf, "%.9g", n);
        out += buf;
        break;
      }
      case Type::Str: esc(out, s); break;
      case Type::Arr:
        out += '[';
        for (size_t i = 0; i < a.size(); ++i) {
          if (i) out += ',';
          nl(level + 1);
          a[i].write(out, indent, level + 1);
        }
        if (!a.empty()) nl(level);
        out += ']';
        break;
      case Type::Obj:
        out += '{';
        for (size_t i = 0; i < o.size(); ++i) {
          if (i) out += ',';
          nl(level + 1);
          esc(out, o[i].first);
          out += indent > 0 ? ": " : ":";
          o[i].second.write(out, indent, level + 1);
        }
        if (!o.empty()) nl(level);
        out += '}';
        break;
    }
  }

  struct Parser {
    const char* cur;
    const char* end;
    void ws() { while (cur < end && (*cur == ' ' || *cur == '\t' || *cur == '\n' || *cur == '\r')) ++cur; }
    bool lit(const char* w) {
      size_t len = std::char_traits<char>::length(w);
      if (static_cast<size_t>(end - cur) < len || std::string(cur, len) != w) return false;
      cur += len;
      return true;
    }
    static void utf8(std::string& s, unsigned cp) {
      if (cp < 0x80) s += static_cast<char>(cp);
      else if (cp < 0x800) { s += static_cast<char>(0xC0 | (cp >> 6)); s += static_cast<char>(0x80 | (cp & 0x3F)); }
      else if (cp < 0x10000) {
        s += static_cast<char>(0xE0 | (cp >> 12)); s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
      } else {
        s += static_cast<char>(0xF0 | (cp >> 18)); s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); s += static_cast<char>(0x80 | (cp & 0x3F));
      }
    }
    bool hex4(unsigned& v) {
      if (end - cur < 4) return false;
      v = 0;
      for (int i = 0; i < 4; ++i) {
        char c = *cur++;
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
        else return false;
      }
      return true;
    }
    bool str(std::string& out) {
      if (cur >= end || *cur != '"') return false;
      ++cur;
      while (cur < end && *cur != '"') {
        char c = *cur++;
        if (c != '\\') { out += c; continue; }
        if (cur >= end) return false;
        char e = *cur++;
        switch (e) {
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            unsigned cp;
            if (!hex4(cp)) return false;
            if (cp >= 0xD800 && cp < 0xDC00 && end - cur >= 6 && cur[0] == '\\' && cur[1] == 'u') {
              cur += 2;
              unsigned lo;
              if (!hex4(lo)) return false;
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8(out, cp);
            break;
          }
          default: out += e;
        }
      }
      if (cur >= end) return false;
      ++cur;
      return true;
    }
    bool value(Json& j, int depth) {
      if (depth > 64 || cur >= end) return false;
      char c = *cur;
      if (c == '{') {
        ++cur; j = Json::object(); ws();
        if (cur < end && *cur == '}') { ++cur; return true; }
        for (;;) {
          ws();
          std::string k;
          if (!str(k)) return false;
          ws();
          if (cur >= end || *cur != ':') return false;
          ++cur; ws();
          Json v;
          if (!value(v, depth + 1)) return false;
          j.o.emplace_back(std::move(k), std::move(v));
          ws();
          if (cur < end && *cur == ',') { ++cur; continue; }
          if (cur < end && *cur == '}') { ++cur; return true; }
          return false;
        }
      }
      if (c == '[') {
        ++cur; j = Json::array(); ws();
        if (cur < end && *cur == ']') { ++cur; return true; }
        for (;;) {
          ws();
          Json v;
          if (!value(v, depth + 1)) return false;
          j.a.push_back(std::move(v));
          ws();
          if (cur < end && *cur == ',') { ++cur; continue; }
          if (cur < end && *cur == ']') { ++cur; return true; }
          return false;
        }
      }
      if (c == '"') { j.type = Type::Str; return str(j.s); }
      if (c == 't') { j = Json::boolean(true); return lit("true"); }
      if (c == 'f') { j = Json::boolean(false); return lit("false"); }
      if (c == 'n') { j = Json(); return lit("null"); }
      char* e = nullptr;
      double d = std::strtod(cur, &e);
      if (e == cur) return false;
      cur = e;
      j = Json::number(d);
      return true;
    }
  };
};

}  // namespace owcapp
