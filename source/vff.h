// vff.h — read/write files inside a WiiConnect24 VFF container.
//
// A VFF ("virtual FAT file") is a 32-byte big-endian header followed by a
// FAT12/16 volume. It is NOT a normal FAT image: there is no boot sector, so
// FatFs cannot auto-mount it. Instead we hand-populate the FATFS state from
// the VFF header (see Mount()) and address sectors with a fixed -480 byte
// offset, so FatFs "sector 1" lands at file byte 32, immediately after the
// header. This mirrors Dolphin's IOS::HLE::NWC24 VFFUtil (GPLv2-or-later),
// which is validated against real hardware behaviour.
//
// The whole container is operated on as one in-memory buffer: read the VFF
// off NAND, construct an Image over that buffer, do the file operations, then
// write the buffer back in a single pass. That keeps NAND I/O to two bulk
// operations and makes backup/rollback trivial (the caller still holds the
// original bytes).
//
// FatFs keeps global state, so only ONE Image may exist at a time.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vff {

class Image {
public:
    // `buffer` is the entire VFF file. It is borrowed, not copied: writes
    // modify it in place. It must outlive the Image.
    explicit Image(std::vector<uint8_t> &buffer);
    ~Image();

    Image(const Image &)            = delete;
    Image &operator=(const Image &) = delete;

    // True if the header parsed and the volume mounted.
    bool ok() const { return m_mounted; }

    struct Entry {
        std::string name;
        uint32_t    size;
        bool        is_dir;
    };

    // List a directory inside the container ("/" for the root). WC24 keeps
    // message files in an "mb" subdirectory, so listing only the root hides
    // everything that matters.
    bool List(std::vector<Entry> &out, const char *dir = "/");

    // Read a whole file out of the container.
    bool ReadFile(const char *name, std::vector<uint8_t> &out);

    // Create/overwrite a file in the container. The volume is fixed-size, so
    // this fails (leaving the buffer untouched) if there isn't room.
    bool WriteFile(const char *name, const void *data, uint32_t size);

    // Remove a file from the container. Succeeds if it was already absent.
    bool DeleteFile(const char *name);

private:
    bool m_mounted = false;
};

}  // namespace vff
