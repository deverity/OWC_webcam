// Small Win32 helpers: paths, UTF-8, registry probes, file IO.
#pragma once
#include <string>
#include <vector>

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

// Native file dialogs (COM must be initialised on the calling thread). `filterName`
// / `filterSpec` are e.g. L"LUT (*.cube)" / L"*.cube". Paths are UTF-8. Return false
// on cancel or failure.
bool pickOpenFile(void* owner, const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec,
                  std::string& out);
bool pickSaveFile(void* owner, const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec,
                  const wchar_t* defaultExt, const std::wstring& defaultName, std::string& out);

// Native folder picker (UTF-8 path). Returns false on cancel or failure.
bool pickFolder(void* owner, const wchar_t* title, std::string& out);
// Files directly inside `dir` (non-recursive) whose extension matches `ext`
// (case-insensitive, e.g. L".cube"), as UTF-8 absolute paths sorted by name.
std::vector<std::string> listFiles(const std::string& dir, const wchar_t* ext);

// DirectShow virtual camera registered (HKCU or HKLM Instance key)?
bool virtualCamRegistered(bool* perUser = nullptr);
// OBS plugin folder present?
bool obsPluginInstalled();

// Local time "HH:MM:SS.mmm" from ms since Unix epoch (UTC).
std::string formatClock(long long unixMs);
// Local date/time "dd/mm/aaaa HH:MM" from seconds since Unix epoch (UTC).
std::string formatDateTime(long long unixSeconds);

// DPAPI, current user, no UI (CryptProtectData / CryptUnprotectData). `entropy`
// binds a blob to its purpose: unprotecting with another value fails. false on
// failure (e.g. a blob from another Windows user or machine).
bool protectForUser(const std::vector<unsigned char>& plain, const std::string& entropy,
                    std::vector<unsigned char>& blob);
bool unprotectForUser(const std::vector<unsigned char>& blob, const std::string& entropy,
                      std::vector<unsigned char>& plain);

}  // namespace owcapp
