#include "wc24dec.h"

#include <cstring>

#include "aes.h"
#include "log.h"
#include "wc24.h"

namespace wc24dec {

const char *ResultName(Result r) {
    switch (r) {
        case Result::Ok:       return "ok";
        case Result::TooSmall: return "response smaller than the WC24 header";
        case Result::Empty:    return "no payload after stripping the header";
        case Result::MissingKey:
            return "encrypted, but the title's wc24pubk.mod key is unavailable";
        case Result::NotSignedButEncrypted:
            return "encrypted but unsigned -- no header to take the IV from";
        default: return "?";
    }
}

Result Decode(const std::vector<uint8_t> &raw, bool rsa_signed, bool encrypted,
              const uint8_t *aes_key, std::vector<uint8_t> &out) {
    out.clear();

    if (!rsa_signed) {
        // No wrapper at all; the body is the payload. There is nowhere to take
        // an IV from, so encrypted content in this form is not something we
        // can decode (and has not been observed).
        if (encrypted) return Result::NotSignedButEncrypted;
        if (raw.empty()) return Result::Empty;
        out = raw;
        return Result::Ok;
    }

    if (raw.size() <= wc24::kWc24FileHeaderSize) {
        return Result::TooSmall;
    }
    if (encrypted && !aes_key) {
        return Result::MissingKey;
    }

    out.assign(raw.begin() + wc24::kWc24FileHeaderSize, raw.end());
    if (out.empty()) return Result::Empty;

    if (encrypted) {
        // The IV travels in the header we just stripped.
        wc24::WC24File header;
        std::memcpy(&header, raw.data(), sizeof(header));
        aes::CryptOfb(aes_key, header.iv, out.data(), out.data(), out.size());
    }

    return Result::Ok;
}

}  // namespace wc24dec
