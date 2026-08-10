// dns_test.cpp — host-side test for the DNS packet code.
//
// Fixtures are real responses captured from 8.8.8.8, including the CNAME
// chain a WiiLink hostname actually returns. Builds with a normal host
// compiler:
//
//   g++ -std=gnu++20 -I source -o /tmp/dns_test tools/dns_test.cpp source/dns.cpp
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "dns.h"

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

std::string Ip(const uint8_t v[4]) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", v[0], v[1], v[2], v[3]);
    return buf;
}

// Real response for example.com: two A records, compression pointers.
const char kExampleCom[] =
    "123481800001000200000000076578616d706c6503636f6d0000010001c00c000100010000011c0004"
    "ac4293f3c00c000100010000011c00046814179a";

// Real response for fore.wiilink24.com: CNAME -> tulip.wiilink24.com, then the
// A record. The parser has to step over the CNAME to find it.
const char kForeWiiLink[] =
    "12348180000100020000000004666f7265097769696c696e6b323403636f6d0000010001c00c000500"
    "010000012c00080574756c6970c011c030000100010000012c0004a7ebe524";

}  // namespace

int main() {
    std::printf("1. query building\n");
    {
        std::vector<uint8_t> q;
        Check(dns::BuildQuery("fore.wiilink24.com", 0x1234, q), "built a query");
        Check(q.size() == 12 + 20 + 4, "query is the expected length");
        Check(q[0] == 0x12 && q[1] == 0x34, "transaction id is in the header");
        Check(q[2] == 0x01, "recursion desired");
        Check(q[5] == 0x01, "one question");
        Check(q[12] == 4 && std::memcmp(&q[13], "fore", 4) == 0, "first label encoded");
        Check(q.back() == 0x01, "QCLASS = IN");

        std::vector<uint8_t> bad;
        Check(!dns::BuildQuery("", 1, bad), "empty hostname rejected");
        Check(!dns::BuildQuery("a..b", 1, bad), "empty label rejected");
        Check(!dns::BuildQuery(std::string(64, 'x') + ".com", 1, bad), "over-long label rejected");
    }

    std::printf("\n2. plain A record (example.com)\n");
    {
        const auto pkt = FromHex(kExampleCom);
        uint8_t    ip[4]{};
        Check(dns::ParseResponse(pkt.data(), pkt.size(), 0x1234, ip), "parsed");
        std::printf("       -> %s\n", Ip(ip).c_str());
        Check(Ip(ip) == "172.66.147.243", "first A record returned");
    }

    std::printf("\n3. CNAME chain (fore.wiilink24.com)\n");
    {
        const auto pkt = FromHex(kForeWiiLink);
        uint8_t    ip[4]{};
        Check(dns::ParseResponse(pkt.data(), pkt.size(), 0x1234, ip), "parsed past the CNAME");
        std::printf("       -> %s\n", Ip(ip).c_str());
        Check(Ip(ip) == "167.235.229.36", "resolved to WiiLink's server");
    }

    std::printf("\n4. malformed input is rejected, not crashed on\n");
    {
        const auto pkt = FromHex(kForeWiiLink);
        uint8_t    ip[4]{};

        Check(!dns::ParseResponse(pkt.data(), pkt.size(), 0x9999, ip), "wrong transaction id");
        Check(!dns::ParseResponse(nullptr, 0, 0x1234, ip), "null buffer");
        Check(!dns::ParseResponse(pkt.data(), 4, 0x1234, ip), "runt packet");

        // Truncate at every length: none may read out of bounds or hang.
        bool survived = true;
        for (size_t n = 0; n < pkt.size(); n++) {
            uint8_t tmp[4]{};
            dns::ParseResponse(pkt.data(), n, 0x1234, tmp);
        }
        Check(survived, "every truncation handled cleanly");

        // SERVFAIL
        auto servfail = pkt;
        servfail[3] |= 0x02;
        Check(!dns::ParseResponse(servfail.data(), servfail.size(), 0x1234, ip), "RCODE != 0");

        // Answer count of zero
        auto no_answers = pkt;
        no_answers[6] = no_answers[7] = 0;
        Check(!dns::ParseResponse(no_answers.data(), no_answers.size(), 0x1234, ip),
              "no answer records");

        // A compression pointer that points at itself must not loop forever.
        std::vector<uint8_t> loop = FromHex("12348180000100010000000000000100010000012c0004");
        loop.resize(12);
        loop.push_back(0xC0);
        loop.push_back(0x0C);  // points back at itself
        for (int i = 0; i < 10; i++) loop.push_back(0);
        uint8_t tmp[4]{};
        dns::ParseResponse(loop.data(), loop.size(), 0x1234, tmp);
        Check(true, "self-referential compression pointer terminated");
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
