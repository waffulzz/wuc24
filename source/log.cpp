#include "log.h"

#include <cstdarg>
#include <cstdio>

#include <coreinit/debug.h>
#include <whb/sdcard.h>

namespace {
char g_sdMountPath[256] = {};
char g_logPath[512]     = {};
}  // namespace

const char *GetSdMountPath() {
    return g_sdMountPath[0] ? g_sdMountPath : nullptr;
}

void LogInit() {
    if (!WHBMountSdCard()) {
        OSReport("[wuc24] WHBMountSdCard failed -- SD logfile unavailable\n");
        return;
    }

    char *mount = WHBGetSdCardMountPath();
    std::snprintf(g_sdMountPath, sizeof(g_sdMountPath), "%s", mount);
    std::snprintf(g_logPath, sizeof(g_logPath), "%s/wuc24.log", mount);

    // Truncate so each session starts with a fresh log.
    FILE *f = std::fopen(g_logPath, "w");
    if (f) {
        std::fclose(f);
        OSReport("[wuc24] logging to %s\n", g_logPath);
    } else {
        OSReport("[wuc24] failed to open %s for writing\n", g_logPath);
        g_logPath[0] = '\0';
    }
}

void LogDeinit() {
    WHBUnmountSdCard();
}

void LogPrintf(const char *fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    OSReport("%s\n", buf);

    if (g_logPath[0] == '\0') return;

    // Open-append-close per line: costs a little SD latency but guarantees
    // nothing is lost if the plugin crashes mid-run.
    FILE *f = std::fopen(g_logPath, "a");
    if (f) {
        std::fprintf(f, "%s\n", buf);
        std::fclose(f);
    }
}
