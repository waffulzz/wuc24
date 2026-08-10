// wc24.h — WiiConnect24 on-NAND file formats.
//
// These structs describe files stored in the vWii NAND under /shared2/wc24/.
// They are stored big-endian. Wii U Cafe OS (Espresso PPC) is also big-endian,
// so fields can be read/written directly with no byte-swapping — same as the
// original Wii tooling (NWC24-Manager) does.
//
// Layouts for nwc24dl.bin are derived from the Dolphin Emulator project
// (GPLv2-or-later) via noahpistilli/NWC24-Manager, cross-checked against
// https://wiibrew.org/wiki//dev/net/kd/request .
#pragma once

#include <cstdint>

namespace wc24 {

// vWii NAND paths (relative to the slccmpt mount point).
inline constexpr char kWc24Dir[]     = "/shared2/wc24";
inline constexpr char kNwc24DlBin[]  = "/shared2/wc24/nwc24dl.bin";
inline constexpr char kNwc24MsgCfg[] = "/shared2/wc24/nwc24msg.cfg";

// Mail. Each box is a pair: a fixed-size ".ctl" index, and a ".mbx" which is
// an ordinary VFF holding the message bodies as "mb/<prefix><id>.msg".
inline constexpr char kWc24RecvCtl[] = "/shared2/wc24/mbox/wc24recv.ctl";
inline constexpr char kWc24RecvMbx[] = "/shared2/wc24/mbox/wc24recv.mbx";
inline constexpr char kWc24SendCtl[] = "/shared2/wc24/mbox/wc24send.ctl";
inline constexpr char kWc24SendMbx[] = "/shared2/wc24/mbox/wc24send.mbx";

#pragma pack(push, 1)

// ---------------------------------------------------------------------------
// nwc24dl.bin — the download-task database. One header, then MAX_ENTRIES
// records, then MAX_ENTRIES entries. Total 63488 bytes.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kDlListMagic = 0x5763446C;  // 'WcDl'
inline constexpr uint32_t kMaxEntries  = 120;

enum EntryType : uint8_t {
    ENTRY_SUBTASK         = 1,
    ENTRY_MAIL            = 2,
    ENTRY_CHANNEL_CONTENT = 3,
    ENTRY_UNUSED          = 0xFF,
};

struct DLListHeader {
    uint32_t magic;    // 'WcDl'
    uint32_t version;  // must be 1
    uint32_t unk1;
    uint32_t unk2;
    uint16_t max_subentries;
    uint16_t reserved_mailnum;
    uint16_t max_entries;
    uint8_t  reserved[106];
};
static_assert(sizeof(DLListHeader) == 128, "DLListHeader size");

struct DLListRecord {
    uint32_t low_title_id;
    uint32_t next_dl_timestamp;
    uint32_t last_modified_timestamp;
    uint8_t  flags;
    uint8_t  padding[3];
};
static_assert(sizeof(DLListRecord) == 16, "DLListRecord size");

struct DLListEntry {
    uint16_t index;
    uint8_t  type;          // EntryType
    uint8_t  record_flags;
    uint32_t flags;
    char     high_title_id[4];
    uint32_t low_title_id;
    uint32_t unknown1;
    uint16_t group_id;
    uint16_t padding1;
    uint16_t remaining_downloads;
    uint16_t error_count;
    uint16_t dl_margin;
    uint16_t retry_frequency;
    int32_t  error_code;
    uint8_t  subtask_id;
    uint8_t  subtask_type;
    uint16_t subtask_flags;
    uint32_t subtask_bitmask;
    uint32_t server_dl_interval;
    uint32_t dl_timestamp;   // last DL time
    uint32_t subtask_timestamps[32];
    char     dl_url[236];    // full download URL for this task
    char     filename[64];   // where KD stores the (decoded) result
    uint8_t  unk6[29];
    uint8_t  should_use_rootca;
    uint16_t unknown3;
};
static_assert(sizeof(DLListEntry) == 512, "DLListEntry size");

struct DLList {
    DLListHeader header;
    DLListRecord records[kMaxEntries];
    DLListEntry  entries[kMaxEntries];
};
static_assert(sizeof(DLList) == 63488, "DLList size");

// entry.flags bit meanings, confirmed against Dolphin's NWC24Dl::IsEncrypted /
// IsRSASigned / SkipSchedulerDownload (GPLv2-or-later). Bit 0 = LSB; no
// byte-swap needed since both the on-NAND format and Cafe OS are big-endian.
inline bool EntryIsRsaSigned(uint32_t flags)         { return !((flags >> 2) & 1); }
inline bool EntryIsEncrypted(uint32_t flags)         { return  (flags >> 3) & 1; }
inline bool EntrySkipsScheduler(uint32_t flags)      { return  (flags >> 5) & 1; }

// ---------------------------------------------------------------------------
// WC24File — the 320-byte wrapper prefixed to a downloaded task's HTTP
// response body when EntryIsRsaSigned() is true. Confirmed against
// Dolphin's IOS::HLE::NWC24::WC24File / NetKDRequest.cpp download handler:
// the real payload always starts at exactly offset 320 in the HTTP response,
// regardless of these header field values (they are not validated before use
// there, so we don't validate them either -- just skip past them).
//
// If EntryIsEncrypted() is also true, that offset-320 payload is further
// AES-128-OFB encrypted using a per-title key from wc24pubk.mod and the `iv`
// field from this header.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kWc24FileHeaderSize = 320;

struct WC24File {
    char     magic[4];
    uint32_t version;
    uint32_t filler;
    uint8_t  crypt_type;
    uint8_t  padding[3];
    uint8_t  reserved[32];
    uint8_t  iv[16];
    uint8_t  rsa_signature[256];
};
static_assert(sizeof(WC24File) == kWc24FileHeaderSize, "WC24File size");

// Per-title key material for EntryIsEncrypted() content, read from
// /title/<low_title_id>/<high_title_id>/data/wc24pubk.mod.
struct WC24PubkMod {
    uint8_t rsa_public[256];
    uint8_t rsa_reserved[256];
    uint8_t aes_key[16];
    uint8_t aes_reserved[16];
};
static_assert(sizeof(WC24PubkMod) == 544, "WC24PubkMod size");

// ---------------------------------------------------------------------------
// Mail list (wc24recv.ctl / wc24send.ctl) — a 128-byte header followed by 127
// fixed 128-byte entries, 16384 bytes in total. Layouts from Dolphin's
// IOS::HLE::NWC24::Mail (GPLv2-or-later); note Dolphin implements the *send*
// side only and explicitly does not save received mail, so the receive path is
// not covered by a working reference.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kMailListMagic = 0x57635466;  // 'WcTf'

// The two boxes are different sizes -- confirmed on hardware: the send list is
// 16384 bytes (127 entries) as in Dolphin, but the receive list is twice that,
// 32768 bytes (255 entries). Always derive the entry count from the file size
// rather than assuming either.
inline constexpr uint32_t kMailSendListSize   = 16384;
inline constexpr uint32_t kMailSendMaxEntries = 127;
inline constexpr uint32_t kMailRecvListSize   = 32768;
inline constexpr uint32_t kMailRecvMaxEntries = 255;

struct MailListHeader {
    uint32_t magic;    // 'WcTf'
    uint32_t version;  // 4 on Wii Menu 4.x
    uint32_t number_of_mail;
    uint32_t total_entries;
    uint32_t total_size_of_messages;
    uint32_t filesize;
    uint32_t next_entry_id;
    uint32_t next_entry_offset;
    uint32_t unk1;
    uint32_t vff_free_space;
    uint8_t  unk2[48];
    // In the SEND list this is compared against a flag the server reports;
    // a mismatch is how the console learns there is mail waiting. The receive
    // list carries the field but never uses it (WiiLink24/kaitais).
    char     mail_flag[40];
};
static_assert(sizeof(MailListHeader) == 128, "MailListHeader size");

struct MailMultipartEntry {
    uint32_t offset;
    uint32_t size;
};

// This is the RECEIVE entry. The send entry is the same size and mostly the
// same, but has app_group as a u4 (no always_1 / multipart count) and 32 bytes
// of padding where this one keeps its multipart tables (WiiLink24/kaitais).
struct MailListEntry {
    uint32_t id;
    uint32_t flag;
    uint32_t msg_size;
    uint32_t app_id;  // 4 ASCII chars rather than a number
    uint32_t header_length;
    uint32_t tag;
    uint32_t wii_cmd;
    uint32_t crc32;  // never validated by IOS
    uint64_t from_friend_code;
    uint32_t minutes_since_1900;
    uint32_t padding;
    uint8_t  always_1;
    uint8_t  number_of_multipart_entries;
    uint16_t app_group;
    // The "packed" fields encode where the corresponding header sits inside
    // the stored message; the exact packing is what the dump is for.
    // See PackField() -- (length << 20) | offset, pointing into the stored
    // message. Undocumented everywhere; derived from a captured entry.
    uint32_t packed_from;
    uint32_t packed_to;
    uint32_t packed_subject;
    uint32_t packed_charset;
    uint32_t packed_transfer_encoding;
    uint32_t message_offset;
    uint32_t encoded_length;  // == message_length unless base64
    MailMultipartEntry multipart_entries[2];
    uint32_t multipart_sizes[2];
    uint32_t multipart_content_types[2];
    uint32_t message_length;
    uint32_t dwc_id;
    uint32_t always_0x80000000;
    uint32_t padding3;
};
static_assert(sizeof(MailListEntry) == 128, "MailListEntry size");

// Number of entries a mail list of `file_size` bytes holds.
inline uint32_t MailEntryCount(size_t file_size) {
    if (file_size < sizeof(MailListHeader) * 2) return 0;
    return static_cast<uint32_t>((file_size - sizeof(MailListHeader)) / sizeof(MailListEntry));
}

// ---------------------------------------------------------------------------
// The packed_* fields
//
// Each one locates a header value inside the stored message as
//
//     (length << 20) | offset
//
// i.e. a 20-bit byte offset from the start of the message with the value's
// length above it. Neither Dolphin nor WiiLink24/kaitais documents this; it was
// derived by decoding a queued outbound entry and checking every field against
// the message it pointed into -- each one landed on exactly the right bytes,
// e.g. packed_subject 0x00B000ED = offset 237, length 11, which is precisely
// the "Subject:" value in that message.
//
// Quirk worth reproducing: from/to/subject lengths cover the value alone, but
// charset and transfer_encoding include the trailing CR of their line.
// ---------------------------------------------------------------------------

inline constexpr uint32_t kPackedOffsetMask = 0x000FFFFF;

inline uint32_t PackField(uint32_t offset, uint32_t length) {
    return (length << 20) | (offset & kPackedOffsetMask);
}
inline uint32_t PackedOffset(uint32_t packed) { return packed & kPackedOffsetMask; }
inline uint32_t PackedLength(uint32_t packed) { return packed >> 20; }

// ---------------------------------------------------------------------------
// VFF (Virtual FAT File) container header — 32 bytes, big-endian.
// Layout confirmed against Dolphin's IOS::HLE::NWC24::VFFHeader /
// VFFUtil.cpp (GPLv2-or-later), which reimplements this against real
// hardware behaviour. The body is a FAT12/16 volume addressed through this
// header rather than a standard FAT boot sector -- FatFs's normal auto-mount
// does not apply; the FATFS state must be hand-populated (see read_vff_header
// in Dolphin's VFFUtil.cpp) and every sector access is offset by -480 bytes
// (sector 0 is invalid; "sector 1" starts right after this header, i.e. at
// byte 32, not byte 512).
// ---------------------------------------------------------------------------

inline constexpr uint16_t kVffEndianBig    = 0xFEFF;
inline constexpr uint16_t kVffEndianLittle = 0xFFFE;
inline constexpr uint16_t kSectorSize      = 512;

struct VffHeader {
    uint8_t  magic[4];
    uint16_t endianness;      // kVffEndianBig on real hardware
    uint16_t unknown_marker;
    uint32_t volume_size;     // total file size in bytes
    uint16_t cluster_size;    // actual cluster size = this * 16
    uint16_t empty;
    uint16_t unknown;
    uint8_t  padding[14];
};
static_assert(sizeof(VffHeader) == 32, "VffHeader size");

#pragma pack(pop)

}  // namespace wc24
