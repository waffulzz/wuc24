// wuc24 — WiiConnect24 for the vWii, from Wii U mode.
//
// WiiLink still serves WiiConnect24 content, but reaching it normally means
// booting vWii and letting it sit there. This plugin does the job from Wii U
// mode instead: it reads the console's own download list, fetches what each
// channel is owed, and writes the results into the vWii NAND, so the channels
// are current the next time you boot them. It handles mail in both directions
// too, sending anything queued on the message board and delivering what is
// waiting on the server.
//
// It runs on its own when the Wii U Menu starts, on a background thread, with a
// toast for progress. Everything up to the moment of writing happens in memory,
// and the writes themselves are journalled and verified, so being interrupted
// -- by launching vWii, starting a game, or losing power -- does not leave a
// channel's data half-written. See guard.h for how that is arranged.
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <vector>

#include <coreinit/title.h>

#include <wups.h>
#include <wups/config_api.h>
#include <wups/config/WUPSConfigItemBoolean.h>

#include "autorun.h"
#include "guard.h"
#include "log.h"
#include "mail.h"
#include "mailfetch.h"
#include "msgcfg.h"
#include "nand.h"
#include "net.h"
#include "tasks.h"
#include "toast.h"
#include "vff.h"
#include "wc24.h"
#include "wc24dec.h"

WUPS_PLUGIN_NAME("wuc24");
WUPS_PLUGIN_DESCRIPTION("Download WiiConnect24 (WiiLink) content to the vWii from Wii U mode");
// Set by the Makefile, which stamps in the commit for dev builds. The fallback
// is only reached if main.cpp is built outside it.
#ifndef WUC24_VERSION
#define WUC24_VERSION "v0.1-dev"
#endif

WUPS_PLUGIN_VERSION(WUC24_VERSION);
WUPS_PLUGIN_AUTHOR("waffulzz");
WUPS_PLUGIN_LICENSE("See THIRD-PARTY.md");

WUPS_USE_WUT_DEVOPTAB();
WUPS_USE_STORAGE("wuc24");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Runs an action with exceptions contained.
//
// An uncaught exception inside a plugin aborts the whole console, so a single
// oversized allocation is enough to crash the machine. Everything triggered
// from the config menu goes through here so the worst case is a log line.
template <typename F>
static void RunGuarded(const char *name, F &&action) {
    try {
        action();
    } catch (const std::exception &e) {
        LOG("%s: aborted by exception (%s) -- probably out of memory", name, e.what());
    } catch (...) {
        LOG("%s: aborted by an unknown exception", name);
    }
}

// Writes a buffer to <sd root>/<name>. Returns false (and logs) on failure.
static bool SaveToSd(const char *name, const void *data, size_t size) {
    const char *mount = GetSdMountPath();
    if (!mount) {
        LOG("SD not mounted, cannot save %s", name);
        return false;
    }

    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s", mount, name);
    FILE *f = std::fopen(path, "wb");
    if (!f) {
        LOG("failed to open %s for writing", path);
        return false;
    }
    const size_t written = std::fwrite(data, 1, size, f);
    std::fclose(f);

    if (written != size) {
        LOG("short write to %s (%zu of %zu bytes)", path, written, size);
        return false;
    }
    LOG("saved %zu bytes to %s", size, path);
    return true;
}

// ---------------------------------------------------------------------------
// Progress reporting
//
// The channel update is minutes of work -- the News Channel alone is 24 files
// and several megabytes. A notification that says "updating channels" for all
// of it is indistinguishable from a hang, so the long-running actions report
// each step into here and the toast follows along.
// ---------------------------------------------------------------------------

static toast::Progress *s_progress = nullptr;

static void Report(const std::string &text) {
    if (s_progress) {
        s_progress->Update(text);
    } else {
        LOG("%s", text.c_str());
    }
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

// Read-only: mount the vWii NAND and log its WC24 download tasks.
static void RunScan() {
    LOG("=== scan: reading vWii WC24 tasks ===");
    VwiiNand nand;
    if (!nand.ok()) {
        LOG("scan aborted: vWii NAND not available");
        return;
    }

    std::vector<DlTaskInfo> tasks;
    if (!ScanDlTasks(nand, tasks)) {
        LOG("scan failed");
        return;
    }
    LOG("=== scan done ===");
}


// Where a container's pre-modification backup lands on SD.
static std::string BackupNameFor(const DlTaskInfo &task) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "wuc24_bak_%08X_%.4s.bin", task.low_title_id,
                  task.high_title_id.c_str());
    return buf;
}

// Tasks we can actually handle today. Everything else is reported and skipped
// rather than silently ignored.
// Some entries still carry Nintendo's original hostnames, which have been dead
// since 2013. WiiLink serves the same content on its own domains; its official
// patchers rewrite these in place on the console. We substitute at download
// time instead, so nothing extra has to be written to NAND.
//
// The replacement hostnames are deliberately the same length as the originals
// (that is what lets WiiLink's patchers overwrite them in place), and the path
// is preserved either way.
struct HostOverride {
    const char *from;
    const char *to;
};

static const HostOverride kHostOverrides[] = {
    // Wii Sports Resort's Check Mii Out data, per WiiLink's WSR-Patcher.
    {"miicontest.wapp.wii.com", "miicontest24.wiilink.ca"},
};

// Returns `url` with any known-stale hostname replaced.
static std::string ApplyHostOverrides(const std::string &url) {
    for (const auto &o : kHostOverrides) {
        const size_t at = url.find(o.from);
        if (at != std::string::npos) {
            std::string patched = url;
            patched.replace(at, std::strlen(o.from), o.to);
            return patched;
        }
    }
    return url;
}

static bool IsEligible(const DlTaskInfo &t, const char **why_not) {
    if (t.filename.empty()) {
        *why_not = "no filename (mailbox entry)";
        return false;
    }
    if (t.url.compare(0, 7, "http://") != 0) {
        *why_not = "not plain HTTP";
        return false;
    }
    return true;
}

// One file to fetch and store. A list entry usually maps to exactly one of
// these, but an entry with subtasks fans out into several numbered ones.
struct DownloadUnit {
    uint16_t    entry_index;
    std::string url;
    std::string filename;
};

// Expands a task into the unit(s) it actually stands for.
//
// subtask_bitmask has one bit per valid subtask id. When it is non-zero the
// entry represents a *set* of numbered downloads: url.NN -> filename.NN. The
// News Channel is the live example -- bitmask 0x00FFFFFF (ids 0..23), a plain
// news.bin that 404s, and a container holding exactly 2.bin.00 ... 2.bin.23.
// When the bitmask is zero the entry is a single plain download.
//
// (Dolphin keys the URL suffix off bit 1 and the filename suffix off bit 0 of
// the same bitmask. That yields the same result for every real entry seen so
// far, since a subtask set always starts at id 0, but conflates "subtask N is
// valid" with "add a suffix", so it is not reproduced here.)
static void ExpandTask(const DlTaskInfo &t, std::vector<DownloadUnit> &out) {
    const std::string url = ApplyHostOverrides(t.url);
    if (url != t.url) {
        LOG("  [%u] host override: %s", t.index, url.c_str());
    }

    if (t.subtask_bitmask == 0) {
        out.push_back(DownloadUnit{t.index, url, t.filename});
        return;
    }

    for (int i = 0; i < 32; i++) {
        if (!((t.subtask_bitmask >> i) & 1)) continue;

        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), ".%02d", i);
        out.push_back(DownloadUnit{t.index, url + suffix, t.filename + suffix});
    }
}

// One container plus every file that goes into it. Several entries commonly
// share a container -- the Forecast Channel's 3.bin and 4.bin both live in
// /title/00010002/48414645/data/wc24dl.vff -- so they must be applied together
// in a single read/modify/write, or the later write would undo the earlier one.
struct ContainerJob {
    std::string               vff_path;
    std::string               backup_name;
    std::string               label;  // what to call it on screen
    std::vector<DownloadUnit> units;
};

// The four-character title code is the only name we have for a channel, and it
// means nothing to anyone reading a notification.
// The channels this knows by name, and whether each one is wanted.
//
// Matching is on the first three characters because the fourth is the region:
// HAGE, HAGJ and HAGP are all the News Channel.
//
// Being able to turn one off matters more than it sounds. The News Channel is
// 24 separate files and a few megabytes, where every other channel is one or
// two small ones -- it accounts for most of the time a run takes, and not
// everybody reads it.
struct Channel {
    const char *prefix;  // first three characters of the title code
    const char *name;    // what to call it in the log
    const char *menu;    // what to call it in the settings menu
    const char *key;     // where its setting lives in plugin storage
    bool        enabled;
};

static Channel s_channels[] = {
    {"HAF", "Forecast",          "Forecast",                    "ch_forecast", true},
    {"HAG", "News",              "News  (slow: 24 files)",      "ch_news",     true},
    {"HAJ", "Everybody Votes",   "Everybody Votes",             "ch_votes",    true},
    {"HAT", "Nintendo Channel",  "Nintendo Channel",            "ch_nintendo", true},
    {"HAB", "Wii Shop",          "Wii Shop",                    "ch_shop",     true},
    {"RZT", "Wii Sports Resort", "Wii Sports Resort",           "ch_wsr",      true},
    {"RMC", "Mario Kart Wii",    "Mario Kart Wii  (needs TLS)", "ch_mkw",      true},
};

// The entry for a task, or null if it is a channel we have no name for.
static Channel *FindChannel(const DlTaskInfo &t) {
    for (auto &c : s_channels) {
        if (t.high_title_id.compare(0, 3, c.prefix) == 0) return &c;
    }
    return nullptr;
}

static std::string ChannelLabel(const DlTaskInfo &t) {
    const Channel *c = FindChannel(t);
    return c ? c->name : t.high_title_id;
}

// Unknown channels are left enabled: there is no toggle for them, so refusing
// to update them would be a setting nobody could find or change.
static bool ChannelEnabled(const DlTaskInfo &t) {
    const Channel *c = FindChannel(t);
    return c ? c->enabled : true;
}

// Collects the containers to update and the files destined for each.
// `skip_disabled` is what the settings menu controls. Restore passes false:
// turning a channel off means "do not spend time downloading it", not "leave it
// broken", so a recovery still covers everything there is a backup for.
static void BuildJobs(const std::vector<DlTaskInfo> &tasks, std::vector<ContainerJob> &jobs,
                      bool log_skips, bool skip_disabled = true) {
    for (const auto &t : tasks) {
        const char *why_not = nullptr;
        if (!IsEligible(t, &why_not)) {
            if (log_skips) LOG("skipping [%u] %s: %s", t.index, t.filename.c_str(), why_not);
            continue;
        }
        if (skip_disabled && !ChannelEnabled(t)) {
            if (log_skips) {
                LOG("skipping [%u] %s: %s is turned off", t.index, t.filename.c_str(),
                    ChannelLabel(t).c_str());
            }
            continue;
        }

        ContainerJob *job = nullptr;
        for (auto &j : jobs) {
            if (j.vff_path == t.vff_path) job = &j;
        }
        if (!job) {
            jobs.push_back(ContainerJob{t.vff_path, BackupNameFor(t), ChannelLabel(t), {}});
            job = &jobs.back();
        }
        // Some titles list the same file twice from different URLs -- the
        // Nintendo Channel asks for csdata.bn and csdata.LZ, which serve byte
        // for byte the same thing. Both would land on the same filename, so
        // the second is only a wasted download and a coin toss over which one
        // wins. Keep the first.
        std::vector<DownloadUnit> expanded;
        ExpandTask(t, expanded);
        for (auto &unit : expanded) {
            bool duplicate = false;
            for (const auto &existing : job->units) {
                if (existing.filename == unit.filename) duplicate = true;
            }
            if (duplicate) {
                if (log_skips) {
                    LOG("skipping [%u] %s: already provided by another task", t.index,
                        unit.filename.c_str());
                }
                continue;
            }
            job->units.push_back(std::move(unit));
        }
    }
}

// FNV-1a over a buffer. Used to confirm what NAND holds matches what we wrote
// without keeping two copies of a multi-megabyte container in memory at once
// (the News Channel's container is several MB, and a plugin should not claim
// twice that just to run a comparison).
static uint64_t HashBuffer(const std::vector<uint8_t> &data) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t b : data) {
        hash ^= b;
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Reads <sd root>/<name> into `out`.
static bool LoadFromSd(const char *name, std::vector<uint8_t> &out) {
    const char *mount = GetSdMountPath();
    if (!mount) return false;

    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s", mount, name);
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;

    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

// Puts a container back from its SD backup after a failed write.
static void RestoreContainer(VwiiNand &nand, const ContainerJob &job) {
    std::vector<uint8_t> backup;
    if (!LoadFromSd(job.backup_name.c_str(), backup)) {
        LOG("  RESTORE FAILED: cannot read %s from SD", job.backup_name.c_str());
        return;
    }
    // Putting the original back is the one write that must not be abandoned:
    // it runs precisely when a container is already in doubt.
    guard::CriticalSection critical;
    if (nand.WriteFile(job.vff_path.c_str(), backup.data(),
                       static_cast<uint32_t>(backup.size()))) {
        nand.Flush();
        LOG("  original restored from %s", job.backup_name.c_str());
    } else {
        LOG("  RESTORE FAILED. The backup is still on SD as %s", job.backup_name.c_str());
    }
}

// The whole pipeline:
//   read tasks -> expand subtasks -> group by container -> for each container,
//   download and apply every file to one in-memory copy -> verify ->
//   (optionally) write it back to NAND.
//
// Each payload is applied to the in-memory copy as it arrives rather than
// buffering them all: the News Channel alone is 24 files of ~110 KB, and
// holding those plus two copies of a multi-megabyte container would be far
// more memory than a plugin should claim. Atomicity is unaffected, because
// NAND is only touched once every file has succeeded.
//
// Dry run and the real thing take exactly the same path and differ only at the
// last step, so what gets verified is always what would get written.
static void RunPipeline(bool commit) {
    const char *mode = commit ? "COMMIT" : "dry run";
    LOG("=== %s ===", mode);

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("%s aborted: vWii NAND not available", mode);
        return;
    }

    std::vector<DlTaskInfo> tasks;
    if (!ScanDlTasks(nand, tasks)) {
        LOG("%s aborted: could not read the task list", mode);
        return;
    }

    std::vector<ContainerJob> jobs;
    BuildJobs(tasks, jobs, true);
    if (jobs.empty()) {
        LOG("%s: nothing eligible to do", mode);
        return;
    }
    LOG("%s: %zu container(s) to update", mode, jobs.size());

    net::Init();

    size_t containers_ok = 0, containers_failed = 0;

    size_t job_index = 0;
    for (const auto &job : jobs) {
        job_index++;
        LOG("--- %s (%zu file(s)) ---", job.vff_path.c_str(), job.units.size());
        {
            char step[128];
            std::snprintf(step, sizeof(step), "WiiConnect24: %s (%zu/%zu)",
                          job.label.c_str(), job_index, jobs.size());
            Report(step);
        }

        // 1. read the container and log what the channel currently has. Done
        //    before anything can fail, so a container we end up skipping still
        //    gets reported.
        std::vector<uint8_t> buffer;
        if (!nand.ReadFile(job.vff_path.c_str(), buffer)) {
            LOG("  could not read the container -- skipped");
            containers_failed++;
            continue;
        }
        const size_t container_size = buffer.size();
        const std::vector<uint8_t> original_bytes = buffer;
        LOG("  container is %zu bytes", container_size);
        {
            vff::Image image(buffer);
            std::vector<vff::Image::Entry> before;
            if (image.ok() && image.List(before)) {
                LOG("  container BEFORE:");
                for (const auto &e : before) {
                    LOG("      %-16s %8u %s", e.name.c_str(), e.size, e.is_dir ? "<dir>" : "");
                }
            } else {
                LOG("  container BEFORE: could not be mounted/listed");
            }
        }

        // 2. back it up. This is also what a failed write is restored from.
        if (!SaveToSd(job.backup_name.c_str(), buffer.data(), buffer.size())) {
            LOG("  refusing to continue without a backup on SD -- skipped");
            containers_failed++;
            continue;
        }

        // 3. download each file and apply it to the in-memory copy as it
        //    arrives. Any failure abandons the whole container.
        bool all_ok = true;
        {
            vff::Image image(buffer);
            if (!image.ok()) {
                LOG("  could not mount the container -- skipped");
                containers_failed++;
                continue;
            }

            size_t n = 0;
            for (const auto &unit : job.units) {
                n++;
                std::vector<uint8_t> body;
                int status = 0;
                if (!net::HttpGet(unit.url, body, status) || status != 200) {
                    LOG("  (%zu/%zu) [%u] %s: download failed (HTTP %d)", n, job.units.size(),
                        unit.entry_index, unit.filename.c_str(), status);
                    all_ok = false;
                    break;
                }

                // Subtask units inherit their parent entry's flags.
                const DlTaskInfo *parent = nullptr;
                for (const auto &t : tasks) {
                    if (t.index == unit.entry_index) parent = &t;
                }
                if (!parent) {
                    all_ok = false;
                    break;
                }

                // Encrypted content needs the title's AES key, which lives in
                // its wc24pubk.mod alongside the container.
                uint8_t        aes_key[16];
                const uint8_t *key = nullptr;
                if (parent->encrypted) {
                    std::vector<uint8_t> pubk;
                    if (!nand.ReadFile(parent->pubk_path.c_str(), pubk) ||
                        pubk.size() < sizeof(wc24::WC24PubkMod)) {
                        LOG("  (%zu/%zu) %s: cannot read %s -- has this title been opted in?",
                            n, job.units.size(), unit.filename.c_str(),
                            parent->pubk_path.c_str());
                        all_ok = false;
                        break;
                    }
                    std::memcpy(aes_key, pubk.data() + offsetof(wc24::WC24PubkMod, aes_key),
                                sizeof(aes_key));
                    key = aes_key;
                }

                std::vector<uint8_t> payload;
                const wc24dec::Result decoded =
                    wc24dec::Decode(body, parent->rsa_signed, parent->encrypted, key, payload);
                if (decoded != wc24dec::Result::Ok) {
                    LOG("  (%zu/%zu) [%u] %s: decode failed (%s)", n, job.units.size(),
                        unit.entry_index, unit.filename.c_str(), wc24dec::ResultName(decoded));
                    all_ok = false;
                    break;
                }

                if (!image.WriteFile(unit.filename.c_str(), payload.data(),
                                     static_cast<uint32_t>(payload.size()))) {
                    LOG("  (%zu/%zu) %s: writing into the container failed", n, job.units.size(),
                        unit.filename.c_str());
                    all_ok = false;
                    break;
                }
                LOG("  (%zu/%zu) %s: %zu bytes stored", n, job.units.size(),
                    unit.filename.c_str(), payload.size());
                if (job.units.size() > 1) {
                    char step[128];
                    std::snprintf(step, sizeof(step), "WiiConnect24: %s (%zu/%zu) %zu%%",
                                  job.label.c_str(), job_index, jobs.size(),
                                  (n * 100) / job.units.size());
                    Report(step);
                }
            }
        }
        if (!all_ok) {
            LOG("  container left untouched");
            containers_failed++;
            continue;
        }

        // 4. verify the modified copy before it goes near NAND
        {
            vff::Image image(buffer);
            if (!image.ok()) {
                LOG("  modified container no longer mounts -- discarded");
                containers_failed++;
                continue;
            }
            std::vector<vff::Image::Entry> after;
            if (image.List(after)) {
                LOG("  container AFTER:");
                for (const auto &e : after) {
                    LOG("      %-16s %8u %s", e.name.c_str(), e.size, e.is_dir ? "<dir>" : "");
                }
            }
        }
        if (buffer.size() != container_size) {
            LOG("  container size changed (%zu -> %zu) -- discarded", container_size,
                buffer.size());
            containers_failed++;
            continue;
        }

        if (!commit) {
            LOG("  dry run OK -- NAND not modified (backup: %s)", job.backup_name.c_str());
            containers_ok++;
            continue;
        }

        // 5. write it back, then read it straight off NAND to confirm
        //
        // Nothing so far has touched the console, so stopping here costs
        // nothing. Once the write starts it has to finish, so this is the last
        // point where quitting is free -- and the right place to check.
        if (guard::StopRequested()) {
            LOG("  stopping before the write: the console is on its way out");
            LOG("  nothing was modified");
            containers_failed++;
            break;
        }

        // An identical container is not worth the risk of writing.
        if (buffer == original_bytes) {
            LOG("  unchanged -- skipping the write");
            containers_ok++;
            continue;
        }

        if (!guard::OpenJournal(job.vff_path, job.backup_name)) {
            LOG("  refusing to write without a recovery journal");
            containers_failed++;
            continue;
        }

        LOG("  writing %zu bytes to NAND ...", buffer.size());
        bool write_ok;
        {
            // Exit handlers wait for this rather than killing us mid-write.
            guard::CriticalSection critical;
            write_ok = nand.WriteFile(job.vff_path.c_str(), buffer.data(),
                                      static_cast<uint32_t>(buffer.size()));
            if (write_ok) nand.Flush();
        }
        if (!write_ok) {
            LOG("  WRITE FAILED -- restoring the original");
            RestoreContainer(nand, job);
            guard::CloseJournal();
            containers_failed++;
            continue;
        }

        // Remember what we wrote, then release it before reading the container
        // back, so only one copy is ever held.
        const uint64_t written_hash = HashBuffer(buffer);
        std::vector<uint8_t>().swap(buffer);

        std::vector<uint8_t> verify;
        if (!nand.ReadFile(job.vff_path.c_str(), verify)) {
            LOG("  VERIFICATION FAILED: cannot read the container back -- restoring");
            RestoreContainer(nand, job);
            guard::CloseJournal();
            containers_failed++;
            continue;
        }
        if (verify.size() != container_size || HashBuffer(verify) != written_hash) {
            LOG("  VERIFICATION FAILED after write (%zu bytes, hash mismatch) -- restoring",
                verify.size());
            RestoreContainer(nand, job);
            containers_failed++;
            continue;
        }

        // Verified, so the write is no longer something to recover from.
        guard::CloseJournal();
        LOG("  COMMITTED and verified byte-for-byte (backup: %s)", job.backup_name.c_str());
        containers_ok++;
    }

    LOG("=== %s done: %zu container(s) ok, %zu failed/skipped ===", mode, containers_ok,
        containers_failed);
}

// Read-only: show the console's mail identity and service URLs.
static void RunMailConfig() {
    LOG("=== mail config (read-only) ===");
    VwiiNand nand;
    if (!nand.ok()) {
        LOG("aborted: vWii NAND not available");
        return;
    }
    msgcfg::Config cfg;
    if (!msgcfg::Read(nand, cfg)) {
        LOG("could not read the mail config");
        return;
    }
    msgcfg::LogSummary(cfg);
    LOG("=== mail config done ===");
}


// Read-only: ask the mail server whether anything is waiting. Consumes
// nothing, so it can be run as often as you like.
static void RunMailCheck() {
    LOG("=== mail check ===");

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("aborted: vWii NAND not available");
        return;
    }
    msgcfg::Config cfg;
    if (!msgcfg::Read(nand, cfg)) {
        LOG("aborted: cannot read the mail config");
        return;
    }

    net::Init();
    mailfetch::CheckResult result;
    if (!mailfetch::Check(cfg, result)) {
        LOG("check failed");
        return;
    }
    LOG("=== mail check done ===");
}

// Downloads waiting mail and puts it in the inbox.
//
// This is the one action that cannot be undone from here: the server marks
// messages delivered as it sends them, so once fetched they will never arrive
// through the console's own route. Hence the arming requirement even for the
// dry run, which still consumes them.
static void RunFetchMail(bool commit) {
    LOG("=== fetch mail (%s) ===", commit ? "COMMIT" : "dry run");

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("aborted: vWii NAND not available");
        return;
    }
    msgcfg::Config cfg;
    if (!msgcfg::Read(nand, cfg)) {
        LOG("aborted: cannot read the mail config");
        return;
    }

    net::Init();

    // Check first: no point consuming anything if nothing is waiting.
    mailfetch::CheckResult check;
    if (mailfetch::Check(cfg, check) && !check.has_mail) {
        LOG("nothing waiting -- not calling receive, so nothing is consumed");
        LOG("=== fetch mail done ===");
        return;
    }

    // Back the inbox up before it changes.
    std::vector<uint8_t> backup;
    if (!nand.ReadFile(wc24::kWc24RecvCtl, backup) ||
        !SaveToSd("wuc24_bak_recv_ctl.bin", backup.data(), backup.size())) {
        LOG("aborted: could not back up the inbox index");
        return;
    }
    if (!nand.ReadFile(wc24::kWc24RecvMbx, backup) ||
        !SaveToSd("wuc24_bak_recv_mbx.bin", backup.data(), backup.size())) {
        LOG("aborted: could not back up the inbox mailbox");
        return;
    }
    backup.clear();
    backup.shrink_to_fit();

    std::vector<std::string> messages;
    if (!mailfetch::Receive(cfg, messages)) {
        LOG("fetch failed");
        return;
    }
    if (messages.empty()) {
        LOG("the server returned no messages");
        LOG("=== fetch mail done ===");
        return;
    }

    // Keep a copy on SD regardless: they are gone from the server now, so if
    // delivery fails this is the only remaining copy.
    for (size_t i = 0; i < messages.size(); i++) {
        char name[64];
        std::snprintf(name, sizeof(name), "wuc24_fetched_%02zu.msg", i);
        SaveToSd(name, messages[i].data(), messages[i].size());
    }

    size_t delivered = 0;
    for (const auto &message : messages) {
        if (mail::DeliverRaw(nand, message, commit)) {
            delivered++;
        } else {
            LOG("failed to deliver one message -- it is on SD as wuc24_fetched_*.msg");
        }
    }

    LOG("=== fetch mail done: %zu of %zu delivered ===", delivered, messages.size());
    if (commit && delivered > 0) LOG("boot vWii and check the message board");
}

// Sends whatever is sitting in the outbox.
//
// The console queues mail written on the message board and sends it the next
// time WiiConnect24 runs; this does that job from Wii U mode instead. Messages
// are only retired from the outbox once the server says it took them, so a
// failure leaves them queued for the console to send later.
static void RunSendMail(bool commit) {
    LOG("=== send mail (%s) ===", commit ? "COMMIT" : "dry run");

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("aborted: vWii NAND not available");
        return;
    }
    msgcfg::Config cfg;
    if (!msgcfg::Read(nand, cfg)) {
        LOG("aborted: cannot read the mail config");
        return;
    }

    std::vector<mail::Queued> queued;
    if (!mail::ReadOutbox(nand, queued)) {
        LOG("aborted: cannot read the outbox");
        return;
    }
    if (queued.empty()) {
        LOG("nothing queued to send");
        LOG("=== send mail done ===");
        return;
    }

    // Keep a copy: once the server accepts them they leave the console.
    for (size_t i = 0; i < queued.size(); i++) {
        char name[64];
        std::snprintf(name, sizeof(name), "wuc24_outbox_%02zu.msg", i);
        SaveToSd(name, queued[i].text.data(), queued[i].text.size());
    }

    // Stop here on a dry run, BEFORE going near the network.
    //
    // Handing the message to the server delivers it for real; skipping only
    // the NAND write afterwards would still send it, and running the dry run
    // and then the commit would deliver it twice. The recipient cannot be
    // un-sent to, so the safe half of this action is the half that never
    // opens a socket.
    if (!commit) {
        LOG("dry run: %zu message(s) would be sent. Nothing has been transmitted.",
            queued.size());
        for (const auto &q : queued) {
            LOG("  slot %u id %u, %zu bytes", q.slot, q.id, q.text.size());
        }
        LOG("copies of each are on SD as wuc24_outbox_NN.msg");
        LOG("=== send mail done ===");
        return;
    }

    std::vector<uint8_t> backup;
    if (!nand.ReadFile(wc24::kWc24SendCtl, backup) ||
        !SaveToSd("wuc24_bak_send_ctl.bin", backup.data(), backup.size())) {
        LOG("aborted: could not back up the send index");
        return;
    }
    if (!nand.ReadFile(wc24::kWc24SendMbx, backup) ||
        !SaveToSd("wuc24_bak_send_mbx.bin", backup.data(), backup.size())) {
        LOG("aborted: could not back up the send mailbox");
        return;
    }
    backup.clear();
    backup.shrink_to_fit();

    std::vector<std::string> texts;
    texts.reserve(queued.size());
    for (const auto &q : queued) texts.push_back(q.text);

    net::Init();
    std::vector<bool> accepted;
    if (!mailfetch::Send(cfg, texts, accepted)) {
        LOG("send failed -- everything stays queued");
        return;
    }

    std::vector<mail::Queued> sent;
    for (size_t i = 0; i < queued.size(); i++) {
        if (i < accepted.size() && accepted[i]) sent.push_back(queued[i]);
    }
    LOG("%zu of %zu accepted by the server", sent.size(), queued.size());

    if (sent.empty()) {
        LOG("=== send mail done ===");
        return;
    }
    if (!mail::ClearFromOutbox(nand, sent, commit)) {
        LOG("WARNING: the server took the mail but the outbox could not be updated.");
        LOG("The console may send them again. Backups are on SD.");
        return;
    }

    LOG("=== send mail done ===");
}



// Read-only: dump the mail boxes so the receive-side format can be worked out
// against real data. Dolphin implements sending only and explicitly does not
// save received mail, so there is no working reference for what a received
// entry should look like -- an existing mailbox with real messages in it is
// the reference.
static void RunDumpMail() {
    LOG("=== dump mail (read-only) ===");

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("dump mail aborted: vWii NAND not available");
        return;
    }

    struct Box {
        const char *ctl_path;
        const char *mbx_path;
        const char *ctl_out;
        const char *mbx_out;
        const char *label;
    };
    const Box boxes[] = {
        {wc24::kWc24RecvCtl, wc24::kWc24RecvMbx, "wuc24_recv_ctl.bin", "wuc24_recv_mbx.bin",
         "RECEIVE"},
        {wc24::kWc24SendCtl, wc24::kWc24SendMbx, "wuc24_send_ctl.bin", "wuc24_send_mbx.bin",
         "SEND"},
    };

    for (const auto &box : boxes) {
        LOG("--- %s box ---", box.label);

        std::vector<uint8_t> ctl;
        if (!nand.ReadFile(box.ctl_path, ctl)) {
            LOG("  cannot read %s", box.ctl_path);
        } else {
            LOG("  %s: %zu bytes", box.ctl_path, ctl.size());
            SaveToSd(box.ctl_out, ctl.data(), ctl.size());

            const uint32_t capacity = wc24::MailEntryCount(ctl.size());
            if (capacity > 0) {
                wc24::MailListHeader header;
                std::memcpy(&header, ctl.data(), sizeof(header));
                const auto *entries = reinterpret_cast<const wc24::MailListEntry *>(
                    ctl.data() + sizeof(wc24::MailListHeader));

                LOG("  magic=0x%08X (want 0x%08X) version=%u mail=%u entries=%u (capacity %u)",
                    header.magic, wc24::kMailListMagic, header.version, header.number_of_mail,
                    header.total_entries, capacity);
                LOG("  total_msg_bytes=%u filesize=%u next_id=%u next_offset=%u vff_free=%u",
                    header.total_size_of_messages, header.filesize, header.next_entry_id,
                    header.next_entry_offset, header.vff_free_space);

                int shown = 0;
                for (uint32_t i = 0; i < capacity; i++) {
                    const auto &e = entries[i];
                    if (e.id == 0 && e.msg_size == 0) continue;
                    if (shown++ >= 8) {
                        LOG("  ... (further entries omitted)");
                        break;
                    }
                    LOG("  [%3u] id=%u flag=0x%08X size=%u app_id=0x%08X hdr_len=%u "
                        "tag=0x%08X cmd=0x%08X",
                        i, e.id, e.flag, e.msg_size, e.app_id, e.header_length, e.tag,
                        e.wii_cmd);
                    LOG("        from=0x%08X to=0x%08X subj=0x%08X charset=0x%08X enc=0x%08X",
                        e.packed_from, e.packed_to, e.packed_subject, e.packed_charset,
                        e.packed_transfer_encoding);
                    LOG("        msg_off=%u enc_len=%u msg_len=%u parts=%u grp=%u "
                        "mins1900=%u always1=%u",
                        e.message_offset, e.encoded_length, e.message_length,
                        e.number_of_multipart_entries, e.app_group, e.minutes_since_1900,
                        e.always_1);
                }
                if (shown == 0) LOG("  (no populated entries)");
            } else {
                LOG("  too small to be a mail list (%zu bytes)", ctl.size());
            }
        }

        std::vector<uint8_t> mbx;
        if (!nand.ReadFile(box.mbx_path, mbx)) {
            LOG("  cannot read %s", box.mbx_path);
            continue;
        }
        LOG("  %s: %zu bytes", box.mbx_path, mbx.size());
        SaveToSd(box.mbx_out, mbx.data(), mbx.size());

        vff::Image image(mbx);
        if (!image.ok()) {
            LOG("  (not mountable as a VFF)");
            continue;
        }
        std::vector<vff::Image::Entry> entries;
        if (image.List(entries)) {
            LOG("  VFF root:");
            for (const auto &e : entries) {
                LOG("      %-20s %8u %s", e.name.c_str(), e.size, e.is_dir ? "<dir>" : "");
            }
        }
        // The messages themselves live under "mb".
        if (image.List(entries, "mb")) {
            LOG("  VFF mb/ (%zu entries):", entries.size());
            for (const auto &e : entries) {
                LOG("      %-20s %8u %s", e.name.c_str(), e.size, e.is_dir ? "<dir>" : "");
            }
        } else {
            LOG("  VFF mb/: could not be listed");
        }
    }

    // The Wii Menu's own message store. /shared2/wc24/mbox is only a transit
    // box -- the Menu imports from it and clears it -- so this is where mail
    // that is actually visible on the console ends up.
    LOG("--- Wii Menu message database ---");
    constexpr char kCdbPath[] = "/title/00000001/00000002/data/cdb.vff";
    const int64_t  cdb_size   = nand.FileSize(kCdbPath);
    LOG("  %s: %lld bytes", kCdbPath, static_cast<long long>(cdb_size));

    // 20 MB will not fit in a plugin's heap -- attempting it throws bad_alloc
    // and takes the console down. Stream it to SD instead and inspect it on a
    // computer; nothing here needs it resident.
    if (cdb_size > 0) {
        const char *mount = GetSdMountPath();
        if (!mount) {
            LOG("  SD not mounted, cannot copy");
        } else {
            char out_path[512];
            std::snprintf(out_path, sizeof(out_path), "%s/wuc24_cdb_vff.bin", mount);
            FILE *f = std::fopen(out_path, "wb");
            if (!f) {
                LOG("  could not open %s", out_path);
            } else {
                const bool ok = nand.CopyFileTo(kCdbPath, f);
                std::fclose(f);
                if (ok) {
                    LOG("  copied to %s", out_path);
                } else {
                    LOG("  copy to %s FAILED", out_path);
                }
            }
        }
    }

    LOG("=== dump mail done ===");
}




// Puts back whatever BackupNameFor() last saved for each container.
static void RunRestore() {
    LOG("=== RESTORE from SD backups ===");

    if (!GetSdMountPath()) {
        LOG("restore aborted: SD not mounted");
        return;
    }

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("restore aborted: vWii NAND not available");
        return;
    }

    std::vector<DlTaskInfo> tasks;
    if (!ScanDlTasks(nand, tasks)) {
        LOG("restore aborted: could not read the task list");
        return;
    }

    std::vector<ContainerJob> jobs;
    BuildJobs(tasks, jobs, false, /*skip_disabled=*/false);

    for (const auto &job : jobs) {
        std::vector<uint8_t> backup;
        if (!LoadFromSd(job.backup_name.c_str(), backup)) {
            LOG("no usable backup %s -- skipping %s", job.backup_name.c_str(),
                job.vff_path.c_str());
            continue;
        }

        if (nand.WriteFile(job.vff_path.c_str(), backup.data(),
                           static_cast<uint32_t>(backup.size()))) {
            LOG("restored %s from %s (%zu bytes)", job.vff_path.c_str(),
                job.backup_name.c_str(), backup.size());
        } else {
            LOG("FAILED to restore %s", job.vff_path.c_str());
        }
    }

    LOG("=== restore done ===");
}

// ---------------------------------------------------------------------------
// The automatic job
// ---------------------------------------------------------------------------

// Run everything on its own when a title starts. On by default: keeping the
// channels current without being asked is the whole point of installing this,
// and the writes are backed up and journalled either way.
static bool s_autorun = true;

// Whether to hand queued mail to the server, and to pull down waiting mail.
static bool s_autoMail = true;

// When the job last finished, as a Unix timestamp, kept in plugin storage.
//
// A plugin is reloaded for every title, so nothing in memory survives to tell a
// cold boot apart from coming back to the Menu after a game -- and the Menu
// starting is the only signal available. Without this, quitting a game would
// kick off several megabytes of downloads every single time. Persisting the
// last run is the only thing that actually distinguishes the two.
static int64_t s_lastRun = 0;

// How long to leave it before running again. WiiConnect24 content is refreshed
// on the order of hours, so anything shorter is just noise.
static constexpr int64_t kCooldownSeconds = 3 * 60 * 60;

// The Wii U Menu, by region. The job only runs here: kicking off several
// megabytes of downloads because someone launched a game would be rude, and the
// Menu is where the console sits idle anyway.
static bool IsWiiUMenu() {
    const uint64_t title = OSGetTitleID();
    return title == 0x0005001010040000ULL ||  // JPN
           title == 0x0005001010040100ULL ||  // USA
           title == 0x0005001010040200ULL;    // EUR
}

// The whole job, start to finish, on the background thread.
//
// Ordering is deliberate. Recovery comes first, so a container left half-written
// by a previous run is repaired before anything else touches NAND. Sending goes
// before fetching so a reply written on the console goes out even if the fetch
// later fails. Channels come last because they are by far the longest part, and
// by then the quick wins are already saved.
static void AutorunJob() {
    LOG("=== automatic run ===");
    toast::Progress progress("WiiConnect24: starting");
    s_progress = &progress;

    // Anything that leaves early has to clear this, or Report() would write
    // into a destroyed notification.
    struct ProgressScope {
        ~ProgressScope() { s_progress = nullptr; }
    } progress_scope;

    {
        VwiiNand nand;
        if (!nand.ok()) {
            progress.Finish("WiiConnect24: vWii NAND unavailable", true);
            return;
        }
        if (guard::RecoverIfNeeded(nand)) {
            toast::Info("WiiConnect24: repaired an interrupted write");
        }
    }

    net::Init();

    if (s_autoMail && !guard::StopRequested()) {
        progress.Update("WiiConnect24: sending mail");
        RunSendMail(true);
    }
    if (s_autoMail && !guard::StopRequested()) {
        progress.Update("WiiConnect24: checking mail");
        RunFetchMail(true);
    }

    if (guard::StopRequested()) {
        // Nothing was written, so there is nothing for the user to act on.
        // Say so plainly rather than raising an alarm.
        progress.Finish("WiiConnect24: postponed");
        LOG("=== automatic run stopped early (%s), nothing was modified ===",
            guard::StopReason());
        return;
    }

    progress.Update("WiiConnect24: updating channels");
    RunPipeline(true);

    if (guard::StopRequested()) {
        progress.Finish("WiiConnect24: postponed, nothing left half-written");
        LOG("stopped because: %s", guard::StopReason());
    } else {
        progress.Finish("WiiConnect24: up to date");
        // Only a run that got all the way through counts, so an interrupted
        // one is retried rather than being locked out by the cooldown.
        s_lastRun = static_cast<int64_t>(std::time(nullptr));
        WUPSStorageAPI::Store("last_run", s_lastRun);
        WUPSStorageAPI::SaveStorage();
    }
    LOG("=== automatic run done ===");
}

// ---------------------------------------------------------------------------
// Config menu
// ---------------------------------------------------------------------------

// A boolean toggled to ON triggers a one-shot action, then we reset it so it
// reads as a momentary "button". WUPS fires the callback when the menu closes.



// Persisted, unlike the action toggles: arming is meant to be a deliberate
// separate step from triggering the write.
static void OnAutorunToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    s_autorun = newValue;
    WUPSStorageAPI::Store("autorun", s_autorun);
    LOG("autorun %s", s_autorun ? "ENABLED" : "disabled");
}

static void OnAutoMailToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    s_autoMail = newValue;
    WUPSStorageAPI::Store("automail", s_autoMail);
}











// One-shot actions run on the same background thread as the automatic job, so
// closing the menu never leaves the console waiting on a download.
static void OnRunNowToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (!newValue) return;
    // Asked for directly, so the cooldown does not apply.
    if (autorun::Running()) {
        toast::Info("WiiConnect24: already running");
        return;
    }
    autorun::Start(&AutorunJob);
}

static void OnRestoreToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("Restore", [] { RunRestore(); });
    }
}

static void OnScanToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("Scan", [] { RunScan(); });
    }
}

static void OnMailCheckToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("MailCheck", [] { RunMailCheck(); });
    }
}

static void OnMailConfigToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("MailConfig", [] { RunMailConfig(); });
    }
}

static void OnDumpMailToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("DumpMail", [] { RunDumpMail(); });
    }
}

// Every channel toggle shares this; the item carries its own identifier, which
// is also its storage key.
static void OnChannelToggled(ConfigItemBoolean *item, bool newValue) {
    if (!item || !item->identifier) return;
    for (auto &c : s_channels) {
        if (std::strcmp(item->identifier, c.key) == 0) {
            c.enabled = newValue;
            WUPSStorageAPI::Store(c.key, c.enabled);
            LOG("channel %s %s", c.name, c.enabled ? "enabled" : "skipped");
            return;
        }
    }
}

static WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle root) {
    // What the plugin is for comes first; everything else is a tool.
    WUPSConfigItemBoolean_AddToCategory(
        root, "autorun", "Update at boot", true, s_autorun, &OnAutorunToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "automail", "Include mail (send and receive)", true, s_autoMail,
        &OnAutoMailToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "run_now", "Update now", false, false, &OnRunNowToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "restore", "Restore from SD backups", false, false, &OnRestoreToggled);

    // Which channels to bother with. News is singled out because it dominates
    // how long a run takes and is the one people are most likely to drop.
    WUPSConfigCategoryHandle channels;
    WUPSConfigAPICreateCategoryOptionsV1 channel_options = {.name = "Channels"};
    if (WUPSConfigAPI_Category_Create(channel_options, &channels) ==
        WUPSCONFIG_API_RESULT_SUCCESS) {
        for (const auto &c : s_channels) {
            WUPSConfigItemBoolean_AddToCategory(channels, c.key, c.menu, true, c.enabled,
                                                &OnChannelToggled);
        }
        WUPSConfigAPI_Category_AddCategory(root, channels);
    }

    // Read-only tools, tucked away. They exist so that "it did not work" can be
    // answered with evidence rather than guesswork.
    WUPSConfigCategoryHandle diagnostics;
    WUPSConfigAPICreateCategoryOptionsV1 options = {.name = "Diagnostics"};
    if (WUPSConfigAPI_Category_Create(options, &diagnostics) == WUPSCONFIG_API_RESULT_SUCCESS) {
        WUPSConfigItemBoolean_AddToCategory(
            diagnostics, "scan", "List WiiConnect24 tasks", false, false, &OnScanToggled);
        WUPSConfigItemBoolean_AddToCategory(
            diagnostics, "mail_check", "Is mail waiting?", false, false, &OnMailCheckToggled);
        WUPSConfigItemBoolean_AddToCategory(
            diagnostics, "mail_cfg", "Show mail identity", false, false, &OnMailConfigToggled);
        WUPSConfigItemBoolean_AddToCategory(
            diagnostics, "dump_mail", "Copy mailboxes to SD", false, false, &OnDumpMailToggled);
        WUPSConfigAPI_Category_AddCategory(root, diagnostics);
    }

    return WUPSCONFIG_API_CALLBACK_RESULT_SUCCESS;
}

static void ConfigMenuClosedCallback() {
    WUPSStorageAPI::SaveStorage();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

INITIALIZE_PLUGIN() {
    LogInit();
    net::Init();

    WUPSStorageAPI::GetOrStoreDefault("autorun", s_autorun, true);
    WUPSStorageAPI::GetOrStoreDefault("automail", s_autoMail, true);
    WUPSStorageAPI::GetOrStoreDefault("last_run", s_lastRun, static_cast<int64_t>(0));
    for (auto &c : s_channels) {
        WUPSStorageAPI::GetOrStoreDefault(c.key, c.enabled, true);
    }

    // The console's own resolver handles every host we actually use; stale
    // Nintendo hostnames are dealt with by kHostOverrides instead.
    net::SetDnsServer(nullptr);

    WUPSConfigAPIOptionsV1 options = {.name = "wuc24"};
    if (WUPSConfigAPI_Init(options, ConfigMenuOpenedCallback, ConfigMenuClosedCallback) !=
        WUPSCONFIG_API_RESULT_SUCCESS) {
        LOG("WUPSConfigAPI_Init failed");
    }
    LOG("wuc24 initialised");
}

DEINITIALIZE_PLUGIN() {
    autorun::Stop("plugin deinit", 5000);
    toast::Shutdown();
    LOG("wuc24 deinitialised");
    LogDeinit();
}

ON_APPLICATION_START() {
    LogInit();
    net::Init();
    toast::Init();

    const uint64_t title = OSGetTitleID();
    LOG("application started: title %016llx", static_cast<unsigned long long>(title));

    if (!s_autorun) return;
    if (!IsWiiUMenu()) {
        LOG("autorun: not the Wii U Menu, staying out of the way");
        return;
    }

    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const int64_t age = now - s_lastRun;
    if (s_lastRun > 0 && age >= 0 && age < kCooldownSeconds) {
        LOG("autorun: last run was %lld minutes ago, waiting until %lld have passed",
            static_cast<long long>(age / 60), static_cast<long long>(kCooldownSeconds / 60));
        return;
    }

    LOG("autorun: starting the background job");
    autorun::Start(&AutorunJob);
}

// The title is going away -- the user launched vWii, started a game, or is
// shutting down. Ask the job to stop and give a write in flight time to land.
ON_APPLICATION_REQUESTS_EXIT() {
    if (autorun::Running()) LOG("autorun: exit requested, winding up");
    autorun::Stop("application requested exit", 5000);
}

ON_APPLICATION_ENDS() {
    autorun::Stop("application ended", 5000);
    toast::Shutdown();
}
