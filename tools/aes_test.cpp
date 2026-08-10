// aes_test.cpp — checks the AES implementation against published vectors.
//
// A wrong AES does not fail loudly; it produces plausible-looking garbage that
// would then be written into a channel's data. So it is checked against the
// official known-answer vectors from FIPS-197 (block encryption) and NIST
// SP 800-38A (OFB mode).
//
//   g++ -std=gnu++20 -fsanitize=address,undefined -I source -o /tmp/aes_test \
//       tools/aes_test.cpp source/aes.cpp && /tmp/aes_test
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aes.h"

namespace {

int g_failures = 0;

void Check(bool condition, const std::string &what) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", what.c_str());
    if (!condition) g_failures++;
}

std::vector<uint8_t> FromHex(const std::string &hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

std::string ToHex(const uint8_t *data, size_t len) {
    std::string out;
    char        buf[3];
    for (size_t i = 0; i < len; i++) {
        std::snprintf(buf, sizeof(buf), "%02x", data[i]);
        out += buf;
    }
    return out;
}

}  // namespace

int main() {
    // -- FIPS-197 Appendix B / C.1: AES-128 known answer -------------------
    std::printf("1. FIPS-197 AES-128 block encryption\n");
    {
        const auto key   = FromHex("000102030405060708090a0b0c0d0e0f");
        const auto plain = FromHex("00112233445566778899aabbccddeeff");
        uint8_t    out[16];
        aes::EncryptBlock(key.data(), plain.data(), out);
        const std::string got = ToHex(out, 16);
        std::printf("       %s\n", got.c_str());
        Check(got == "69c4e0d86a7b0430d8cdb78070b4c55a", "matches the published ciphertext");
    }

    std::printf("\n2. FIPS-197 Appendix B worked example\n");
    {
        const auto key   = FromHex("2b7e151628aed2a6abf7158809cf4f3c");
        const auto plain = FromHex("3243f6a8885a308d313198a2e0370734");
        uint8_t    out[16];
        aes::EncryptBlock(key.data(), plain.data(), out);
        const std::string got = ToHex(out, 16);
        std::printf("       %s\n", got.c_str());
        Check(got == "3925841d02dc09fbdc118597196a0b32", "matches the published ciphertext");
    }

    // -- NIST SP 800-38A F.4.1: OFB-AES128 ---------------------------------
    std::printf("\n3. NIST SP 800-38A OFB-AES128 (4 blocks)\n");
    {
        const auto key = FromHex("2b7e151628aed2a6abf7158809cf4f3c");
        const auto iv  = FromHex("000102030405060708090a0b0c0d0e0f");
        const auto plain = FromHex(
            "6bc1bee22e409f96e93d7e117393172a"
            "ae2d8a571e03ac9c9eb76fac45af8e51"
            "30c81c46a35ce411e5fbc1191a0a52ef"
            "f69f2445df4f9b17ad2b417be66c3710");
        const std::string expected =
            "3b3fd92eb72dad20333449f8e83cfb4a"
            "7789508d16918f03f53c52dac54ed825"
            "9740051e9c5fecf64344f7a82260edcc"
            "304c6528f659c77866a510d9c1d6ae5e";

        std::vector<uint8_t> out(plain.size());
        aes::CryptOfb(key.data(), iv.data(), plain.data(), out.data(), out.size());
        const std::string got = ToHex(out.data(), out.size());
        std::printf("       %s\n", got.c_str());
        Check(got == expected, "keystream matches the published vector");

        // OFB is its own inverse.
        std::vector<uint8_t> round_trip(out.size());
        aes::CryptOfb(key.data(), iv.data(), out.data(), round_trip.data(), round_trip.size());
        Check(round_trip == plain, "decrypting the ciphertext restores the plaintext");
    }

    std::printf("\n4. behaviour the WC24 payloads depend on\n");
    {
        const auto key = FromHex("2b7e151628aed2a6abf7158809cf4f3c");
        const auto iv  = FromHex("000102030405060708090a0b0c0d0e0f");

        // WC24 payloads are arbitrary lengths, not block multiples.
        const auto full = FromHex(
            "6bc1bee22e409f96e93d7e117393172a"
            "ae2d8a571e03ac9c9eb76fac45af8e51");
        for (size_t len : {size_t{1}, size_t{15}, size_t{16}, size_t{17}, size_t{31}}) {
            std::vector<uint8_t> enc(len), dec(len);
            aes::CryptOfb(key.data(), iv.data(), full.data(), enc.data(), len);
            aes::CryptOfb(key.data(), iv.data(), enc.data(), dec.data(), len);
            Check(std::memcmp(dec.data(), full.data(), len) == 0,
                  "round-trips a " + std::to_string(len) + "-byte payload");
        }

        // Decryption must work in place, since that is how it will be used.
        std::vector<uint8_t> buf(full.begin(), full.end());
        aes::CryptOfb(key.data(), iv.data(), buf.data(), buf.data(), buf.size());
        aes::CryptOfb(key.data(), iv.data(), buf.data(), buf.data(), buf.size());
        Check(std::memcmp(buf.data(), full.data(), full.size()) == 0, "in-place round trip");

        // The IV the caller passes must not be modified.
        auto                 iv_copy = iv;
        std::vector<uint8_t> scratch(full.size());
        aes::CryptOfb(key.data(), iv_copy.data(), full.data(), scratch.data(), scratch.size());
        Check(iv_copy == iv, "caller's IV is left untouched");

        // Zero length must be a no-op, not a crash.
        aes::CryptOfb(key.data(), iv.data(), full.data(), scratch.data(), 0);
        Check(true, "zero-length input handled");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
