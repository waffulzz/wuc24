// aes.h — AES-128 in OFB mode, which is what WC24 uses for encrypted content.
//
// OFB builds a keystream by repeatedly encrypting the IV and XORs it with the
// data, so only the *encryption* direction of the block cipher is needed and
// encrypting and decrypting are the same operation.
//
// Pure and dependency-free so it can be checked against the published FIPS-197
// and NIST SP 800-38A vectors on a host -- see tools/aes_test.cpp. Getting
// this subtly wrong would silently produce garbage that we would then write
// into a channel's data.
#pragma once

#include <cstddef>
#include <cstdint>

namespace aes {

inline constexpr size_t kBlockSize = 16;
inline constexpr size_t kKeySize   = 16;

// Encrypts one 16-byte block. Exposed mainly so the test can use the FIPS-197
// known-answer vectors directly.
void EncryptBlock(const uint8_t key[kKeySize], const uint8_t in[kBlockSize],
                  uint8_t out[kBlockSize]);

// AES-128-OFB over `size` bytes. `in` and `out` may be the same pointer.
// `iv` is not modified. Any trailing partial block is handled.
void CryptOfb(const uint8_t key[kKeySize], const uint8_t iv[kBlockSize], const uint8_t *in,
              uint8_t *out, size_t size);

}  // namespace aes
