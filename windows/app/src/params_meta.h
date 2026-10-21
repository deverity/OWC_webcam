// pt-BR metadata (labels, tooltips, formatting) for the known ParamIds.
#pragma once
#include <string>
#include <vector>

#include <owc/protocol.h>

namespace owcapp {

enum class PFmt { Plain, Mbps, MsFromNs, Diopter, ZoomX, Percent, Ms, Frames, Signed };

struct PMeta {
  owc::ParamId id;
  const char* label;
  const char* tip;
  PFmt fmt;
};

const PMeta* findMeta(owc::ParamId id);
// Label for any id (known -> pt-BR, unknown -> "Parâmetro #N (name)").
std::string paramLabel(owc::ParamId id);

const std::vector<owc::ParamId>& cameraGroup();   // live camera controls
const std::vector<owc::ParamId>& encoderGroup();  // codec / network / latency controls
bool isSpecialParam(owc::ParamId id);             // handled by dedicated widgets

}  // namespace owcapp
