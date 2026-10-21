#include "platform.h"

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace owcapp {

std::string toUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring fromUtf8(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
  return w;
}

std::wstring exePath() {
  wchar_t buf[MAX_PATH * 2];
  DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
  return std::wstring(buf, n);
}

std::wstring exeDir() {
  std::wstring p = exePath();
  size_t k = p.find_last_of(L"\\/");
  return k == std::wstring::npos ? p : p.substr(0, k);
}

std::wstring appDataDir() {
  PWSTR roaming = nullptr;
  std::wstring dir;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) && roaming) {
    dir = roaming;
    CoTaskMemFree(roaming);
  } else {
    dir = exeDir();
  }
  dir += L"\\OWC";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring findNearExe(const std::wstring& rel) {
  std::wstring dir = exeDir();
  for (int i = 0; i < 8 && !dir.empty(); ++i) {
    std::wstring cand = dir + L"\\" + rel;
    if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES) return cand;
    size_t k = dir.find_last_of(L"\\/");
    if (k == std::wstring::npos) break;
    dir = dir.substr(0, k);
  }
  return {};
}

bool readTextFile(const std::wstring& path, std::string& out) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  GetFileSizeEx(h, &sz);
  if (sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(h); return false; }
  out.resize(static_cast<size_t>(sz.QuadPart));
  DWORD got = 0;
  bool ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
  CloseHandle(h);
  if (!ok) return false;
  out.resize(got);
  if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB &&
      static_cast<unsigned char>(out[2]) == 0xBF)
    out.erase(0, 3);
  return true;
}

bool writeTextFileAtomic(const std::wstring& path, const std::string& data) {
  std::wstring tmp = path + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  DWORD put = 0;
  bool ok = data.empty() || WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &put, nullptr);
  CloseHandle(h);
  if (!ok || put != data.size()) { DeleteFileW(tmp.c_str()); return false; }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
  return true;
}

static bool runFileDialog(IFileDialog* dlg, HWND owner, const wchar_t* title, const wchar_t* filterName,
                          const wchar_t* filterSpec, std::string& out) {
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  if (title) dlg->SetTitle(title);
  COMDLG_FILTERSPEC fs[2] = {{filterName, filterSpec}, {L"Todos os arquivos", L"*.*"}};
  dlg->SetFileTypes(2, fs);
  dlg->SetFileTypeIndex(1);
  bool ok = false;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = toUtf8(p);
        ok = true;
        CoTaskMemFree(p);
      }
      item->Release();
    }
  }
  return ok;
}

bool pickOpenFile(void* owner, const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec,
                  std::string& out) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  bool ok = runFileDialog(dlg, static_cast<HWND>(owner), title, filterName, filterSpec, out);
  dlg->Release();
  return ok;
}

bool pickSaveFile(void* owner, const wchar_t* title, const wchar_t* filterName, const wchar_t* filterSpec,
                  const wchar_t* defaultExt, const std::wstring& defaultName, std::string& out) {
  IFileSaveDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_OVERWRITEPROMPT);
  if (defaultExt) dlg->SetDefaultExtension(defaultExt);
  if (!defaultName.empty()) dlg->SetFileName(defaultName.c_str());
  bool ok = runFileDialog(dlg, static_cast<HWND>(owner), title, filterName, filterSpec, out);
  dlg->Release();
  return ok;
}

bool pickFolder(void* owner, const wchar_t* title, std::string& out) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
  if (title) dlg->SetTitle(title);
  bool ok = false;
  if (SUCCEEDED(dlg->Show(static_cast<HWND>(owner)))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item)) && item) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        out = toUtf8(p);
        ok = true;
        CoTaskMemFree(p);
      }
      item->Release();
    }
  }
  dlg->Release();
  return ok;
}

std::vector<std::string> listFiles(const std::string& dir, const wchar_t* ext) {
  std::vector<std::string> out;
  if (dir.empty() || !ext) return out;
  std::wstring base = fromUtf8(dir);
  if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base += L'\\';
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
  if (h == INVALID_HANDLE_VALUE) return out;
  const size_t el = wcslen(ext);
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const size_t n = wcslen(fd.cFileName);
    if (n <= el || _wcsicmp(fd.cFileName + (n - el), ext) != 0) continue;
    out.push_back(toUtf8(base + fd.cFileName));
    if (out.size() >= 500) break;  // sanity cap
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end());
  return out;
}

static bool regKeyExists(HKEY root, const wchar_t* sub) {
  HKEY k = nullptr;
  if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) == ERROR_SUCCESS) { RegCloseKey(k); return true; }
  return false;
}

bool virtualCamRegistered(bool* perUser) {
  const wchar_t* sub =
      L"Software\\Classes\\CLSID\\{860BB310-5D01-11d0-BD3B-00A0C911CE86}\\Instance\\"
      L"{8433CA51-5EEA-4F62-A174-13FA1F6BADD5}";
  bool cu = regKeyExists(HKEY_CURRENT_USER, sub);
  bool lm = regKeyExists(HKEY_LOCAL_MACHINE, sub);
  if (perUser) *perUser = cu;
  return cu || lm;
}

bool obsPluginInstalled() {
  wchar_t pd[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
  std::wstring base = n ? std::wstring(pd, n) : L"C:\\ProgramData";
  DWORD a = GetFileAttributesW((base + L"\\obs-studio\\plugins\\owc-obs-source").c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string formatClock(long long unixMs) {
  FILETIME ft;
  ULARGE_INTEGER u;
  u.QuadPart = static_cast<unsigned long long>(unixMs) * 10000ull + 116444736000000000ull;
  ft.dwLowDateTime = u.LowPart;
  ft.dwHighDateTime = u.HighPart;
  FILETIME lt;
  SYSTEMTIME st;
  FileTimeToLocalFileTime(&ft, &lt);
  FileTimeToSystemTime(&lt, &st);
  char buf[32];
  std::snprintf(buf, sizeof buf, "%02u:%02u:%02u.%03u", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  return buf;
}

}  // namespace owcapp
