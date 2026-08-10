// wuc24 — WiiConnect24-for-vWii downloader, as an Aroma (WUPS) plugin.
//
// Goal: from Wii U (Cafe OS) mode, fetch WiiConnect24 content from WiiLink and
// write it into the vWii NAND so the vWii channels have fresh data — without
// booting into vWii.
//
// Milestone status (read the README for the full plan):
//   [x] Plugin scaffold + config menu
//   [x] vWii NAND (slccmpt) mount via libmocha + FSA
//   [x] Read & summarise nwc24dl.bin download tasks   <-- current: read-only
//   [ ] VFF read/write, HTTPS client, WC24 decode, NAND writes
//
// Right now the plugin is READ-ONLY: the "Scan" action mounts the vWii NAND and
// dumps the download-task list to the log. Nothing is ever written to NAND yet.
// "Fetch (test)" does a real HTTP GET of one live WC24 task and dumps the raw
// bytes to SD -- no NAND writes, no decode. It's a connectivity/DNS probe and
// the first real sample data for the WC24 decode work.
#include <cstdio>
#include <cstring>
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
WUPS_PLUGIN_VERSION("v0.1-dev");
WUPS_PLUGIN_AUTHOR("anonymous");
WUPS_PLUGIN_LICENSE("GPLv2");

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

// Test target: entry [3] from a live scan -- WiiLink's Forecast Channel
// subtask. Hardcoded here deliberately; this action exists to validate
// DNS + plain-HTTP connectivity from Wii U mode, not to be a real feature.
static constexpr char kTestFetchUrl[] = "http://fore.wiilink24.com///1/049/forecast.bin";
static constexpr char kTestFetchOutFile[] = "wuc24_forecast_raw.bin";

// Real HTTP GET of one live WC24 task, raw bytes dumped to SD. No NAND access.
static void RunFetchTest() {
    LOG("=== fetch test: %s ===", kTestFetchUrl);

    net::Init();

    std::vector<uint8_t> body;
    int status = 0;
    if (!net::HttpGet(kTestFetchUrl, body, status)) {
        LOG("fetch test failed (no response parsed)");
        return;
    }

    const char *mount = GetSdMountPath();
    if (!mount) {
        LOG("fetch test: SD not mounted, can't save %s", kTestFetchOutFile);
        return;
    }

    char out_path[512];
    std::snprintf(out_path, sizeof(out_path), "%s/%s", mount, kTestFetchOutFile);
    FILE *f = std::fopen(out_path, "wb");
    if (!f) {
        LOG("fetch test: failed to open %s for writing", out_path);
        return;
    }
    std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);

    LOG("fetch test: HTTP %d, %zu bytes saved to %s", status, body.size(), out_path);
    LOG("=== fetch test done ===");
}

// Same test target as RunFetchTest(): entry [3], Forecast Channel
// (low_title_id=0x00010002, high_title_id="HAFE" -> 0x48414645).
static constexpr char kTestVffPath[] = "/title/00010002/48414645/data/wc24dl.vff";
static constexpr char kTestVffOutFile[] = "wuc24_forecast_vff_raw.bin";

// Read-only: pull the real on-NAND VFF container for one channel and log its
// header fields, to validate our VFF layout understanding against real data
// before writing any FAT code. No NAND writes.
static void RunDumpVff() {
    LOG("=== dump vff: %s ===", kTestVffPath);
    VwiiNand nand;
    if (!nand.ok()) {
        LOG("dump vff aborted: vWii NAND not available");
        return;
    }

    std::vector<uint8_t> raw;
    if (!nand.ReadFile(kTestVffPath, raw)) {
        LOG("dump vff: could not read %s", kTestVffPath);
        return;
    }
    LOG("dump vff: read %zu bytes", raw.size());

    if (raw.size() >= sizeof(wc24::VffHeader)) {
        wc24::VffHeader hdr;
        std::memcpy(&hdr, raw.data(), sizeof(hdr));
        LOG("  magic='%.4s' endian=0x%04X (big=0x%04X) volume_size=%u cluster_size_field=%u "
            "(actual=%u) unknown_marker=0x%04X unknown=0x%04X",
            hdr.magic, hdr.endianness, wc24::kVffEndianBig, hdr.volume_size,
            hdr.cluster_size, hdr.cluster_size * 16, hdr.unknown_marker, hdr.unknown);
    } else {
        LOG("  file too small to hold a VffHeader (%zu < %zu)", raw.size(), sizeof(wc24::VffHeader));
    }

    const char *mount = GetSdMountPath();
    if (!mount) {
        LOG("dump vff: SD not mounted, can't save %s", kTestVffOutFile);
        return;
    }
    char out_path[512];
    std::snprintf(out_path, sizeof(out_path), "%s/%s", mount, kTestVffOutFile);
    FILE *f = std::fopen(out_path, "wb");
    if (!f) {
        LOG("dump vff: failed to open %s for writing", out_path);
        return;
    }
    std::fwrite(raw.data(), 1, raw.size(), f);
    std::fclose(f);
    LOG("dump vff: saved to %s", out_path);
    LOG("=== dump vff done ===");
}

// Set by the "arm" config item. NAND writes refuse to run without it, so a
// single stray toggle can never modify the console.
static bool s_armed = false;

// Resolve through WiiLink's DNS rather than the console's.
//
// OFF by default. The idea was that WiiLink's resolver redirects the dead
// Nintendo hostnames still listed in nwc24dl.bin, but it does not answer
// queries at all -- verified from three separate networks, including a control
// lookup through 1.1.1.1 that succeeds from the same console. Stale hostnames
// are handled by kHostOverrides instead, which is also what WiiLink's own
// patchers do. Left as an option in case the server comes back.
static bool s_useWiiLinkDns = false;

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
    std::vector<DownloadUnit> units;
};

// Collects the containers to update and the files destined for each.
static void BuildJobs(const std::vector<DlTaskInfo> &tasks, std::vector<ContainerJob> &jobs,
                      bool log_skips) {
    for (const auto &t : tasks) {
        const char *why_not = nullptr;
        if (!IsEligible(t, &why_not)) {
            if (log_skips) LOG("skipping [%u] %s: %s", t.index, t.filename.c_str(), why_not);
            continue;
        }

        ContainerJob *job = nullptr;
        for (auto &j : jobs) {
            if (j.vff_path == t.vff_path) job = &j;
        }
        if (!job) {
            jobs.push_back(ContainerJob{t.vff_path, BackupNameFor(t), {}});
            job = &jobs.back();
        }
        ExpandTask(t, job->units);
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

    if (commit && !s_armed && !autorun::Running()) {
        LOG("refusing to write: enable \"Arm NAND write\" first");
        return;
    }

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

    for (const auto &job : jobs) {
        LOG("--- %s (%zu file(s)) ---", job.vff_path.c_str(), job.units.size());

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

// Writes a synthetic message into the inbox, to prove the format is right
// before any of it depends on talking to a server. Backs the inbox up first
// and refuses without arming.
static void RunInjectTestMail(bool commit) {
    LOG("=== inject test mail (%s) ===", commit ? "COMMIT" : "dry run");

    if (commit && !s_armed) {
        LOG("refusing to write: enable \"Arm NAND write\" first");
        return;
    }

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("aborted: vWii NAND not available");
        return;
    }

    msgcfg::Config cfg;
    if (!msgcfg::Read(nand, cfg) || cfg.WiiAddress().empty()) {
        LOG("aborted: could not determine this console's mail address");
        return;
    }
    LOG("delivering to %s", cfg.WiiAddress().c_str());

    // Back the inbox up before touching it -- both halves, since a half-written
    // pair is the one state that would be awkward to recover from.
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

    mail::Message msg;
    msg.from     = "w0000000000000000@rc24.xyz";
    msg.to       = cfg.WiiAddress();
    msg.subject  = "Wii Message";
    msg.alt_name = "wuc24";
    msg.body =
        "Hello from wuc24.\n\n"
        "This message was written straight into the vWii inbox\n"
        "from Wii U mode. If you can read it on the message\n"
        "board, the mail format is correct.\n";

    if (!mail::Deliver(nand, msg, commit)) {
        LOG("delivery failed -- backups are on SD as wuc24_bak_recv_*.bin");
        return;
    }

    if (commit) {
        LOG("written. Boot vWii and look at the message board.");
    }
    LOG("=== inject test mail done ===");
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

    if (!s_armed && !autorun::Running()) {
        LOG("refusing: enable \"Arm NAND write\" first.");
        LOG("note: fetching consumes mail server-side even without writing.");
        return;
    }

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

    if (commit && !s_armed && !autorun::Running()) {
        LOG("refusing to write: enable \"Arm NAND write\" first");
        return;
    }

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

// Minimal write test for the inbox index.
//
// Injecting a message and finding the file reverted afterwards has two very
// different explanations: the Wii Menu rejected our entry, or something
// restores /shared2/wc24 wholesale regardless of what we put there. This
// changes ONE inert counter -- no entries, no message, nothing to validate --
// so if even that does not survive a vWii boot, entry correctness is not the
// problem and Route A is dead on this hardware.
static void RunMailProbe() {
    LOG("=== mail probe ===");

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("aborted: vWii NAND not available");
        return;
    }

    std::vector<uint8_t> ctl;
    if (!nand.ReadFile(wc24::kWc24RecvCtl, ctl) || ctl.size() < sizeof(wc24::MailListHeader)) {
        LOG("aborted: cannot read the inbox index");
        return;
    }

    wc24::MailListHeader header;
    std::memcpy(&header, ctl.data(), sizeof(header));
    if (header.magic != wc24::kMailListMagic) {
        LOG("aborted: bad magic 0x%08X", header.magic);
        return;
    }

    const uint32_t before = header.next_entry_id;

    // Always report first: unarmed, this is a read-only way to see whether a
    // previous bump survived a trip through vWii.
    LOG("inbox now reads: next_entry_id=%u number_of_mail=%u next_entry_offset=%u", before,
        header.number_of_mail, header.next_entry_offset);

    if (!s_armed) {
        LOG("not armed -- reporting only, nothing written.");
        LOG("=== mail probe done ===");
        return;
    }

    if (!SaveToSd("wuc24_bak_recv_ctl.bin", ctl.data(), ctl.size())) {
        LOG("aborted: could not back it up");
        return;
    }

    header.next_entry_id  = before + 1;
    std::memcpy(ctl.data(), &header, sizeof(header));

    if (!nand.WriteFile(wc24::kWc24RecvCtl, ctl.data(), static_cast<uint32_t>(ctl.size()))) {
        LOG("write FAILED");
        return;
    }

    std::vector<uint8_t> check;
    if (!nand.ReadFile(wc24::kWc24RecvCtl, check)) {
        LOG("cannot read back");
        return;
    }
    wc24::MailListHeader after;
    std::memcpy(&after, check.data(), sizeof(after));

    LOG("next_entry_id %u -> %u, reads back as %u  [%s]", before, before + 1, after.next_entry_id,
        after.next_entry_id == before + 1 ? "PERSISTED" : "NOT PERSISTED");
    LOG("Now boot vWii, come back, and run this again WITHOUT arming --");
    LOG("if it reads %u again, the console reverted the file.", before);
    LOG("=== mail probe done ===");
}

// Read-only: list the places a received message could be living.
//
// /shared2/wc24/mbox is only a transit box -- KD downloads into it and the Wii
// Menu then moves messages into its own storage and clears it, which is why a
// message visible on the console does not appear there. This walks the likely
// locations so the real message store can be found rather than guessed at.
static void RunExplore() {
    LOG("=== explore NAND (read-only) ===");

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("explore aborted: vWii NAND not available");
        return;
    }

    const char *paths[] = {
        "/shared2/wc24",
        "/shared2/wc24/mbox",
        "/title/00000001/00000002/data",     // Wii Menu
        "/title/00000001/00000002/content",
        "/shared2",
        "/title/00010002/48414541/data",     // HAEA -- the mail/board task's title
    };

    for (const char *path : paths) {
        std::vector<VwiiNand::DirEntry> entries;
        if (!nand.ListDir(path, entries)) {
            LOG("--- %s : cannot list ---", path);
            continue;
        }
        LOG("--- %s (%zu entries) ---", path, entries.size());
        for (const auto &e : entries) {
            LOG("    %-24s %10u %s%s", e.name.c_str(), e.size, e.is_dir ? "<dir>" : "     ",
                e.encrypted ? "  [encrypted]" : "");
        }
    }

    LOG("=== explore done ===");
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

// Read-only: resolve every hostname in the task list both through the
// configured DNS and through the console's own resolver, and show them side by
// side. This is the only way to see the legacy Nintendo hostnames, since the
// tasks using them are all mail or encrypted and so never reach a download.
// If WiiLink's DNS is doing its job, those rows resolve to a WiiLink address
// via custom DNS and to a dead Nintendo/Akamai/AWS address via the system one.
static void RunDnsTest() {
    LOG("=== DNS test (read-only) ===");

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("dns test aborted: vWii NAND not available");
        return;
    }

    std::vector<DlTaskInfo> tasks;
    if (!ScanDlTasks(nand, tasks)) {
        LOG("dns test aborted: could not read the task list");
        return;
    }

    net::Init();

    // Control first: if our DNS client can talk to a known-good public
    // resolver but not to WiiLink's, the client is fine and the problem is
    // WiiLink's server or the network path to it. If BOTH fail, the fault is
    // on our side.
    std::string control_ip;
    const bool  control_ok =
        net::ResolveVia(net::kControlDns, "example.com", control_ip);
    LOG("control: example.com via %s -> %s  [%s]", net::kControlDns, control_ip.c_str(),
        control_ok ? "DNS CLIENT WORKS" : "DNS CLIENT NOT WORKING");

    std::string wiilink_ip;
    const bool  wiilink_ok =
        net::ResolveVia(net::kWiiLinkDns, "example.com", wiilink_ip);
    LOG("control: example.com via %s -> %s  [%s]", net::kWiiLinkDns, wiilink_ip.c_str(),
        wiilink_ok ? "WIILINK DNS ANSWERS" : "WIILINK DNS SILENT");

    if (control_ok && !wiilink_ok) {
        LOG("=> our DNS client is fine; WiiLink's resolver is not reachable from here.");
        LOG("   (some routers/ISPs block outbound port 53 to anything but their own)");
    } else if (!control_ok) {
        LOG("=> no DNS server answered at all -- likely a fault in the plugin or blocked UDP/53.");
    }

    std::vector<std::string> seen;
    LOG("%-42s %-16s %-16s", "host", "wiilink dns", "console dns");
    for (const auto &t : tasks) {
        const std::string host = net::HostOf(t.url);
        if (host.empty()) continue;

        bool already = false;
        for (const auto &h : seen) {
            if (h == host) already = true;
        }
        if (already) continue;
        seen.push_back(host);

        std::string via_custom, via_system;
        net::ResolveVia(net::kWiiLinkDns, host, via_custom);
        net::ResolveViaSystem(host, via_system);
        LOG("%-42s %-16s %-16s%s", host.c_str(), via_custom.c_str(), via_system.c_str(),
            (via_custom != via_system && via_custom != "-") ? "  <- redirected" : "");
    }

    LOG("=== dns test done ===");
}

// Read-only probe: for every eligible task, ask the server for the plain URL
// and then for numbered subtask variants (url.00, url.01, ...). KD fans a
// single list entry out into several numbered downloads for some channels, and
// this reports which ones actually exist rather than guessing at the encoding
// of subtask_bitmask. Stops after two consecutive misses. Touches nothing.
static void RunProbeSubtasks() {
    LOG("=== probe subtask URLs (read-only) ===");

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("probe aborted: vWii NAND not available");
        return;
    }

    std::vector<DlTaskInfo> tasks;
    if (!ScanDlTasks(nand, tasks)) {
        LOG("probe aborted: could not read the task list");
        return;
    }

    net::Init();

    for (const auto &t : tasks) {
        const char *why_not = nullptr;
        if (!IsEligible(t, &why_not)) continue;

        LOG("--- [%u] %s (file '%s', sub_type=%u sub_bitmask=0x%08X)", t.index, t.url.c_str(),
            t.filename.c_str(), t.subtask_type, t.subtask_bitmask);

        std::vector<uint8_t> body;
        int status = 0;
        if (net::HttpGet(t.url, body, status)) {
            LOG("    plain        -> HTTP %d, %zu bytes", status, body.size());
        } else {
            LOG("    plain        -> request failed");
        }

        int misses = 0;
        for (int i = 0; i < 8 && misses < 2; i++) {
            char suffixed[512];
            std::snprintf(suffixed, sizeof(suffixed), "%s.%02d", t.url.c_str(), i);

            std::vector<uint8_t> sub_body;
            int sub_status = 0;
            if (net::HttpGet(suffixed, sub_body, sub_status) && sub_status == 200) {
                LOG("    .%02d          -> HTTP 200, %zu bytes", i, sub_body.size());
                misses = 0;
            } else {
                LOG("    .%02d          -> HTTP %d", i, sub_status);
                misses++;
            }
        }
    }

    LOG("=== probe done ===");
}

// Deletes the downloaded files from every container we would otherwise write
// to, so a channel has no WC24 data at all. Used to tell "the channel is
// showing data we wrote" apart from "the channel downloaded it itself".
static void RunClear() {
    LOG("=== CLEAR WC24 data ===");

    if (!s_armed) {
        LOG("refusing to write: enable \"Arm NAND write\" first");
        return;
    }

    VwiiNand nand;
    if (!nand.ok()) {
        LOG("clear aborted: vWii NAND not available");
        return;
    }

    std::vector<DlTaskInfo> tasks;
    if (!ScanDlTasks(nand, tasks)) {
        LOG("clear aborted: could not read the task list");
        return;
    }

    std::vector<ContainerJob> jobs;
    BuildJobs(tasks, jobs, false);

    for (const auto &job : jobs) {
        LOG("--- %s (%zu file(s)) ---", job.vff_path.c_str(), job.units.size());

        std::vector<uint8_t> buffer;
        if (!nand.ReadFile(job.vff_path.c_str(), buffer)) {
            LOG("  could not read the container -- skipped");
            continue;
        }
        if (!SaveToSd(job.backup_name.c_str(), buffer.data(), buffer.size())) {
            LOG("  refusing to continue without a backup on SD -- skipped");
            continue;
        }

        bool ok = true;
        {
            vff::Image image(buffer);
            if (!image.ok()) {
                LOG("  could not mount the container -- skipped");
                continue;
            }
            for (const auto &unit : job.units) {
                if (!image.DeleteFile(unit.filename.c_str())) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                LOG("  deleted %zu file(s)", job.units.size());
                std::vector<vff::Image::Entry> entries;
                if (image.List(entries)) {
                    LOG("  container now holds:");
                    for (const auto &e : entries) {
                        LOG("      %-16s %8u %s", e.name.c_str(), e.size,
                            e.is_dir ? "<dir>" : "");
                    }
                }
            }
        }
        if (!ok) {
            LOG("  container left untouched");
            continue;
        }

        if (!nand.WriteFile(job.vff_path.c_str(), buffer.data(),
                            static_cast<uint32_t>(buffer.size()))) {
            LOG("  WRITE FAILED -- restoring the original");
            RestoreContainer(nand, job);
            continue;
        }
        LOG("  cleared (backup: %s)", job.backup_name.c_str());
    }

    LOG("=== clear done ===");
}

// Puts back whatever BackupNameFor() last saved for each container.
static void RunRestore() {
    LOG("=== RESTORE from SD backups ===");

    if (!s_armed) {
        LOG("refusing to write: enable \"Arm NAND write\" first");
        return;
    }

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
    BuildJobs(tasks, jobs, false);

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

// Run everything on its own when a title starts. Off by default: it writes to
// NAND, which should be a decision, not a surprise.
static bool s_autorun = false;

// Whether to hand queued mail to the server, and to pull down waiting mail.
static bool s_autoMail = true;

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
        progress.Finish("WiiConnect24: stopped");
        LOG("=== automatic run stopped early ===");
        return;
    }

    progress.Update("WiiConnect24: updating channels");
    RunPipeline(true);

    if (guard::StopRequested()) {
        progress.Finish("WiiConnect24: stopped, nothing left half-written");
    } else {
        progress.Finish("WiiConnect24: up to date");
    }
    LOG("=== automatic run done ===");
}

// ---------------------------------------------------------------------------
// Config menu
// ---------------------------------------------------------------------------

// A boolean toggled to ON triggers a one-shot action, then we reset it so it
// reads as a momentary "button". WUPS fires the callback when the menu closes.
static void OnScanToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("Scan", [] { RunScan(); });
    }
}

static void OnFetchTestToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("FetchTest", [] { RunFetchTest(); });
    }
}

static void OnDumpVffToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("DumpVff", [] { RunDumpVff(); });
    }
}

static void OnDryRunToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("DryRun", [] { RunPipeline(false); });
    }
}

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

static void OnArmToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    s_armed = newValue;
    WUPSStorageAPI::Store("armed", s_armed);
    LOG("NAND write %s", s_armed ? "ARMED" : "disarmed");
}

static void OnWiiLinkDnsToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    s_useWiiLinkDns = newValue;
    WUPSStorageAPI::Store("wiilink_dns", s_useWiiLinkDns);
    net::SetDnsServer(s_useWiiLinkDns ? net::kWiiLinkDns : nullptr);
}

static void OnCommitToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("Commit", [] { RunPipeline(true); });
    }
}

static void OnMailCheckToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("MailCheck", [] { RunMailCheck(); });
    }
}

static void OnFetchMailToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("FetchMail", [] { RunFetchMail(true); });
    }
}

static void OnSendMailDryToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("SendMailDry", [] { RunSendMail(false); });
    }
}

static void OnSendMailToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("SendMail", [] { RunSendMail(true); });
    }
}

static void OnMailProbeToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("MailProbe", [] { RunMailProbe(); });
    }
}

static void OnMailConfigToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("MailConfig", [] { RunMailConfig(); });
    }
}

static void OnInjectDryToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("InjectDry", [] { RunInjectTestMail(false); });
    }
}

static void OnInjectToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("Inject", [] { RunInjectTestMail(true); });
    }
}

static void OnExploreToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("Explore", [] { RunExplore(); });
    }
}

static void OnDumpMailToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("DumpMail", [] { RunDumpMail(); });
    }
}

static void OnDnsTestToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("DnsTest", [] { RunDnsTest(); });
    }
}

static void OnProbeToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("Probe", [] { RunProbeSubtasks(); });
    }
}

static void OnClearToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("Clear", [] { RunClear(); });
    }
}

static void OnRestoreToggled(ConfigItemBoolean * /*item*/, bool newValue) {
    if (newValue) {
        RunGuarded("Restore", [] { RunRestore(); });
    }
}

static WUPSConfigAPICallbackStatus ConfigMenuOpenedCallback(WUPSConfigCategoryHandle root) {
    // The automatic behaviour comes first: it is what most people want, and
    // everything below it is a manual tool.
    WUPSConfigItemBoolean_AddToCategory(
        root, "autorun", "Update automatically at boot (writes to vWii NAND)",
        false, s_autorun, &OnAutorunToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "automail", "  ...including sending and receiving mail",
        true, s_autoMail, &OnAutoMailToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "scan", "Scan vWii WC24 tasks (read-only) — toggle ON + close menu",
        false, false, &OnScanToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "fetch_test", "Fetch test: forecast.bin -> SD (no NAND write) — toggle ON + close menu",
        false, false, &OnFetchTestToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "dump_vff", "Dump vWii Forecast VFF (read-only) -> SD — toggle ON + close menu",
        false, false, &OnDumpVffToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "wiilink_dns", "Resolve via WiiLink DNS (167.235.229.36) — currently unresponsive",
        false, s_useWiiLinkDns, &OnWiiLinkDnsToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "mail_check", "MAIL CHECK: is mail waiting on the server? (read-only)",
        false, false, &OnMailCheckToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "mail_cfg", "MAIL CONFIG: show this console's mail identity (read-only)",
        false, false, &OnMailConfigToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "inject_dry", "TEST MAIL (dry run): build an inbox message, no NAND write",
        false, false, &OnInjectDryToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "explore", "EXPLORE: list NAND dirs to find the message store (read-only)",
        false, false, &OnExploreToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "dump_mail", "DUMP MAIL: copy the WC24 mailboxes to SD (read-only)",
        false, false, &OnDumpMailToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "dns_test", "DNS TEST: resolve every task host, both ways (read-only)",
        false, false, &OnDnsTestToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "probe", "PROBE: which subtask URLs exist on the servers (read-only)",
        false, false, &OnProbeToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "dry_run", "DRY RUN: download + update all containers -> SD only (no NAND write)",
        false, false, &OnDryRunToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "armed", "Arm NAND write (required by the manual write actions)",
        false, s_armed, &OnArmToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "commit", "COMMIT: download + write all containers to vWii NAND",
        false, false, &OnCommitToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "clear", "CLEAR: delete WC24 data from the containers (for testing)",
        false, false, &OnClearToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "send_dry", "SEND MAIL (dry run): list the outbox, no network at all",
        false, false, &OnSendMailDryToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "send_mail", "SEND MAIL: send queued outbox mail (needs arming)",
        false, false, &OnSendMailToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "fetch_mail", "FETCH MAIL: download waiting mail into the inbox (needs arming)",
        false, false, &OnFetchMailToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "mail_probe", "MAIL PROBE: bump one inbox counter (needs arming)",
        false, false, &OnMailProbeToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "inject", "TEST MAIL: write a message into the vWii inbox (needs arming)",
        false, false, &OnInjectToggled);
    WUPSConfigItemBoolean_AddToCategory(
        root, "restore", "RESTORE: put the SD backups back onto NAND",
        false, false, &OnRestoreToggled);
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

    WUPSStorageAPI::GetOrStoreDefault("armed", s_armed, false);
    WUPSStorageAPI::GetOrStoreDefault("autorun", s_autorun, false);
    WUPSStorageAPI::GetOrStoreDefault("automail", s_autoMail, true);
    WUPSStorageAPI::GetOrStoreDefault("wiilink_dns", s_useWiiLinkDns, false);
    net::SetDnsServer(s_useWiiLinkDns ? net::kWiiLinkDns : nullptr);

    WUPSConfigAPIOptionsV1 options = {.name = "wuc24"};
    if (WUPSConfigAPI_Init(options, ConfigMenuOpenedCallback, ConfigMenuClosedCallback) !=
        WUPSCONFIG_API_RESULT_SUCCESS) {
        LOG("WUPSConfigAPI_Init failed");
    }
    LOG("wuc24 initialised");
}

DEINITIALIZE_PLUGIN() {
    autorun::Stop(5000);
    toast::Shutdown();
    LOG("wuc24 deinitialised");
    LogDeinit();
}

ON_APPLICATION_START() {
    LogInit();
    net::Init();
    toast::Init();

    if (!s_autorun) return;
    if (!IsWiiUMenu()) {
        LOG("autorun: not the Wii U Menu, staying out of the way");
        return;
    }
    LOG("autorun: starting the background job");
    autorun::Start(&AutorunJob);
}

// The title is going away -- the user launched vWii, started a game, or is
// shutting down. Ask the job to stop and give a write in flight time to land.
ON_APPLICATION_REQUESTS_EXIT() {
    if (autorun::Running()) LOG("autorun: exit requested, winding up");
    autorun::Stop(5000);
}

ON_APPLICATION_ENDS() {
    autorun::Stop(5000);
    toast::Shutdown();
}
