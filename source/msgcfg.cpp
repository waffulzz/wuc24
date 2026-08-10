#include "msgcfg.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"
#include "wc24.h"

namespace msgcfg {

namespace {

// On-disk layout. Big-endian, and so is Cafe OS, so it maps directly.
#pragma pack(push, 1)
struct ConfigData {
    uint32_t magic;    // 'WcCf'
    uint32_t version;  // 8
    uint64_t nwc24_id;
    uint32_t id_generation;
    uint32_t creation_stage;
    char     email[0x40];
    char     password[0x20];
    char     mlchkid[0x24];
    char     http_urls[URL_COUNT][0x80];
    uint8_t  reserved[0xDC];
    uint32_t enable_booting;
    uint32_t checksum;
};
#pragma pack(pop)

// Sum of the first 0xFF big-endian words, per Dolphin.
uint32_t CalculateChecksum(const ConfigData &data) {
    const auto *words = reinterpret_cast<const uint32_t *>(&data);
    uint32_t    sum   = 0;
    for (int i = 0; i < 0xFF; i++) sum += words[i];
    return sum;
}

std::string Bounded(const char *field, size_t max) {
    return std::string(field, strnlen(field, max));
}

}  // namespace

std::string Config::WiiAddress() const {
    if (email.empty()) return {};
    if (email[0] != '@') return email;  // already a full address

    char buf[80];
    std::snprintf(buf, sizeof(buf), "w%016llu%s", static_cast<unsigned long long>(nwc24_id),
                  email.c_str());
    return buf;
}

bool Read(VwiiNand &nand, Config &out) {
    out = Config{};

    std::vector<uint8_t> raw;
    if (!nand.ReadFile(wc24::kNwc24MsgCfg, raw)) {
        LOG("msgcfg: cannot read %s", wc24::kNwc24MsgCfg);
        return false;
    }
    if (raw.size() < sizeof(ConfigData)) {
        LOG("msgcfg: too small (%zu bytes, want %zu)", raw.size(), sizeof(ConfigData));
        return false;
    }

    ConfigData data;
    std::memcpy(&data, raw.data(), sizeof(data));

    if (data.magic != kMagic) {
        LOG("msgcfg: bad magic 0x%08X", data.magic);
        return false;
    }
    if (data.version != kVersion) {
        LOG("msgcfg: unexpected version %u (want %u)", data.version, kVersion);
    }

    const uint32_t expected = CalculateChecksum(data);
    out.checksum_ok = (expected == data.checksum);
    if (!out.checksum_ok) {
        LOG("msgcfg: checksum mismatch (stored 0x%08X, computed 0x%08X)", data.checksum, expected);
    }

    out.version        = data.version;
    out.nwc24_id       = data.nwc24_id;
    out.id_generation  = data.id_generation;
    out.creation_stage = data.creation_stage;
    out.email          = Bounded(data.email, sizeof(data.email));
    out.password       = Bounded(data.password, sizeof(data.password));
    out.mlchkid        = Bounded(data.mlchkid, sizeof(data.mlchkid));
    for (int i = 0; i < URL_COUNT; i++) {
        out.urls[i] = Bounded(data.http_urls[i], sizeof(data.http_urls[i]));
    }
    out.valid = true;
    return true;
}

void LogSummary(const Config &cfg) {
    if (!cfg.valid) {
        LOG("msgcfg: not loaded");
        return;
    }

    static const char *kStage[] = {"initial", "generated", "registered"};
    LOG("msgcfg: version=%u checksum=%s stage=%s", cfg.version, cfg.checksum_ok ? "ok" : "BAD",
        cfg.creation_stage < 3 ? kStage[cfg.creation_stage] : "?");
    LOG("  address: %s   (domain field: '%s')", cfg.WiiAddress().c_str(),
        cfg.email.c_str());
    LOG("  id: %llu (generation %u)", static_cast<unsigned long long>(cfg.nwc24_id),
        cfg.id_generation);
    // Credentials are intentionally not printed -- only whether they are set.
    LOG("  password: %s, mlchkid: %s", cfg.password.empty() ? "absent" : "present (not shown)",
        cfg.mlchkid.empty() ? "absent" : "present (not shown)");
    for (int i = 0; i < URL_COUNT; i++) {
        if (!cfg.urls[i].empty()) LOG("  url[%d]: %s", i, cfg.urls[i].c_str());
    }
}

}  // namespace msgcfg
