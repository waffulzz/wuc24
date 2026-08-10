// wc24dec.h — turn a raw WC24 HTTP response into the bytes that belong in a
// channel's VFF.
//
// A downloaded task's body is:
//   [320-byte WC24File header][payload]      when the entry is RSA-signed
//   [payload]                                otherwise
//
// and when the entry is additionally flagged encrypted, that payload is
// AES-128-OFB encrypted with a per-title key from the title's wc24pubk.mod
// plus the IV carried in the header.
//
// We deliberately do NOT verify the RSA signature: the signature covers the
// original Nintendo content, and WiiLink re-serves its own data. The real KD
// would reject it, which is why WiiLink's setup patches the check out. There
// is nothing for us to usefully verify against.
#pragma once

#include <cstdint>
#include <vector>

namespace wc24dec {

enum class Result {
    Ok,
    TooSmall,        // response can't even hold the header
    Empty,           // nothing left after stripping
    MissingKey,      // encrypted, but no AES key was supplied
    NotSignedButEncrypted,  // encrypted content without a header to take the IV from
};

const char *ResultName(Result r);

// Strips the WC24 wrapper from `raw`, producing the bytes to store in the VFF.
// `rsa_signed`/`encrypted` come from the task's entry flags.
//
// `aes_key` is the 16-byte key from the title's wc24pubk.mod, required when
// `encrypted` is set and ignored otherwise; pass nullptr when there is none.
// The IV comes from the header that is being stripped.
Result Decode(const std::vector<uint8_t> &raw, bool rsa_signed, bool encrypted,
              const uint8_t *aes_key, std::vector<uint8_t> &out);

}  // namespace wc24dec
