#include "mail.h"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "log.h"
#include "nand.h"
#include "vff.h"
#include "wc24.h"

namespace mail {

bool Deliver(VwiiNand &nand, const Message &msg, bool commit) {
    // --- read the index -----------------------------------------------------
    std::vector<uint8_t> ctl;
    if (!nand.ReadFile(wc24::kWc24RecvCtl, ctl)) {
        LOG("mail: cannot read the inbox index");
        return false;
    }
    const uint32_t capacity = wc24::MailEntryCount(ctl.size());
    if (capacity == 0) {
        LOG("mail: inbox index is too small (%zu bytes)", ctl.size());
        return false;
    }

    wc24::MailListHeader header;
    std::memcpy(&header, ctl.data(), sizeof(header));
    if (header.magic != wc24::kMailListMagic) {
        LOG("mail: bad inbox magic 0x%08X", header.magic);
        return false;
    }

    // Where the console says the next entry goes. Offsets are from the start
    // of the file, with the 128-byte header first.
    uint32_t slot = 0;
    if (header.next_entry_offset >= sizeof(wc24::MailListHeader)) {
        slot = (header.next_entry_offset - sizeof(wc24::MailListHeader)) /
               sizeof(wc24::MailListEntry);
    }
    if (slot >= capacity) {
        LOG("mail: next entry offset %u is out of range", header.next_entry_offset);
        return false;
    }

    auto *entries = reinterpret_cast<wc24::MailListEntry *>(ctl.data() +
                                                            sizeof(wc24::MailListHeader));
    if (entries[slot].id != 0) {
        LOG("mail: slot %u is already occupied (id %u)", slot, entries[slot].id);
        return false;
    }

    const uint32_t id = header.next_entry_id;

    // The entry timestamps mail in minutes since 1900, which is also what the
    // Date: header is rendered from. 2208988800 is the gap from 1900 to the
    // Unix epoch.
    const uint32_t minutes_since_1900 =
        static_cast<uint32_t>((static_cast<uint64_t>(std::time(nullptr)) + 2208988800ULL) / 60);

    // --- build the message --------------------------------------------------
    FieldSpans        spans;
    const std::string text = BuildMessage(msg, id, 0, minutes_since_1900, spans);
    LOG("mail: built a %zu byte message, body at %u (%u bytes)", text.size(), spans.body_offset,
        spans.body_length);

    // --- store it in the mailbox VFF ---------------------------------------
    std::vector<uint8_t> mbx;
    if (!nand.ReadFile(wc24::kWc24RecvMbx, mbx)) {
        LOG("mail: cannot read the inbox mailbox");
        return false;
    }
    const size_t mbx_size = mbx.size();

    // Every message the console stores is padded with NULs to a 32-byte
    // boundary while msg_size keeps the true length -- 353 bytes of message in
    // a 384-byte file, and likewise for the received ones (992, 320, 960). An
    // unpadded file is the one way our entry differed from every real sample.
    std::string stored = text;
    stored.resize((text.size() + 31) & ~static_cast<size_t>(31), '\0');

    char msg_path[64];
    std::snprintf(msg_path, sizeof(msg_path), "mb/r%07u.msg", id);
    {
        vff::Image image(mbx);
        if (!image.ok()) {
            LOG("mail: cannot mount the inbox mailbox");
            return false;
        }
        if (!image.WriteFile(msg_path, stored.data(), static_cast<uint32_t>(stored.size()))) {
            LOG("mail: cannot write %s into the mailbox", msg_path);
            return false;
        }
    }
    LOG("mail: %zu bytes of message stored in a %zu byte file (32-byte aligned)", text.size(),
        stored.size());
    if (mbx.size() != mbx_size) {
        LOG("mail: mailbox changed size, discarding");
        return false;
    }
    LOG("mail: stored as %s", msg_path);

    // --- fill in the index entry -------------------------------------------
    //
    // Every constant here is copied from a genuine received entry captured off
    // the console, not inferred. Notably app_id is zero (not the title code),
    // wii_cmd is set even though the message carries no X-Wii-Cmd header,
    // minutes_since_1900 is left at zero, and header_length points at the
    // start of the MIME body rather than at the text.
    wc24::MailListEntry entry{};
    entry.id            = id;
    entry.flag          = 0x00210222;
    entry.msg_size      = static_cast<uint32_t>(text.size());
    entry.app_id        = 0;
    entry.header_length = spans.mime_offset;
    entry.tag           = 0;
    entry.wii_cmd       = 0x00044001;
    entry.crc32         = 0;  // never validated
    entry.from_friend_code            = 0;
    entry.minutes_since_1900          = 0;
    entry.always_1                    = 1;
    entry.number_of_multipart_entries = 0;
    entry.app_group                   = 0;
    entry.packed_from    = wc24::PackField(spans.from_offset, spans.from_length);
    entry.packed_to      = wc24::PackField(spans.to_offset, spans.to_length);
    entry.packed_subject = wc24::PackField(spans.subject_offset, spans.subject_length);
    entry.packed_charset = wc24::PackField(spans.charset_offset, spans.charset_length);
    entry.packed_transfer_encoding = 0;  // absent from received mail
    entry.message_offset    = spans.body_offset;
    entry.encoded_length    = spans.body_length;
    entry.message_length    = spans.body_length;
    entry.dwc_id            = 0;
    entry.always_0x80000000 = 0x80000000;

    entries[slot] = entry;

    // --- update the header --------------------------------------------------
    header.number_of_mail         = header.number_of_mail + 1;
    header.total_size_of_messages = header.total_size_of_messages +
                                    static_cast<uint32_t>(text.size());
    header.next_entry_id          = id + 1;
    header.next_entry_offset =
        static_cast<uint32_t>(sizeof(wc24::MailListHeader) +
                              (slot + 1) * sizeof(wc24::MailListEntry));
    std::memcpy(ctl.data(), &header, sizeof(header));

    LOG("mail: slot %u, id %u, %u mail in the box", slot, id, header.number_of_mail);

    if (!commit) {
        LOG("mail: dry run -- NAND not modified");
        return true;
    }

    // --- write both files back ---------------------------------------------
    if (!nand.WriteFile(wc24::kWc24RecvMbx, mbx.data(), static_cast<uint32_t>(mbx.size()))) {
        LOG("mail: FAILED writing the mailbox -- inbox index left untouched");
        return false;
    }
    if (!nand.WriteFile(wc24::kWc24RecvCtl, ctl.data(), static_cast<uint32_t>(ctl.size()))) {
        LOG("mail: FAILED writing the index; the mailbox now holds an orphaned message");
        LOG("mail: restore both files from the SD backups");
        return false;
    }

    // --- read it straight back --------------------------------------------
    //
    // A write returning success is not proof it landed: these files may be
    // cached or rewritten by the console. Without this, "delivered" and
    // "silently discarded" look identical afterwards.
    std::vector<uint8_t> check;
    if (!nand.ReadFile(wc24::kWc24RecvCtl, check) || check.size() != ctl.size()) {
        LOG("mail: cannot read the index back to verify");
        return false;
    }

    wc24::MailListHeader written;
    std::memcpy(&written, check.data(), sizeof(written));
    const auto *written_entries =
        reinterpret_cast<const wc24::MailListEntry *>(check.data() + sizeof(written));

    const bool header_ok = written.number_of_mail == header.number_of_mail &&
                           written.next_entry_id == header.next_entry_id;
    const bool entry_ok  = written_entries[slot].id == id;

    LOG("mail: readback -- number_of_mail=%u next_id=%u slot%u.id=%u  [%s]",
        written.number_of_mail, written.next_entry_id, slot, written_entries[slot].id,
        (header_ok && entry_ok) ? "PERSISTED" : "NOT PERSISTED");

    if (!header_ok || !entry_ok) {
        LOG("mail: the index did not keep our changes -- the console rejected or reverted them");
        return false;
    }

    // And confirm the body is really in the mailbox.
    std::vector<uint8_t> check_mbx;
    if (nand.ReadFile(wc24::kWc24RecvMbx, check_mbx)) {
        vff::Image image(check_mbx);
        std::vector<vff::Image::Entry> listing;
        if (image.ok() && image.List(listing, "mb")) {
            LOG("mail: mailbox mb/ now holds %zu file(s)", listing.size());
            for (const auto &e : listing) {
                LOG("    %-16s %8u", e.name.c_str(), e.size);
            }
        }
    }

    LOG("mail: delivered");
    return true;
}

}  // namespace mail
