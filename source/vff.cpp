#include "vff.h"

#include <cstring>

extern "C" {
#include "ff.h"
#include "diskio.h"
}

#include "log.h"

namespace {

// The single VFF image FatFs is currently operating on. FatFs's disk_* hooks
// are global C functions with no user-data parameter, so the current image has
// to live here.
std::vector<uint8_t> *g_buf = nullptr;

// VFF sectors are 512 bytes, but "sector 1" starts at file byte 32 (right
// after the 32-byte header) rather than byte 512. Hence the -480 skew.
// Sector 0 is not addressable.
constexpr uint32_t kSectorSize   = 512;
constexpr int32_t  kSectorOffset = -480;

constexpr uint16_t kEndianBig    = 0xFEFF;
constexpr uint16_t kEndianLittle = 0xFFFE;

// The VFF header is big-endian. Read it explicitly so this code behaves
// identically on the big-endian console and on a little-endian host (which is
// what lets us test the format offline).
inline uint16_t ReadBE16(const uint8_t *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t ReadBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

inline uint32_t AlignUp(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

// Maps a FatFs sector to a byte range in the buffer, bounds-checked.
// Returns false if the access would fall outside the image.
bool SectorRange(uint32_t sector, uint32_t count, size_t &offset, size_t &length) {
    if (!g_buf || sector == 0) return false;

    const int64_t start = static_cast<int64_t>(sector) * kSectorSize + kSectorOffset;
    const int64_t size  = static_cast<int64_t>(count) * kSectorSize;
    if (start < 0 || size < 0) return false;
    if (static_cast<uint64_t>(start + size) > g_buf->size()) return false;

    offset = static_cast<size_t>(start);
    length = static_cast<size_t>(size);
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// FatFs disk hooks. These operate on g_buf.
//
// Unlike Dolphin's implementation we bounds-check every access and never
// delete or truncate the container on failure -- this runs against a real
// console's only copy of the data.
// ---------------------------------------------------------------------------

extern "C" {

DSTATUS disk_initialize(BYTE /*pdrv*/) {
    return g_buf ? 0 : STA_NOINIT;
}

// Must not report STA_NOINIT, or FatFs discards our hand-populated FATFS state
// and tries to parse a boot sector that a VFF does not have.
DSTATUS disk_status(BYTE /*pdrv*/) {
    return g_buf ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE /*pdrv*/, BYTE *buff, LBA_t sector, UINT count) {
    size_t offset, length;
    if (!SectorRange(static_cast<uint32_t>(sector), count, offset, length)) {
        return RES_PARERR;
    }
    std::memcpy(buff, g_buf->data() + offset, length);
    return RES_OK;
}

DRESULT disk_write(BYTE /*pdrv*/, const BYTE *buff, LBA_t sector, UINT count) {
    size_t offset, length;
    if (!SectorRange(static_cast<uint32_t>(sector), count, offset, length)) {
        return RES_PARERR;
    }
    std::memcpy(g_buf->data() + offset, buff, length);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE /*pdrv*/, BYTE cmd, void *buff) {
    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;
        case GET_SECTOR_COUNT:
            if (!g_buf) return RES_NOTRDY;
            *static_cast<LBA_t *>(buff) = g_buf->size() / kSectorSize;
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD *>(buff) = kSectorSize;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *static_cast<DWORD *>(buff) = 1;
            return RES_OK;
        default:
            return RES_OK;
    }
}

// FatFs timestamp for created/modified files. Fixed value: the vWii channels
// do not care about the timestamp, and a constant keeps writes reproducible.
// Format: bits 31-25 year-1980, 24-21 month, 20-16 day, 15-11 hour,
// 10-5 minute, 4-0 second/2.
DWORD get_fattime(void) {
    return (static_cast<DWORD>(2024 - 1980) << 25) | (1 << 21) | (1 << 16);
}

}  // extern "C"

namespace vff {

namespace {

// Hand-populate the FATFS state from the VFF header, replacing FatFs's normal
// boot-sector parsing. Port of Dolphin's read_vff_header().
bool PopulateFatFs(FATFS *fs, const std::vector<uint8_t> &buf) {
    if (buf.size() < 32) {
        LOG("vff: too small to hold a header (%zu bytes)", buf.size());
        return false;
    }
    if (std::memcmp(buf.data(), "VFF ", 4) != 0) {
        LOG("vff: bad magic");
        return false;
    }

    const uint16_t endianness   = ReadBE16(buf.data() + 4);
    const uint32_t volume_size  = ReadBE32(buf.data() + 8);
    const uint16_t cluster_field = ReadBE16(buf.data() + 12);

    if (endianness == kEndianLittle) {
        // Little-endian VFFs exist in theory; no known channel produces one,
        // and guessing at the layout risks corrupting real data.
        LOG("vff: little-endian container not supported");
        return false;
    }
    if (endianness != kEndianBig) {
        LOG("vff: unknown endianness marker 0x%04X", endianness);
        return false;
    }

    const uint32_t cluster_size = static_cast<uint32_t>(cluster_field) * 16;
    if (cluster_size == 0) {
        LOG("vff: zero cluster size");
        return false;
    }
    if (volume_size > buf.size()) {
        LOG("vff: header volume_size %u exceeds file size %zu", volume_size, buf.size());
        return false;
    }

    const uint32_t cluster_count = volume_size / cluster_size;

    uint32_t table_size;
    if (cluster_count < 4085) {
        fs->fs_type = FS_FAT12;
        table_size  = ((cluster_count + 1) / 2) * 3;
    } else if (cluster_count < 65525) {
        fs->fs_type = FS_FAT16;
        table_size  = cluster_count * 2;
    } else {
        LOG("vff: cluster count %u is neither FAT12 nor FAT16", cluster_count);
        return false;
    }
    table_size = AlignUp(table_size, cluster_size);
    fs->fsize  = table_size / kSectorSize;

    fs->n_fats = 2;
    fs->csize  = 1;
    // The root directory is 4096 bytes of 32-byte entries.
    fs->n_rootdir = 128;

    const uint32_t sysect = 1 + (fs->fsize * 2) + fs->n_rootdir / (kSectorSize / 32);
    if (sysect >= cluster_count) {
        LOG("vff: system area (%u) larger than volume (%u clusters)", sysect, cluster_count);
        return false;
    }

    fs->n_fatent = (cluster_count - sysect) + 2;
    fs->volbase  = 0;
    fs->fatbase  = 1;
    fs->database = sysect;
    fs->dirbase  = fs->fatbase + fs->fsize * 2;

    fs->last_clst = fs->free_clst = 0xFFFFFFFF;
    fs->fsi_flag  = 0x80;
    fs->id        = 0;
    fs->cdir      = 0;
    fs->wflag     = 0;
    fs->winsect   = static_cast<LBA_t>(-1);

    return true;
}

FATFS g_fatfs;

}  // namespace

Image::Image(std::vector<uint8_t> &buffer) {
    if (g_buf) {
        LOG("vff: another image is already mounted");
        return;
    }
    g_buf = &buffer;

    std::memset(&g_fatfs, 0, sizeof(g_fatfs));
    g_fatfs.fs_type = 0;
    g_fatfs.pdrv    = 0;

    // Register the (lazily mounted) volume, then overwrite its state with
    // values derived from the VFF header. FatFs skips its own mount as long as
    // fs_type is non-zero and disk_status() reports the drive is ready.
    if (f_mount(&g_fatfs, "", 0) != FR_OK) {
        LOG("vff: f_mount failed");
        g_buf = nullptr;
        return;
    }
    if (!PopulateFatFs(&g_fatfs, buffer)) {
        f_unmount("");
        g_buf = nullptr;
        return;
    }

    m_mounted = true;
}

Image::~Image() {
    if (m_mounted) {
        f_unmount("");
    }
    if (g_buf) {
        g_buf = nullptr;
    }
}

bool Image::List(std::vector<Entry> &out, const char *path) {
    out.clear();
    if (!m_mounted) return false;

    DIR dir{};
    if (f_opendir(&dir, path) != FR_OK) {
        return false;
    }

    for (;;) {
        FILINFO info{};
        if (f_readdir(&dir, &info) != FR_OK) {
            f_closedir(&dir);
            return false;
        }
        if (info.fname[0] == '\0') break;  // end of directory

        Entry e;
        e.name   = info.fname;
        e.size   = static_cast<uint32_t>(info.fsize);
        e.is_dir = (info.fattrib & AM_DIR) != 0;
        out.push_back(std::move(e));
    }

    f_closedir(&dir);
    return true;
}

bool Image::ReadFile(const char *name, std::vector<uint8_t> &out) {
    out.clear();
    if (!m_mounted) return false;

    FIL fp{};
    if (f_open(&fp, name, FA_READ) != FR_OK) {
        LOG("vff: cannot open '%s' for reading", name);
        return false;
    }

    out.resize(static_cast<size_t>(f_size(&fp)));
    UINT read = 0;
    if (!out.empty()) {
        if (f_read(&fp, out.data(), static_cast<UINT>(out.size()), &read) != FR_OK ||
            read != out.size()) {
            LOG("vff: short read on '%s' (%u of %zu)", name, read, out.size());
            f_close(&fp);
            out.clear();
            return false;
        }
    }

    f_close(&fp);
    return true;
}

bool Image::WriteFile(const char *name, const void *data, uint32_t size) {
    if (!m_mounted) return false;

    FIL fp{};
    if (f_open(&fp, name, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) {
        LOG("vff: cannot open '%s' for writing", name);
        return false;
    }

    const auto *p        = static_cast<const uint8_t *>(data);
    uint32_t    remaining = size;
    uint32_t    offset    = 0;
    while (remaining > 0) {
        constexpr uint32_t kChunk = 32768;
        const UINT to_write = static_cast<UINT>(remaining < kChunk ? remaining : kChunk);

        UINT written = 0;
        if (f_write(&fp, p + offset, to_write, &written) != FR_OK || written != to_write) {
            LOG("vff: write of '%s' failed at offset %u (%u of %u) -- volume full?",
                name, offset, written, to_write);
            f_close(&fp);
            return false;
        }
        remaining -= to_write;
        offset    += to_write;
    }

    if (f_close(&fp) != FR_OK) {
        LOG("vff: failed to close '%s'", name);
        return false;
    }
    return true;
}

bool Image::DeleteFile(const char *name) {
    if (!m_mounted) return false;

    const FRESULT res = f_unlink(name);
    if (res == FR_OK || res == FR_NO_FILE) {
        return true;
    }
    LOG("vff: failed to delete '%s' (FatFs error %d)", name, static_cast<int>(res));
    return false;
}

}  // namespace vff
