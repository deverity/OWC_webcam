// Windows Firewall helper for OWC discovery.
//
// The passive announce listener (owc/discovery.h, UDP 47803) receives
// UNSOLICITED broadcasts from phones, which Windows Firewall blocks unless an
// inbound allow rule exists (the first time an app binds a port Windows may
// show its own "allow access" prompt; if the user dismissed it, nothing
// arrives). Everything else OWC uses is either outbound or a reply to an
// outbound packet (stateful), so this is the only rule needed:
//
//   name "OWC Discovery (UDP 47803)", inbound, allow, UDP, local port 47803,
//   profile Private (home/office Wi-Fi; Public networks stay closed).
//
// Usage from the UI (worker thread, these run netsh and block ~100-300 ms):
//   if (!owc::Firewall::rulesPresent()) {
//     show a banner "Permitir descoberta no firewall" -> on click:
//     owc::Firewall::addRulesElevated();   // UAC prompt, waits for netsh
//   }
// or show addRuleCommandLine() for the user to run in an elevated prompt.
#pragma once

#include <cstdint>
#include <string>

namespace owc {

class Firewall {
 public:
  static const char* ruleName();  // "OWC Discovery (UDP 47803)"
  // True if the inbound rule exists (any state). Runs `netsh advfirewall
  // firewall show rule name=...` hidden. False also when netsh fails.
  static bool rulesPresent();
  // netsh arguments (without "netsh") that add / delete the rule.
  static std::string addRuleArgs(uint16_t port = 47803);
  static std::string deleteRuleArgs();
  // Full command line for an elevated prompt: netsh advfirewall firewall add rule ...
  static std::string addRuleCommandLine(uint16_t port = 47803);
  // Runs netsh elevated (ShellExecuteEx "runas": shows the UAC prompt) and
  // waits up to timeout_ms. Returns true if netsh exited with 0. False if the
  // user declined UAC. Blocking; call from a worker thread.
  static bool addRulesElevated(uint16_t port = 47803, uint32_t timeout_ms = 60000);
};

}  // namespace owc
