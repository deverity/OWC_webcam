// DLL entry points: class factory, DllCanUnloadNow, (un)registration.
//
// Registration is written directly to the registry (no IFilterMapper2), so
// it can target either hive:
//   regsvr32 owc_virtualcam.dll                -> HKLM\Software\Classes (admin)
//   regsvr32 /n /i:user owc_virtualcam.dll     -> HKCU\Software\Classes (no admin)
//   regsvr32 /u [/n /i:user] owc_virtualcam.dll
// A 32-bit build lands in the WOW64 view automatically (registry redirection).
#include <windows.h>
#include <dshow.h>

#include <new>
#include <string>
#include <vector>

#include <owc/vcam_ids.h>

#include "filter.h"

namespace owc {
namespace vcam {

volatile LONG g_objects = 0;
volatile LONG g_locks = 0;
HMODULE g_module = nullptr;

namespace {

const wchar_t kVideoInputCategory[] = L"{860BB310-5D01-11d0-BD3B-00A0C911CE86}";

class ClassFactory final : public IClassFactory {
 public:
  ClassFactory() { InterlockedIncrement(&g_objects); }
  STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory)) {
      *ppv = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
  STDMETHODIMP_(ULONG) Release() override {
    const LONG r = InterlockedDecrement(&ref_);
    if (r == 0) delete this;
    return (ULONG)r;
  }
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    return CreateFilterInstance(outer, riid, ppv);
  }
  STDMETHODIMP LockServer(BOOL lock) override {
    if (lock)
      InterlockedIncrement(&g_locks);
    else
      InterlockedDecrement(&g_locks);
    return S_OK;
  }

 private:
  ~ClassFactory() { InterlockedDecrement(&g_objects); }
  volatile LONG ref_ = 1;
};

// ---------------------------------------------------------------- FilterData

void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i)));
}
void putGuid(std::vector<uint8_t>& v, const GUID& g) {
  const uint8_t* p = (const uint8_t*)&g;
  v.insert(v.end(), p, p + sizeof(GUID));
}
void putSig(std::vector<uint8_t>& v, char first, const char* rest) {
  v.push_back((uint8_t)first);
  v.push_back((uint8_t)rest[0]);
  v.push_back((uint8_t)rest[1]);
  v.push_back((uint8_t)rest[2]);
}

// Serialized REGFILTER2 (version 2) exactly as IFilterMapper2 writes it:
//   REG_RF   {dwVersion, dwMerit, dwPins, dwUnused}
//   per pin: REG_RFP {"0pi3", dwFlags, dwInstances, dwMediaTypes, dwMediums, bCategory}
//            [DWORD offset of category GUID if bCategory]
//            per type: REG_TYPE {"0ty3", dwUnused, offMajor, offMinor}
//   GUID pool (offsets are from the start of the blob)
std::vector<uint8_t> buildFilterData() {
  const GUID minors[] = {subtypeFor(PixFmt::NV12), subtypeFor(PixFmt::YUY2), subtypeFor(PixFmt::I420),
                         subtypeFor(PixFmt::RGB32), subtypeFor(PixFmt::RGB24)};
  const uint32_t nTypes = (uint32_t)(sizeof(minors) / sizeof(minors[0]));
  const uint32_t header = 16 + 24 + 4 + nTypes * 16;  // pool starts here
  const uint32_t offCategory = header, offMajor = header + 16, offMinor0 = header + 32;
  std::vector<uint8_t> v;
  put32(v, 2);                 // dwVersion
  put32(v, MERIT_DO_NOT_USE);  // never picked by Intelligent Connect
  put32(v, 1);                 // pins
  put32(v, 0);
  putSig(v, '0', "pi3");
  put32(v, REG_PINFLAG_B_OUTPUT);
  put32(v, 1);  // instances
  put32(v, nTypes);
  put32(v, 0);  // mediums
  put32(v, 1);  // has category
  put32(v, offCategory);
  for (uint32_t i = 0; i < nTypes; ++i) {
    putSig(v, (char)('0' + i), "ty3");
    put32(v, 0);
    put32(v, offMajor);
    put32(v, offMinor0 + 16 * i);
  }
  putGuid(v, PIN_CATEGORY_CAPTURE);
  putGuid(v, MEDIATYPE_Video);
  for (const GUID& g : minors) putGuid(v, g);
  return v;
}

// ---------------------------------------------------------------- registry

LSTATUS setValue(HKEY root, const std::wstring& path, const wchar_t* name, DWORD type, const void* data,
                 DWORD bytes) {
  HKEY k = nullptr;
  LSTATUS s = RegCreateKeyExW(root, path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr,
                              &k, nullptr);
  if (s != ERROR_SUCCESS) return s;
  s = RegSetValueExW(k, name, 0, type, (const BYTE*)data, bytes);
  RegCloseKey(k);
  return s;
}

LSTATUS setString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
  return setValue(root, path, name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

std::wstring clsidKey() { return std::wstring(L"Software\\Classes\\CLSID\\") + OWC_VCAM_CLSID_STR; }
std::wstring instanceKey() {
  return std::wstring(L"Software\\Classes\\CLSID\\") + kVideoInputCategory + L"\\Instance\\" + OWC_VCAM_CLSID_STR;
}

HRESULT unregisterFrom(HKEY root) {
  LSTATUS a = RegDeleteTreeW(root, instanceKey().c_str());
  LSTATUS b = RegDeleteTreeW(root, clsidKey().c_str());
  if (a == ERROR_FILE_NOT_FOUND) a = ERROR_SUCCESS;
  if (b == ERROR_FILE_NOT_FOUND) b = ERROR_SUCCESS;
  if (a != ERROR_SUCCESS) return HRESULT_FROM_WIN32(a);
  return HRESULT_FROM_WIN32(b);
}

HRESULT registerIn(HKEY root) {
  wchar_t path[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(g_module, path, (DWORD)(sizeof(path) / sizeof(path[0])));
  if (n == 0 || n >= sizeof(path) / sizeof(path[0])) return E_FAIL;
  const std::wstring clsid = clsidKey(), inproc = clsid + L"\\InprocServer32", inst = instanceKey();
  const std::vector<uint8_t> blob = buildFilterData();
  LSTATUS s = setString(root, clsid, nullptr, OWC_VCAM_FRIENDLY_NAME);
  if (s == ERROR_SUCCESS) s = setString(root, inproc, nullptr, path);
  if (s == ERROR_SUCCESS) s = setString(root, inproc, L"ThreadingModel", L"Both");
  if (s == ERROR_SUCCESS) s = setString(root, inst, L"FriendlyName", OWC_VCAM_FRIENDLY_NAME);
  if (s == ERROR_SUCCESS) s = setString(root, inst, L"CLSID", OWC_VCAM_CLSID_STR);
  if (s == ERROR_SUCCESS) s = setValue(root, inst, L"FilterData", REG_BINARY, blob.data(), (DWORD)blob.size());
  if (s != ERROR_SUCCESS) {
    unregisterFrom(root);  // roll back partial writes
    return HRESULT_FROM_WIN32(s);
  }
  return S_OK;
}

bool isUserCmdLine(PCWSTR cmd) {
  if (!cmd) return false;
  return _wcsicmp(cmd, L"user") == 0 || _wcsicmp(cmd, L"peruser") == 0 || _wcsicmp(cmd, L"currentuser") == 0;
}

}  // namespace
}  // namespace vcam
}  // namespace owc

using namespace owc::vcam;

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = hinst;
    DisableThreadLibraryCalls(hinst);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
  if (!ppv) return E_POINTER;
  *ppv = nullptr;
  if (!IsEqualCLSID(rclsid, CLSID_OwcVirtualCam)) return CLASS_E_CLASSNOTAVAILABLE;
  ClassFactory* f = new (std::nothrow) ClassFactory();
  if (!f) return E_OUTOFMEMORY;
  const HRESULT hr = f->QueryInterface(riid, ppv);
  f->Release();
  return hr;
}

STDAPI DllCanUnloadNow() { return (g_objects == 0 && g_locks == 0) ? S_OK : S_FALSE; }

STDAPI DllRegisterServer() { return registerIn(HKEY_LOCAL_MACHINE); }

STDAPI DllUnregisterServer() { return unregisterFrom(HKEY_LOCAL_MACHINE); }

// regsvr32 /n /i:user  -> per-user (HKCU\Software\Classes), no elevation.
// regsvr32 /i[:machine] (with or without /n) -> HKLM.
STDAPI DllInstall(BOOL bInstall, PCWSTR pszCmdLine) {
  const HKEY root = isUserCmdLine(pszCmdLine) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
  return bInstall ? registerIn(root) : unregisterFrom(root);
}
