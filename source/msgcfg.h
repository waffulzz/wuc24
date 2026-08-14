// Portions derived from the Dolphin Emulator Project (GPL-2.0-or-later):
// the nwc24msg.cfg layout.
// msgcfg.h — /shared2/wc24/nwc24msg.cfg, the console's mail identity.
//
// Holds the console's WiiConnect24 id (its "Wii number"), its own mail
// address, the credentials KD uses against the mail server, and the service
// URLs. Layout from Dolphin's IOS::HLE::NWC24::NWC24Config (GPLv2-or-later).
//
// The password and mlchkid are secrets: they are parsed because the mail
// protocol needs them, and are deliberately never logged.
#pragma once

#include <cstdint>
#include <string>

#include "nand.h"

namespace msgcfg {

inline constexpr uint32_t kMagic   = 0x57634366;  // 'WcCf'
inline constexpr uint32_t kVersion = 8;

// http_urls[] slots, read off a real console rather than inferred -- the
// endpoint names come back in this order, which is NOT the order Dolphin's
// accessors imply (it has send at 0 and account at 4).
enum UrlIndex {
    URL_ACCOUNT = 0,
    URL_CHECK   = 1,
    URL_RECEIVE = 2,
    URL_DELETE  = 3,
    URL_SEND    = 4,
    URL_COUNT   = 5,
};

struct Config {
    bool     valid            = false;
    bool     checksum_ok      = false;
    uint32_t version          = 0;
    uint64_t nwc24_id         = 0;  // the console's Wii number
    uint32_t id_generation    = 0;
    uint32_t creation_stage   = 0;  // 0 initial, 1 generated, 2 registered
    std::string email;              // the console's own mail address
    std::string urls[URL_COUNT];

    // Secrets. Never log these.
    std::string password;
    std::string mlchkid;

    bool registered() const { return creation_stage == 2; }

    // The console's full mail address.
    //
    // `email` holds only the domain part ("@rc24.xyz"); the local part is "w"
    // followed by the id padded to 16 digits, which is how the address appears
    // in messages the console itself sends. If the field ever does contain a
    // full address, it is returned unchanged.
    std::string WiiAddress() const;
};

// Reads and validates the config off the vWii NAND.
bool Read(VwiiNand &nand, Config &out);

// Logs the non-secret parts (id, address, URLs, registration state).
void LogSummary(const Config &cfg);

}  // namespace msgcfg
