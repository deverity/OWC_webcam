#include "util/thread.h"

#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

namespace owc {

void setThreadName(const char* name) { pthread_setname_np(pthread_self(), name); }

int raiseThreadPriority(int wanted) {
  // Android app processes may usually lower nice down to -19 (URGENT_AUDIO is
  // -19); some vendors restrict it. Step towards 0 and keep the first success.
  const pid_t tid = gettid();
  for (int nice = wanted; nice < 0; nice += 2) {
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(tid), nice) == 0) return nice;
  }
  return 0;
}

}  // namespace owc
