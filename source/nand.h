// nand.h — access the vWii NAND (slccmpt) from Wii U (Cafe OS) mode.
//
// The vWii's WiiConnect24 files live in the SLCCMPT partition of the console's
// internal NAND, which Cafe OS does not normally mount. We use libmocha to
// mount /dev/slccmpt01 into the FSA namespace, unlock an FSA client so it may
// touch it, and then do ordinary FSA file I/O.
//
// USAGE: construct a VwiiNand; check ok(). Files are addressed by their vWii
// path (e.g. "/shared2/wc24/nwc24dl.bin") — the mount prefix is added for you.
// The destructor unmounts and tears everything down.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <coreinit/filesystem_fsa.h>

class VwiiNand {
public:
    VwiiNand();
    ~VwiiNand();

    VwiiNand(const VwiiNand &)            = delete;
    VwiiNand &operator=(const VwiiNand &) = delete;

    // True once mocha is up, slccmpt is mounted, and the FSA client is unlocked.
    bool ok() const { return m_ready; }

    // Read an entire vWii file into `out`. Returns false on any error.
    //
    // Only for files small enough to sit in a plugin's heap. A 20 MB file will
    // throw std::bad_alloc, which takes the console down -- use CopyFileTo()
    // for anything large.
    bool ReadFile(const char *vwii_path, std::vector<uint8_t> &out);

    // Copies a vWii file to an open stdio stream in fixed-size chunks, so
    // arbitrarily large files cost only the chunk buffer.
    bool CopyFileTo(const char *vwii_path, FILE *dest);

    // Overwrite a vWii file with `data`. Returns false on any error.
    // (Not used yet — writes come once the format work is validated.)
    bool WriteFile(const char *vwii_path, const void *data, uint32_t size);

    // Size of a vWii file, or -1 if it can't be stat'd.
    int64_t FileSize(const char *vwii_path);

    struct DirEntry {
        std::string name;
        uint32_t    size;
        bool        is_dir;
        bool        encrypted;  // FS_STAT_ENCRYPTED_FILE -- cannot be read normally
        uint32_t    flags;
    };

    // Lists a directory on the vWii NAND.
    bool ListDir(const char *vwii_path, std::vector<DirEntry> &out);

private:
    std::string FullPath(const char *vwii_path) const;

    bool            m_mochaInit  = false;
    bool            m_mounted    = false;
    bool            m_fsaAdded   = false;
    bool            m_ready      = false;
    FSAClientHandle m_client     = 0;
};
