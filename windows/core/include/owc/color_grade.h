// Colour grading helpers shared by the client and the UI: built-in looks,
// .cube LUT parsing and export of the current grade as a .cube file.
// The per-frame grading itself is internal to owc::Client (ClientOptions::color).
#pragma once

#include <string>
#include <vector>

#include <owc/client.h>

namespace owc {

// A built-in look, selected with ColorGrade::lut = "builtin:" + key.
struct BuiltinLook {
  const char* key;          // stable id, e.g. "teal_orange"
  const char* name;         // UI name (pt-BR), e.g. "Cinema (teal & orange)"
  const char* description;  // one line (pt-BR)
};
const std::vector<BuiltinLook>& builtinLooks();

// A parsed .cube LUT. 3D: size^3 RGB triplets, red changing fastest (the
// .cube order). 1D: size RGB triplets. Values are kept as written (they are
// mapped through domain_min/max on lookup).
struct CubeLut {
  std::string title;
  bool is3d = true;
  int size = 0;
  float domain_min[3] = {0, 0, 0};
  float domain_max[3] = {1, 1, 1};
  std::vector<float> rgb;  // 3 * (is3d ? size^3 : size)
};

// Parses a .cube file (UTF-8 path). Accepts TITLE, LUT_3D_SIZE (2..256),
// LUT_1D_SIZE (2..65536), DOMAIN_MIN/MAX, LUT_3D_INPUT_RANGE /
// LUT_1D_INPUT_RANGE, comments (#) and blank lines. On failure returns false
// and sets `err` (pt-BR, with the line number when relevant).
bool loadCubeFile(const std::string& path, CubeLut& out, std::string& err);
bool parseCube(const std::string& text, CubeLut& out, std::string& err);

// Writes the whole grade (adjustments + LUT at its strength) as a 3D .cube of
// `size` points per axis, in full-range RGB, so it can be used in OBS,
// DaVinci Resolve, Premiere, etc. Ignores `enabled` and `compare_split`.
bool exportCube(const ColorGrade& g, const std::string& path, int size, std::string& err);

// True when the grade changes nothing (disabled, or neutral values and no LUT
// or strength 0).
bool isIdentity(const ColorGrade& g);

}  // namespace owc
