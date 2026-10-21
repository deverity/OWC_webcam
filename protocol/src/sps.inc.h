// SPS parse/patch (H.264 + HEVC). Included INSIDE namespace owc by protocol.cpp.
// Original implementation written from the H.264 (7.3.2.1.1, E.1.1) and HEVC
// (7.3.2.2.1, 7.3.3) syntax tables.

namespace spsimpl {

struct BitR {
  const uint8_t* d;
  size_t nbits;
  size_t pos = 0;
  bool ok = true;
  BitR(const uint8_t* data, size_t nbytes) : d(data), nbits(nbytes * 8) {}
  uint32_t bit() {
    if (pos >= nbits) { ok = false; return 0; }
    uint32_t v = (d[pos >> 3] >> (7 - (pos & 7))) & 1u;
    ++pos;
    return v;
  }
  uint32_t bits(unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) v = (v << 1) | bit();
    return v;
  }
  void skip(size_t n) {
    if (nbits - pos < n) { ok = false; pos = nbits; } else pos += n;
  }
  uint32_t ue() {
    unsigned lz = 0;
    while (ok && bit() == 0) {
      if (++lz > 31) { ok = false; return 0; }
    }
    if (!ok) return 0;
    if (lz == 0) return 0;
    uint32_t v = bits(lz);
    return ((1u << lz) - 1u) + v;
  }
  int32_t se() {
    uint32_t k = ue();
    int32_t m = static_cast<int32_t>((k + 1u) >> 1);
    return (k & 1u) ? m : -m;
  }
};

struct BitW {
  std::vector<uint8_t> b;
  size_t nbits = 0;
  void bit(uint32_t v) {
    if ((nbits & 7) == 0) b.push_back(0);
    if (v & 1u) b.back() = static_cast<uint8_t>(b.back() | (0x80u >> (nbits & 7)));
    ++nbits;
  }
  void bits(uint32_t v, unsigned n) {
    for (unsigned i = n; i > 0; --i) bit((v >> (i - 1)) & 1u);
  }
  void ue(uint32_t v) {
    uint64_t x = static_cast<uint64_t>(v) + 1u;
    unsigned len = 0;
    while ((x >> len) != 0) ++len;
    for (unsigned i = 1; i < len; ++i) bit(0);
    for (unsigned i = len; i > 0; --i) bit(static_cast<uint32_t>((x >> (i - 1)) & 1u));
  }
  void copy(const uint8_t* d, size_t from, size_t to) {  // bits [from,to)
    for (size_t i = from; i < to; ++i) bit((d[i >> 3] >> (7 - (i & 7))) & 1u);
  }
};

inline void unescape(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
  out.clear();
  out.reserve(n);
  unsigned zeros = 0;
  for (size_t i = 0; i < n; ++i) {
    if (zeros >= 2 && p[i] == 3) { zeros = 0; continue; }
    out.push_back(p[i]);
    zeros = p[i] == 0 ? zeros + 1 : 0;
  }
}
inline void escape(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
  unsigned zeros = 0;
  for (uint8_t v : in) {
    if (zeros >= 2 && v <= 3) { out.push_back(3); zeros = 0; }
    out.push_back(v);
    zeros = v == 0 ? zeros + 1 : 0;
  }
}
// Bit index of the rbsp stop bit (last 1 bit); false if none.
inline bool stopBit(const std::vector<uint8_t>& r, size_t& pos) {
  size_t i = r.size();
  while (i > 0 && r[i - 1] == 0) --i;
  if (i == 0) return false;
  uint8_t v = r[i - 1];
  unsigned t = 0;
  while (((v >> t) & 1u) == 0) ++t;
  pos = (i - 1) * 8 + (7 - t);
  return true;
}

inline void skipScalingList(BitR& r, unsigned size) {
  int32_t last = 8, next = 8;
  for (unsigned j = 0; j < size && r.ok; ++j) {
    if (next != 0) {
      int32_t delta = r.se();
      next = (last + delta + 256) % 256;
    }
    last = (next == 0) ? last : next;
  }
}
inline void skipHrd(BitR& r) {
  uint32_t cnt = r.ue();
  if (cnt > 31) { r.ok = false; return; }
  r.skip(8);
  for (uint32_t i = 0; i <= cnt && r.ok; ++i) { r.ue(); r.ue(); r.bit(); }
  r.skip(20);
}

struct H264Parsed {
  SpsInfo info;
  size_t pos_vui_flag = 0, pos_vui_start = 0, pos_br_flag = 0, pos_after_vui = 0, stop = 0;
  bool br = false;
  uint32_t mv_flag = 1, max_bytes = 0, max_bits = 0, log2h = 15, log2v = 15;
};

inline bool parseH264Rbsp(const std::vector<uint8_t>& rbsp, H264Parsed& P) {
  BitR r(rbsp.data(), rbsp.size());
  SpsInfo& in = P.info;
  in = SpsInfo();
  in.profile_idc = static_cast<uint8_t>(r.bits(8));
  r.skip(8);
  in.level_idc = static_cast<uint8_t>(r.bits(8));
  r.ue();  // sps id
  uint32_t chroma = 1;
  bool separate = false;
  switch (in.profile_idc) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118:
    case 128: case 138: case 139: case 134: case 135: {
      chroma = r.ue();
      if (chroma > 3) return false;
      if (chroma == 3) separate = r.bit() != 0;
      r.ue(); r.ue(); r.bit();
      if (r.bit()) {
        unsigned n = chroma != 3 ? 8 : 12;
        for (unsigned i = 0; i < n && r.ok; ++i)
          if (r.bit()) skipScalingList(r, i < 6 ? 16 : 64);
      }
      break;
    }
    default: break;
  }
  r.ue();  // log2_max_frame_num_minus4
  in.pic_order_cnt_type = r.ue();
  if (in.pic_order_cnt_type == 0) {
    r.ue();
  } else if (in.pic_order_cnt_type == 1) {
    r.bit(); r.se(); r.se();
    uint32_t n = r.ue();
    if (n > 255) return false;
    for (uint32_t i = 0; i < n && r.ok; ++i) r.se();
  } else if (in.pic_order_cnt_type != 2) {
    return false;
  }
  in.num_ref_frames = r.ue();
  r.bit();
  uint32_t wmb = r.ue(), hmap = r.ue();
  uint32_t fmo = r.bit();
  if (!fmo) r.bit();
  r.bit();
  uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
  if (r.bit()) { cl = r.ue(); cr = r.ue(); ct = r.ue(); cb = r.ue(); }
  if (!r.ok) return false;
  uint32_t cat = separate ? 0u : chroma;
  uint32_t cux = (cat == 1 || cat == 2) ? 2u : 1u;
  uint32_t cuy = ((cat == 1) ? 2u : 1u) * (2u - fmo);
  uint64_t w = (static_cast<uint64_t>(wmb) + 1u) * 16u;
  uint64_t h = (static_cast<uint64_t>(hmap) + 1u) * (2u - fmo) * 16u;
  uint64_t cw = (static_cast<uint64_t>(cl) + cr) * cux, ch = (static_cast<uint64_t>(ct) + cb) * cuy;
  if (cw >= w || ch >= h || w > 65535 || h > 65535) return false;
  in.width = static_cast<uint32_t>(w - cw);
  in.height = static_cast<uint32_t>(h - ch);
  P.pos_vui_flag = r.pos;
  in.vui_present = r.bit() != 0;
  if (!r.ok) return false;
  P.pos_vui_start = r.pos;
  if (in.vui_present) {
    if (r.bit()) { if (r.bits(8) == 255) r.skip(32); }
    if (r.bit()) r.bit();
    in.video_signal_type_present = r.bit() != 0;
    if (in.video_signal_type_present) {
      r.skip(3);  // video_format
      in.video_full_range = r.bit() != 0;
      in.colour_description_present = r.bit() != 0;
      if (in.colour_description_present) {
        in.colour_primaries = static_cast<uint8_t>(r.bits(8));
        in.transfer_characteristics = static_cast<uint8_t>(r.bits(8));
        in.matrix_coefficients = static_cast<uint8_t>(r.bits(8));
      }
    }
    if (r.bit()) { r.ue(); r.ue(); }
    if (r.bit()) r.skip(65);
    bool nal = r.bit() != 0;
    if (nal) skipHrd(r);
    bool vcl = r.bit() != 0;
    if (vcl) skipHrd(r);
    if (nal || vcl) r.bit();
    r.bit();  // pic_struct_present_flag
    P.pos_br_flag = r.pos;
    P.br = r.bit() != 0;
    if (P.br) {
      P.mv_flag = r.bit(); P.max_bytes = r.ue(); P.max_bits = r.ue();
      P.log2h = r.ue(); P.log2v = r.ue();
      in.max_num_reorder_frames = r.ue();
      in.max_dec_frame_buffering = r.ue();
    }
    in.bitstream_restriction = P.br;
  }
  if (!r.ok) return false;
  P.pos_after_vui = r.pos;
  if (!stopBit(rbsp, P.stop) || P.stop < P.pos_after_vui) return false;
  return true;
}

inline bool parseHevcPtl(BitR& r, unsigned maxSubMinus1) {
  r.skip(96);
  bool pp[8] = {}, lp[8] = {};
  for (unsigned i = 0; i < maxSubMinus1; ++i) { pp[i] = r.bit() != 0; lp[i] = r.bit() != 0; }
  if (maxSubMinus1 > 0)
    for (unsigned i = maxSubMinus1; i < 8; ++i) r.skip(2);
  for (unsigned i = 0; i < maxSubMinus1; ++i) {
    if (pp[i]) r.skip(88);
    if (lp[i]) r.skip(8);
  }
  return r.ok;
}

struct HevcParsed {
  SpsInfo info;
  struct Entry { size_t begin, end; uint32_t dec, reorder, lat; };
  std::vector<Entry> entries;
  size_t stop = 0;
};

inline bool parseHevcRbsp(const std::vector<uint8_t>& rbsp, HevcParsed& P) {
  BitR r(rbsp.data(), rbsp.size());
  P.info = SpsInfo();
  P.entries.clear();
  r.skip(4);
  unsigned maxSub = r.bits(3);
  r.bit();
  if (maxSub > 6) return false;
  P.info.profile_idc = static_cast<uint8_t>(0);
  if (!r.ok) return false;
  size_t ptl_start = r.pos;
  if (!parseHevcPtl(r, maxSub)) return false;
  // general_profile_idc is bits 3..7 of the first PTL byte; level is the last PTL byte of general part
  P.info.profile_idc = static_cast<uint8_t>(rbsp[ptl_start >> 3] & 0x1Fu);
  {
    size_t lp = ptl_start + 88;
    P.info.level_idc = static_cast<uint8_t>(rbsp[lp >> 3]);
  }
  r.ue();  // sps id
  uint32_t chroma = r.ue();
  if (chroma > 3) return false;
  if (chroma == 3) r.bit();
  r.ue(); r.ue();
  if (r.bit()) { r.ue(); r.ue(); r.ue(); r.ue(); }
  r.ue(); r.ue(); r.ue();
  bool present = r.bit() != 0;
  if (!r.ok) return false;
  for (unsigned i = present ? 0u : maxSub; i <= maxSub; ++i) {
    HevcParsed::Entry e;
    e.begin = r.pos;
    e.dec = r.ue(); e.reorder = r.ue(); e.lat = r.ue();
    e.end = r.pos;
    if (!r.ok) return false;
    P.entries.push_back(e);
  }
  const HevcParsed::Entry& last = P.entries.back();
  P.info.max_num_reorder_frames = last.reorder;
  P.info.max_dec_frame_buffering = last.dec + 1;
  P.info.max_latency_increase_plus1 = last.lat;
  P.info.vui_present = false;
  if (!stopBit(rbsp, P.stop) || P.stop < last.end) return false;
  return true;
}

}  // namespace spsimpl

bool parseH264Sps(const uint8_t* nal, size_t len, SpsInfo& info) {
  if (!nal || len < 4 || (nal[0] & 0x1Fu) != 7) return false;
  std::vector<uint8_t> rbsp;
  spsimpl::unescape(nal + 1, len - 1, rbsp);
  spsimpl::H264Parsed P;
  if (!spsimpl::parseH264Rbsp(rbsp, P)) return false;
  info = P.info;
  return true;
}

bool parseHevcSps(const uint8_t* nal, size_t len, SpsInfo& info) {
  if (!nal || len < 6 || ((nal[0] >> 1) & 0x3Fu) != 33) return false;
  std::vector<uint8_t> rbsp;
  spsimpl::unescape(nal + 2, len - 2, rbsp);
  spsimpl::HevcParsed P;
  if (!spsimpl::parseHevcRbsp(rbsp, P)) return false;
  info = P.info;
  return true;
}

bool patchH264Sps(const uint8_t* nal, size_t len, std::vector<uint8_t>& out, bool has_bframes) {
  if (!nal || len < 4 || (nal[0] & 0x1Fu) != 7) return false;
  std::vector<uint8_t> rbsp;
  spsimpl::unescape(nal + 1, len - 1, rbsp);
  spsimpl::H264Parsed P;
  if (!spsimpl::parseH264Rbsp(rbsp, P)) return false;
  if (has_bframes) { out.assign(nal, nal + len); return true; }
  spsimpl::BitW w;
  const uint8_t* d = rbsp.data();
  w.copy(d, 0, P.pos_vui_flag);
  w.bit(1);
  if (P.info.vui_present) w.copy(d, P.pos_vui_start, P.pos_br_flag);
  else w.bits(0, 8);
  w.bit(1);
  w.bit(P.mv_flag);
  w.ue(P.max_bytes);
  w.ue(P.max_bits);
  w.ue(P.log2h);
  w.ue(P.log2v);
  w.ue(0);
  w.ue(std::max<uint32_t>(1u, P.info.num_ref_frames));
  w.copy(d, P.pos_after_vui, P.stop + 1);
  std::vector<uint8_t> res;
  res.push_back(nal[0]);
  spsimpl::escape(w.b, res);
  out.swap(res);
  return true;
}

bool patchHevcSps(const uint8_t* nal, size_t len, std::vector<uint8_t>& out, bool has_bframes) {
  if (!nal || len < 6 || ((nal[0] >> 1) & 0x3Fu) != 33) return false;
  std::vector<uint8_t> rbsp;
  spsimpl::unescape(nal + 2, len - 2, rbsp);
  spsimpl::HevcParsed P;
  if (!spsimpl::parseHevcRbsp(rbsp, P)) return false;
  if (has_bframes) { out.assign(nal, nal + len); return true; }
  spsimpl::BitW w;
  const uint8_t* d = rbsp.data();
  w.copy(d, 0, P.entries.front().begin);
  for (const auto& e : P.entries) { w.ue(e.dec); w.ue(0); w.ue(0); }
  w.copy(d, P.entries.back().end, P.stop + 1);
  std::vector<uint8_t> res;
  res.push_back(nal[0]);
  res.push_back(nal[1]);
  spsimpl::escape(w.b, res);
  out.swap(res);
  return true;
}

size_t patchAnnexBSps(const uint8_t* data, size_t len, Codec codec, std::vector<uint8_t>& out,
                      bool has_bframes) {
  out.clear();
  if (!data || len == 0) return 0;
  auto findSc = [&](size_t from) -> size_t {  // index of first 00 of a 00 00 01, or len
    for (size_t i = from; i + 2 < len; ++i)
      if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) return i;
    return len;
  };
  size_t count = 0;
  size_t sc = findSc(0);
  out.insert(out.end(), data, data + sc);
  std::vector<uint8_t> tmp;
  while (sc < len) {
    size_t nb = sc + 3;
    size_t next = findSc(nb);
    size_t e = next;
    while (e > nb && data[e - 1] == 0) --e;
    out.insert(out.end(), data + sc, data + nb);
    bool patched = false;
    if (e > nb) {
      if (codec == Codec::H264 && (data[nb] & 0x1Fu) == 7)
        patched = patchH264Sps(data + nb, e - nb, tmp, has_bframes);
      else if (codec == Codec::Hevc && ((data[nb] >> 1) & 0x3Fu) == 33)
        patched = patchHevcSps(data + nb, e - nb, tmp, has_bframes);
    }
    if (patched) {
      out.insert(out.end(), tmp.begin(), tmp.end());
      ++count;
    } else {
      out.insert(out.end(), data + nb, data + e);
    }
    out.insert(out.end(), data + e, data + next);
    sc = next;
  }
  return count;
}
