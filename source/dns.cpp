#include "dns.h"

#include <cstring>

namespace dns {

namespace {

// Encodes "a.b.c" as the length-prefixed label sequence DNS expects.
bool EncodeName(const std::string &host, std::vector<uint8_t> &out) {
    if (host.empty()) return false;

    size_t start = 0;
    for (;;) {
        const size_t dot = host.find('.', start);
        const size_t end = (dot == std::string::npos) ? host.size() : dot;
        const size_t len = end - start;
        if (len == 0 || len > 63) return false;

        out.push_back(static_cast<uint8_t>(len));
        out.insert(out.end(), host.begin() + start, host.begin() + end);

        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    out.push_back(0);
    return true;
}

// Steps over a (possibly compressed) name, returning the offset just past it,
// or -1 if it runs off the end of the packet. Compression pointers are not
// followed -- we only ever need to skip names, never read them, which also
// makes a malicious pointer loop impossible.
int SkipName(const uint8_t *buf, size_t len, size_t pos) {
    while (pos < len) {
        const uint8_t n = buf[pos];
        if (n == 0) return static_cast<int>(pos) + 1;
        if ((n & 0xC0) == 0xC0) {
            // A pointer is two bytes and always ends the name.
            return (pos + 1 < len) ? static_cast<int>(pos) + 2 : -1;
        }
        if ((n & 0xC0) != 0) return -1;  // reserved label type
        pos += 1 + n;
    }
    return -1;
}

}  // namespace

bool BuildQuery(const std::string &host, uint16_t id, std::vector<uint8_t> &out) {
    out.clear();
    out.reserve(host.size() + 18);

    out.push_back(static_cast<uint8_t>(id >> 8));
    out.push_back(static_cast<uint8_t>(id & 0xFF));
    out.push_back(0x01);  // flags: standard query, recursion desired
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);  // QDCOUNT = 1
    for (int i = 0; i < 6; i++) out.push_back(0x00);  // AN/NS/AR = 0

    if (!EncodeName(host, out)) {
        out.clear();
        return false;
    }

    out.push_back(0x00);
    out.push_back(0x01);  // QTYPE  = A
    out.push_back(0x00);
    out.push_back(0x01);  // QCLASS = IN
    return true;
}

bool ParseResponse(const uint8_t *buf, size_t len, uint16_t id, uint8_t out_ipv4[4]) {
    if (!buf || len < 12) return false;

    const uint16_t reply_id = static_cast<uint16_t>((buf[0] << 8) | buf[1]);
    if (reply_id != id) return false;
    if ((buf[3] & 0x0F) != 0) return false;  // RCODE != NOERROR

    const int qdcount = (buf[4] << 8) | buf[5];
    const int ancount = (buf[6] << 8) | buf[7];
    if (ancount <= 0) return false;

    size_t pos = 12;
    for (int i = 0; i < qdcount; i++) {
        const int next = SkipName(buf, len, pos);
        if (next < 0) return false;
        pos = static_cast<size_t>(next) + 4;  // QTYPE + QCLASS
        if (pos > len) return false;
    }

    for (int i = 0; i < ancount; i++) {
        const int next = SkipName(buf, len, pos);
        if (next < 0) return false;
        pos = static_cast<size_t>(next);
        if (pos + 10 > len) return false;

        const uint16_t type  = static_cast<uint16_t>((buf[pos] << 8) | buf[pos + 1]);
        const uint16_t rdlen = static_cast<uint16_t>((buf[pos + 8] << 8) | buf[pos + 9]);
        pos += 10;
        if (pos + rdlen > len) return false;

        if (type == 1 && rdlen == 4) {  // A record
            std::memcpy(out_ipv4, buf + pos, 4);
            return true;
        }
        pos += rdlen;  // CNAME etc: keep looking
    }
    return false;
}

}  // namespace dns
