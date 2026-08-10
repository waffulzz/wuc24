#include "aes.h"

#include <cstring>

namespace aes {

namespace {

constexpr int kRounds = 10;  // AES-128
constexpr int kNk     = 4;   // key words
constexpr int kNb     = 4;   // block words

const uint8_t kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

const uint8_t kRcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

// Multiply by 2 in GF(2^8), reducing by the AES polynomial.
inline uint8_t XTime(uint8_t x) {
    return static_cast<uint8_t>((x << 1) ^ ((x >> 7) * 0x1b));
}

using RoundKeys = uint8_t[(kRounds + 1) * 16];

void ExpandKey(const uint8_t key[kKeySize], RoundKeys round_keys) {
    std::memcpy(round_keys, key, 16);

    for (int i = kNk; i < kNb * (kRounds + 1); i++) {
        uint8_t t[4];
        std::memcpy(t, round_keys + (i - 1) * 4, 4);

        if (i % kNk == 0) {
            const uint8_t tmp = t[0];  // RotWord
            t[0] = kSbox[t[1]];
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[tmp];
            t[0] = static_cast<uint8_t>(t[0] ^ kRcon[i / kNk]);
        }

        for (int j = 0; j < 4; j++) {
            round_keys[i * 4 + j] = static_cast<uint8_t>(round_keys[(i - kNk) * 4 + j] ^ t[j]);
        }
    }
}

void AddRoundKey(uint8_t state[16], const uint8_t *round_key) {
    for (int i = 0; i < 16; i++) state[i] ^= round_key[i];
}

void SubBytes(uint8_t state[16]) {
    for (int i = 0; i < 16; i++) state[i] = kSbox[state[i]];
}

// The state is column-major: byte (r, c) lives at state[c * 4 + r].
void ShiftRows(uint8_t state[16]) {
    uint8_t tmp;

    tmp          = state[1];
    state[1]     = state[5];
    state[5]     = state[9];
    state[9]     = state[13];
    state[13]    = tmp;

    tmp          = state[2];
    state[2]     = state[10];
    state[10]    = tmp;
    tmp          = state[6];
    state[6]     = state[14];
    state[14]    = tmp;

    tmp          = state[15];
    state[15]    = state[11];
    state[11]    = state[7];
    state[7]     = state[3];
    state[3]     = tmp;
}

void MixColumns(uint8_t state[16]) {
    for (int c = 0; c < 4; c++) {
        uint8_t *col = state + c * 4;
        const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
        const uint8_t all = static_cast<uint8_t>(a0 ^ a1 ^ a2 ^ a3);

        col[0] ^= static_cast<uint8_t>(XTime(static_cast<uint8_t>(a0 ^ a1)) ^ all);
        col[1] ^= static_cast<uint8_t>(XTime(static_cast<uint8_t>(a1 ^ a2)) ^ all);
        col[2] ^= static_cast<uint8_t>(XTime(static_cast<uint8_t>(a2 ^ a3)) ^ all);
        col[3] ^= static_cast<uint8_t>(XTime(static_cast<uint8_t>(a3 ^ a0)) ^ all);
    }
}

}  // namespace

void EncryptBlock(const uint8_t key[kKeySize], const uint8_t in[kBlockSize],
                  uint8_t out[kBlockSize]) {
    RoundKeys round_keys;
    ExpandKey(key, round_keys);

    uint8_t state[16];
    std::memcpy(state, in, 16);

    AddRoundKey(state, round_keys);
    for (int round = 1; round < kRounds; round++) {
        SubBytes(state);
        ShiftRows(state);
        MixColumns(state);
        AddRoundKey(state, round_keys + round * 16);
    }
    SubBytes(state);
    ShiftRows(state);
    AddRoundKey(state, round_keys + kRounds * 16);

    std::memcpy(out, state, 16);
}

void CryptOfb(const uint8_t key[kKeySize], const uint8_t iv[kBlockSize], const uint8_t *in,
              uint8_t *out, size_t size) {
    uint8_t feedback[kBlockSize];
    std::memcpy(feedback, iv, kBlockSize);

    size_t done = 0;
    while (done < size) {
        // OFB: the keystream is E(previous keystream block), independent of
        // the data, so encryption and decryption are identical.
        EncryptBlock(key, feedback, feedback);

        const size_t chunk = (size - done < kBlockSize) ? (size - done) : kBlockSize;
        for (size_t i = 0; i < chunk; i++) {
            out[done + i] = static_cast<uint8_t>(in[done + i] ^ feedback[i]);
        }
        done += chunk;
    }
}

}  // namespace aes
