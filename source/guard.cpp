#include "guard.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include <coreinit/atomic.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>

#include "log.h"
#include "nand.h"

namespace guard {

namespace {

volatile uint32_t s_stop     = 0;
volatile int32_t  s_critical = 0;

// Kept deliberately simple: one line of "nand path -> backup file", so that
// recovery works even if the plugin that wrote it is a different build.
constexpr char kJournalName[] = "wuc24_write_journal.txt";

std::string JournalPath() {
    const char *mount = GetSdMountPath();
    if (!mount) return {};
    return std::string(mount) + "/" + kJournalName;
}

}  // namespace

void RequestStop() {
    OSCompareAndSwapAtomic(&s_stop, 0, 1);
}

void ClearStop() {
    s_stop = 0;
}

bool StopRequested() {
    return s_stop != 0;
}

CriticalSection::CriticalSection() {
    OSAddAtomic(&s_critical, 1);
}

CriticalSection::~CriticalSection() {
    OSAddAtomic(&s_critical, -1);
}

bool InCriticalSection() {
    return s_critical != 0;
}

bool WaitForCriticalSection(int timeout_ms) {
    constexpr int kStepMs = 20;
    for (int waited = 0; waited < timeout_ms; waited += kStepMs) {
        if (!InCriticalSection()) return true;
        OSSleepTicks(OSMillisecondsToTicks(kStepMs));
    }
    // Giving up here means letting the process die mid-write. The journal is
    // what makes that recoverable.
    return !InCriticalSection();
}

bool OpenJournal(const std::string &nand_path, const std::string &backup_name) {
    const std::string path = JournalPath();
    if (path.empty()) {
        LOG("guard: no SD card, refusing to write without a recovery journal");
        return false;
    }

    FILE *f = std::fopen(path.c_str(), "w");
    if (!f) {
        LOG("guard: could not open the journal at %s", path.c_str());
        return false;
    }
    std::fprintf(f, "%s\n%s\n", nand_path.c_str(), backup_name.c_str());
    std::fflush(f);
    std::fclose(f);
    return true;
}

void CloseJournal() {
    const std::string path = JournalPath();
    if (!path.empty()) std::remove(path.c_str());
}

bool RecoverIfNeeded(VwiiNand &nand) {
    const std::string path = JournalPath();
    if (path.empty()) return false;

    FILE *f = std::fopen(path.c_str(), "r");
    if (!f) return false;  // the normal case: no interrupted write

    char nand_path[512]   = {};
    char backup_name[256] = {};
    const bool read_ok = std::fgets(nand_path, sizeof(nand_path), f) &&
                         std::fgets(backup_name, sizeof(backup_name), f);
    std::fclose(f);

    auto trim = [](char *s) {
        size_t n = std::strlen(s);
        while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
    };
    trim(nand_path);
    trim(backup_name);

    if (!read_ok || nand_path[0] == '\0' || backup_name[0] == '\0') {
        LOG("guard: journal is unreadable, removing it");
        std::remove(path.c_str());
        return false;
    }

    LOG("guard: a previous write to %s was interrupted", nand_path);

    const char *mount = GetSdMountPath();
    if (!mount) {
        LOG("guard: no SD card, cannot restore -- leaving the journal in place");
        return false;
    }

    const std::string backup_path = std::string(mount) + "/" + backup_name;
    FILE             *bf          = std::fopen(backup_path.c_str(), "rb");
    if (!bf) {
        LOG("guard: backup %s is missing, cannot restore", backup_name);
        LOG("guard: leaving the journal so this is not silently forgotten");
        return false;
    }
    std::fseek(bf, 0, SEEK_END);
    const long size = std::ftell(bf);
    std::fseek(bf, 0, SEEK_SET);

    std::vector<uint8_t> data(static_cast<size_t>(size > 0 ? size : 0));
    const size_t         got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), bf);
    std::fclose(bf);

    if (data.empty() || got != data.size()) {
        LOG("guard: backup could not be read in full, not restoring");
        return false;
    }

    if (!nand.WriteFile(nand_path, data.data(), static_cast<uint32_t>(data.size()))) {
        LOG("guard: RESTORE FAILED for %s -- the backup is still on SD", nand_path);
        return false;
    }

    LOG("guard: restored %s from %s (%zu bytes)", nand_path, backup_name, data.size());
    std::remove(path.c_str());
    return true;
}

}  // namespace guard
