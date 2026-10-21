// Loads owc-obs-source.dll with the installed OBS bin dir on the DLL search
// path and resolves the module exports WITHOUT calling obs_module_load.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <string>

int wmain(int argc, wchar_t** argv) {
  std::wstring plugin = argc > 1 ? argv[1] : L"obs-plugins\\owc-obs-source\\bin\\64bit\\owc-obs-source.dll";
  std::wstring obsBin = argc > 2 ? argv[2] : L"C:\\Program Files\\obs-studio\\bin\\64bit";

  wchar_t full[MAX_PATH];
  GetFullPathNameW(plugin.c_str(), MAX_PATH, full, nullptr);

  SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
  if (!AddDllDirectory(obsBin.c_str())) {
    fwprintf(stderr, L"AddDllDirectory failed (%lu)\n", GetLastError());
    return 2;
  }
  HMODULE m = LoadLibraryExW(full, nullptr,
                             LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                 LOAD_LIBRARY_SEARCH_USER_DIRS);
  if (!m) {
    fwprintf(stderr, L"LoadLibraryEx failed for %s (%lu)\n", full, GetLastError());
    return 1;
  }
  const char* names[] = {"obs_module_load", "obs_module_unload", "obs_module_set_pointer",
                         "obs_current_module", "obs_module_ver", "obs_module_set_locale",
                         "obs_module_free_locale", "obs_module_name"};
  int missing = 0;
  for (const char* n : names) {
    FARPROC p = GetProcAddress(m, n);
    printf("%-26s %s\n", n, p ? "resolved" : "NOT exported");
    if (!p && strcmp(n, "obs_module_unload") != 0 && strcmp(n, "obs_module_name") != 0 && strcmp(n, "obs_current_module") != 0) ++missing;
  }
  HMODULE obs = GetModuleHandleW(L"obs.dll");
  wchar_t op[MAX_PATH] = L"";
  if (obs) GetModuleFileNameW(obs, op, MAX_PATH);
  wprintf(L"obs.dll loaded from: %s\n", op);
  FreeLibrary(m);
  puts(missing ? "FAIL" : "OK: plugin loaded, imports resolved against obs.dll");
  return missing ? 1 : 0;
}
