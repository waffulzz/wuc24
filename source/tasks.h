// tasks.h — read and summarise the vWii's WiiConnect24 download tasks.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nand.h"

struct DlTaskInfo {
    uint16_t    index;
    uint8_t     type;                 // wc24::EntryType
    std::string high_title_id;        // 4-char ascii, e.g. "HAFE" (Forecast)
    uint32_t    high_title_id_raw;     // same 4 bytes, as a big-endian u32
    uint32_t    low_title_id;
    uint16_t    remaining_downloads;
    std::string url;
    std::string filename;             // where KD stores the decoded result
    uint32_t    flags;
    bool        rsa_signed;           // wc24::EntryIsRsaSigned(flags)
    bool        encrypted;            // wc24::EntryIsEncrypted(flags)
    std::string vff_path;             // /title/<low>/<high>/data/wc24dl.vff
    std::string pubk_path;            // /title/<low>/<high>/data/wc24pubk.mod

    // Subtask fan-out: one entry can stand for several numbered downloads
    // (news.bin.00, .01, ...). Logged so the real semantics can be read off a
    // live console rather than guessed at.
    uint8_t     subtask_id;
    uint8_t     subtask_type;
    uint16_t    subtask_flags;
    uint32_t    subtask_bitmask;
    uint16_t    group_id;
};

// Read /shared2/wc24/nwc24dl.bin from the mounted vWii NAND, validate it, and
// return one entry per active (non-UNUSED) task. Also logs a summary.
// Returns false if the file can't be read or is malformed.
bool ScanDlTasks(VwiiNand &nand, std::vector<DlTaskInfo> &out);

const char *EntryTypeName(uint8_t type);
