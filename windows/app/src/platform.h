// Small Win32 helpers: paths, UTF-8, registry probes, file IO.
#pragma once
#include <string>

namespace owcapp {

std::string toUtf8(const std::wstring& w);
std::wstring fromUtf8(const std::string& s);

std::wstring exePath();
std::wstring exeDir();
// %APPDATA%\OWC (created on demand).
std::wstring appDataDir();
// Looks for a relative path next to the exe and then in up to 6 ancestor
// directories (repo layout: build\app\out\Release -> repo root). "" if missing.
std::wstring findNearExe(const std::wstring& rel);

bool readTextFile(const std::wstring& path, std::string& out);
bool writeTextFileAtomic(const std::wstring& path, const std::string& data);

// DirectShow virtual camera registered (HKCU or HKLM Instance key)?
bool virtualCamRegistered(bool* perUser = nullptr);
// OBS plugin folder present?
bool obsPluginInstalled();

// Local time "HH:MM:SS.mmm" from ms since Unix epoch (UTC).
std::string formatClock(long long unixMs);

}  // namespace owcapp
