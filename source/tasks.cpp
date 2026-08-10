#include "tasks.h"

#include <cstdio>
#include <cstring>

#include "log.h"
#include "wc24.h"

const char *EntryTypeName(uint8_t type) {
    switch (type) {
        case wc24::ENTRY_SUBTASK:         return "subtask";
        case wc24::ENTRY_MAIL:            return "mail";
        case wc24::ENTRY_CHANNEL_CONTENT: return "channel";
        default:                          return "?";
    }
}

bool ScanDlTasks(VwiiNand &nand, std::vector<DlTaskInfo> &out) {
    out.clear();

    std::vector<uint8_t> raw;
    if (!nand.ReadFile(wc24::kNwc24DlBin, raw)) {
        LOG("could not read %s", wc24::kNwc24DlBin);
        return false;
    }
    if (raw.size() != sizeof(wc24::DLList)) {
        LOG("nwc24dl.bin unexpected size %zu (want %zu)", raw.size(), sizeof(wc24::DLList));
        return false;
    }

    // Cafe OS is big-endian, same as the on-NAND format, so read directly.
    const auto *list = reinterpret_cast<const wc24::DLList *>(raw.data());
    if (list->header.magic != wc24::kDlListMagic) {
        LOG("bad nwc24dl.bin magic 0x%08X", list->header.magic);
        return false;
    }

    for (const auto &e : list->entries) {
        if (e.type == wc24::ENTRY_UNUSED) continue;

        DlTaskInfo info{};
        info.index               = e.index;
        info.type                = e.type;
        info.high_title_id.assign(e.high_title_id, 4);
        std::memcpy(&info.high_title_id_raw, e.high_title_id, 4);
        info.low_title_id        = e.low_title_id;
        info.remaining_downloads = e.remaining_downloads;
        info.url.assign(e.dl_url, strnlen(e.dl_url, sizeof(e.dl_url)));
        info.filename.assign(e.filename, strnlen(e.filename, sizeof(e.filename)));
        info.flags       = e.flags;
        info.rsa_signed   = wc24::EntryIsRsaSigned(e.flags);
        info.encrypted    = wc24::EntryIsEncrypted(e.flags);

        info.subtask_id      = e.subtask_id;
        info.subtask_type    = e.subtask_type;
        info.subtask_flags   = e.subtask_flags;
        info.subtask_bitmask = e.subtask_bitmask;
        info.group_id        = e.group_id;

        char path[128];
        std::snprintf(path, sizeof(path), "/title/%08x/%08x/data/wc24dl.vff",
                      info.low_title_id, info.high_title_id_raw);
        info.vff_path = path;

        // Holds the per-title AES key used for encrypted content.
        std::snprintf(path, sizeof(path), "/title/%08x/%08x/data/wc24pubk.mod",
                      info.low_title_id, info.high_title_id_raw);
        info.pubk_path = path;

        out.push_back(std::move(info));
    }

    LOG("found %zu active WC24 download task(s):", out.size());
    for (const auto &t : out) {
        LOG("  [%3u] %-7s %.4s/%08X  left=%u  sig=%d enc=%d  file='%s'  %s",
            t.index, EntryTypeName(t.type), t.high_title_id.c_str(),
            t.low_title_id, t.remaining_downloads, t.rsa_signed, t.encrypted,
            t.filename.c_str(), t.url.c_str());
        LOG("         vff=%s", t.vff_path.c_str());
        LOG("         flags=0x%08X group=%u sub_id=%u sub_type=%u sub_flags=0x%04X "
            "sub_bitmask=0x%08X",
            t.flags, t.group_id, t.subtask_id, t.subtask_type, t.subtask_flags,
            t.subtask_bitmask);
    }
    return true;
}
