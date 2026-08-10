// mail.h — put a message into the vWii's WiiConnect24 inbox.
//
// The inbox is a pair of files under /shared2/wc24/mbox/:
//   wc24recv.mbx  a VFF holding message bodies as "mb/r<id:07d>.msg"
//   wc24recv.ctl  a fixed 32768-byte index: header + 255 fixed entries
//
// Writing a message means storing its text in the VFF and adding an entry that
// points at the header values inside that text (see wc24::PackField). The Wii
// Menu then imports it onto the message board the next time it runs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

class VwiiNand;

namespace mail {

// What to deliver. Kept deliberately simple: a plain us-ascii text message,
// which is what the message board displays for ordinary mail.
struct Message {
    std::string from;      // e.g. "w0000000000000000@rc24.xyz"
    std::string to;        // normally the console's own address
    std::string subject;
    std::string body;
    std::string alt_name;  // display name; becomes X-Wii-AltName. Optional.
};

// Where each header value sits inside the built message, for the index entry.
struct FieldSpans {
    uint32_t from_offset = 0, from_length = 0;
    uint32_t to_offset = 0, to_length = 0;
    uint32_t subject_offset = 0, subject_length = 0;
    uint32_t charset_offset = 0, charset_length = 0;
    uint32_t mime_offset = 0;  // first boundary line; the entry's header_len
    uint32_t body_offset = 0, body_length = 0;
};

// Renders minutes-since-1900 as an RFC822 date (exposed for testing).
std::string FormatDate(uint32_t minutes_since_1900);

// Builds the RFC822 text the console stores, recording where each value lands.
// `id` and `friend_code` only affect the Message-Id, which is cosmetic.
std::string BuildMessage(const Message &msg, uint32_t id, uint64_t friend_code,
                         uint32_t minutes_since_1900, FieldSpans &spans);

// Reads the inbox, appends `msg`, and writes it back. Backs both files up to
// SD first via the caller-supplied callback, and leaves NAND untouched unless
// every step succeeds.
//
// `commit` false does everything except the final NAND write.
bool Deliver(VwiiNand &nand, const Message &msg, bool commit);

}  // namespace mail
