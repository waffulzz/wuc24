#include "nand.h"

#include <cstring>

#include <mocha/mocha.h>

#include "log.h"

namespace {
// libmocha mounts the device here in the FSA namespace.
constexpr char kVirtName[]   = "slccmpt";
constexpr char kDevPath[]    = "/dev/slccmpt01";
constexpr char kMountPath[]  = "/vol/storage_slccmpt01";
}  // namespace

VwiiNand::VwiiNand() {
    if (Mocha_InitLibrary() != MOCHA_RESULT_SUCCESS) {
        LOG("Mocha_InitLibrary failed — is Aroma/mocha present?");
        return;
    }
    m_mochaInit = true;

    MochaUtilsStatus ms = Mocha_MountFS(kVirtName, kDevPath, kMountPath);
    if (ms != MOCHA_RESULT_SUCCESS && ms != MOCHA_RESULT_ALREADY_EXISTS) {
        LOG("Mocha_MountFS(%s) failed: %s", kDevPath, Mocha_GetStatusStr(ms));
        return;
    }
    m_mounted = true;

    if (FSAInit() != FS_ERROR_OK) {
        LOG("FSAInit failed");
        return;
    }
    m_client = FSAAddClient(nullptr);
    if (m_client <= 0) {
        LOG("FSAAddClient failed (%d)", m_client);
        return;
    }
    m_fsaAdded = true;

    // Without this the FSA client is not permitted to touch a raw-mounted NAND.
    if (Mocha_UnlockFSClientEx(m_client) != MOCHA_RESULT_SUCCESS) {
        LOG("Mocha_UnlockFSClientEx failed");
        return;
    }

    m_ready = true;
    LOG("vWii NAND mounted at %s", kMountPath);
}

VwiiNand::~VwiiNand() {
    if (m_fsaAdded) {
        FSADelClient(m_client);
    }
    if (m_mounted) {
        Mocha_UnmountFS(kVirtName);
    }
    if (m_mochaInit) {
        Mocha_DeInitLibrary();
    }
}

std::string VwiiNand::FullPath(const char *vwii_path) const {
    return std::string(kMountPath) + vwii_path;
}

int64_t VwiiNand::FileSize(const char *vwii_path) {
    if (!m_ready) return -1;
    FSAStat stat{};
    if (FSAGetStat(m_client, FullPath(vwii_path).c_str(), &stat) != FS_ERROR_OK) {
        return -1;
    }
    return static_cast<int64_t>(stat.size);
}

bool VwiiNand::ListDir(const char *vwii_path, std::vector<DirEntry> &out) {
    out.clear();
    if (!m_ready) return false;

    const std::string  path = FullPath(vwii_path);
    FSADirectoryHandle dir  = 0;
    const FSError err = FSAOpenDir(m_client, path.c_str(), &dir);
    if (err != FS_ERROR_OK) {
        LOG("opendir %s failed: %s", vwii_path, FSAGetStatusStr(err));
        return false;
    }

    for (;;) {
        FSADirectoryEntry entry{};
        if (FSAReadDir(m_client, dir, &entry) != FS_ERROR_OK) break;  // end of directory

        DirEntry e;
        e.name      = entry.name;
        e.size      = entry.info.size;
        e.flags     = static_cast<uint32_t>(entry.info.flags);
        e.is_dir    = (entry.info.flags & FS_STAT_DIRECTORY) != 0;
        e.encrypted = (entry.info.flags & FS_STAT_ENCRYPTED_FILE) != 0;
        out.push_back(std::move(e));
    }

    FSACloseDir(m_client, dir);
    return true;
}

bool VwiiNand::ReadFile(const char *vwii_path, std::vector<uint8_t> &out) {
    if (!m_ready) return false;

    const std::string path = FullPath(vwii_path);
    FSAFileHandle handle   = 0;
    FSError err = FSAOpenFileEx(m_client, path.c_str(), "r",
                                static_cast<FSMode>(0x660), FS_OPEN_FLAG_NONE, 0, &handle);
    if (err != FS_ERROR_OK) {
        LOG("open %s failed: %s", vwii_path, FSAGetStatusStr(err));
        return false;
    }

    FSAStat stat{};
    if (FSAGetStatFile(m_client, handle, &stat) != FS_ERROR_OK) {
        LOG("statfile %s failed", vwii_path);
        FSACloseFile(m_client, handle);
        return false;
    }

    out.resize(stat.size);
    uint32_t total = 0;
    while (total < stat.size) {
        int r = FSAReadFile(m_client, out.data() + total, 1, stat.size - total, handle, 0);
        if (r <= 0) {
            LOG("read %s failed at %u: %s", vwii_path, total, FSAGetStatusStr(static_cast<FSError>(r)));
            FSACloseFile(m_client, handle);
            return false;
        }
        total += static_cast<uint32_t>(r);
    }

    FSACloseFile(m_client, handle);
    return true;
}

bool VwiiNand::Flush() {
    if (!m_ready) return false;
    const FSError err = FSAFlushVolume(m_client, kMountPath);
    if (err != FS_ERROR_OK) {
        LOG("flush failed: %s", FSAGetStatusStr(err));
        return false;
    }
    return true;
}

bool VwiiNand::CopyFileTo(const char *vwii_path, FILE *dest) {
    if (!m_ready || !dest) return false;

    const std::string path   = FullPath(vwii_path);
    FSAFileHandle     handle = 0;
    FSError err = FSAOpenFileEx(m_client, path.c_str(), "r", static_cast<FSMode>(0x660),
                                FS_OPEN_FLAG_NONE, 0, &handle);
    if (err != FS_ERROR_OK) {
        LOG("open %s failed: %s", vwii_path, FSAGetStatusStr(err));
        return false;
    }

    // Small enough to be safe in a plugin heap, large enough to keep the
    // number of NAND round trips reasonable.
    constexpr uint32_t kChunk = 128 * 1024;
    std::vector<uint8_t> buffer;
    buffer.resize(kChunk);

    bool ok = true;
    for (;;) {
        const int got = FSAReadFile(m_client, buffer.data(), 1, kChunk, handle, 0);
        if (got == 0) break;  // end of file
        if (got < 0) {
            LOG("read %s failed: %s", vwii_path, FSAGetStatusStr(static_cast<FSError>(got)));
            ok = false;
            break;
        }
        if (std::fwrite(buffer.data(), 1, static_cast<size_t>(got), dest) !=
            static_cast<size_t>(got)) {
            LOG("short write while copying %s", vwii_path);
            ok = false;
            break;
        }
    }

    FSACloseFile(m_client, handle);
    return ok;
}

bool VwiiNand::WriteFile(const char *vwii_path, const void *data, uint32_t size) {
    if (!m_ready) return false;

    const std::string path = FullPath(vwii_path);
    FSAFileHandle handle   = 0;
    FSError err = FSAOpenFileEx(m_client, path.c_str(), "r+",
                                static_cast<FSMode>(0x660), FS_OPEN_FLAG_NONE, 0, &handle);
    if (err != FS_ERROR_OK) {
        LOG("open(w) %s failed: %s", vwii_path, FSAGetStatusStr(err));
        return false;
    }

    uint32_t total = 0;
    const uint8_t *p = static_cast<const uint8_t *>(data);
    while (total < size) {
        int w = FSAWriteFile(m_client, const_cast<uint8_t *>(p + total), 1, size - total, handle, 0);
        if (w <= 0) {
            LOG("write %s failed at %u: %s", vwii_path, total, FSAGetStatusStr(static_cast<FSError>(w)));
            FSACloseFile(m_client, handle);
            return false;
        }
        total += static_cast<uint32_t>(w);
    }

    FSACloseFile(m_client, handle);
    return true;
}
