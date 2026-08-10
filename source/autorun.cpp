#include "autorun.h"

#include <cstdlib>
#include <cstring>

#include <coreinit/thread.h>
#include <coreinit/time.h>

#include "guard.h"
#include "log.h"

namespace autorun {

namespace {

// Big enough for the deepest thing the job does, which is FatFs walking a
// directory with a long-filename buffer on the stack.
constexpr uint32_t kStackSize = 256 * 1024;

// Lower than the default 16, so the Menu keeps every cycle it wants. This is
// what stops the console feeling locked up while the job runs.
constexpr int32_t kPriority = 24;

OSThread *s_thread  = nullptr;
void     *s_stack   = nullptr;
Job       s_job     = nullptr;
bool      s_running = false;

int JobEntry(int /*argc*/, const char ** /*argv*/) {
    if (s_job) s_job();
    s_running = false;
    return 0;
}

void Cleanup() {
    std::free(s_thread);
    std::free(s_stack);
    s_thread = nullptr;
    s_stack  = nullptr;
}

}  // namespace

bool Running() {
    return s_running;
}

bool Start(Job job) {
    if (s_running) {
        LOG("autorun: already running, not starting again");
        return false;
    }
    Cleanup();
    guard::ClearStop();

    s_thread = static_cast<OSThread *>(std::malloc(sizeof(OSThread)));
    s_stack  = std::malloc(kStackSize);
    if (!s_thread || !s_stack) {
        LOG("autorun: could not allocate a thread");
        Cleanup();
        return false;
    }
    std::memset(s_thread, 0, sizeof(OSThread));

    s_job     = job;
    s_running = true;

    // The stack grows down, so hand over its top.
    void *stack_top = static_cast<uint8_t *>(s_stack) + kStackSize;
    if (!OSCreateThread(s_thread, &JobEntry, 0, nullptr, stack_top, kStackSize, kPriority,
                        OS_THREAD_ATTRIB_AFFINITY_ANY)) {
        LOG("autorun: OSCreateThread failed");
        s_running = false;
        Cleanup();
        return false;
    }

    OSSetThreadName(s_thread, "wuc24 background job");
    OSResumeThread(s_thread);
    return true;
}

bool Stop(const char *reason, int timeout_ms) {
    if (!s_thread) return true;

    guard::RequestStop(reason);

    // A write already under way is short; letting it finish is far better than
    // being killed inside it.
    if (!guard::WaitForCriticalSection(timeout_ms)) {
        LOG("autorun: a NAND write is still running as we are being shut down");
        LOG("autorun: the journal will repair it on the next boot if it is cut off");
    }

    constexpr int kStepMs = 20;
    for (int waited = 0; waited < timeout_ms; waited += kStepMs) {
        if (!s_running) break;
        OSSleepTicks(OSMillisecondsToTicks(kStepMs));
    }

    if (s_running) {
        LOG("autorun: job did not stop in %dms", timeout_ms);
        return false;
    }

    int result = 0;
    OSJoinThread(s_thread, &result);
    Cleanup();
    return true;
}

}  // namespace autorun
