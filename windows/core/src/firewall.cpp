#include <owc/firewall.h>

#include <owc/adb.h>  // runProcess

#include "util.h"

#include <shellapi.h>

namespace owc {

using namespace detail;

namespace {
std::wstring netshPath() {
  wchar_t sys[MAX_PATH];
  const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
  return std::wstring(sys, n) + L"\\netsh.exe";
}
}  // namespace

const char* Firewall::ruleName() { return "OWC Discovery (UDP 47803)"; }

std::string Firewall::addRuleArgs(uint16_t port) {
  return std::string("advfirewall firewall add rule name=\"") + ruleName() +
         "\" dir=in action=allow protocol=UDP localport=" + std::to_string(port) +
         " profile=private";
}

std::string Firewall::deleteRuleArgs() {
  return std::string("advfirewall firewall delete rule name=\"") + ruleName() + "\"";
}

std::string Firewall::addRuleCommandLine(uint16_t port) { return "netsh " + addRuleArgs(port); }

bool Firewall::rulesPresent() {
  // netsh exits with 1 ("No rules match the specified criteria") when absent;
  // the output text is localized, so only the exit code is used.
  const ProcessResult r = runProcess(
      netshPath(), {"advfirewall", "firewall", "show", "rule", std::string("name=") + ruleName()},
      10000);
  return r.ok();
}

bool Firewall::addRulesElevated(uint16_t port, uint32_t timeout_ms) {
  const std::wstring exe = netshPath();
  const std::wstring args = utf8ToWide(addRuleArgs(port));
  SHELLEXECUTEINFOW sei{};
  sei.cbSize = sizeof(sei);
  sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NO_CONSOLE;
  sei.lpVerb = L"runas";
  sei.lpFile = exe.c_str();
  sei.lpParameters = args.c_str();
  sei.nShow = SW_HIDE;
  if (!ShellExecuteExW(&sei) || !sei.hProcess) return false;  // UAC declined
  bool ok = false;
  if (WaitForSingleObject(sei.hProcess, timeout_ms) == WAIT_OBJECT_0) {
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    ok = code == 0;
  }
  CloseHandle(sei.hProcess);
  return ok;
}

}  // namespace owc
