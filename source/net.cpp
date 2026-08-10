#include "net.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nsysnet/_socket.h>  // socket_lib_init/finish

#include "dns.h"
#include "log.h"

namespace net {

namespace {

bool        s_inited = false;
std::string s_dnsServer;

// A DNS server that never answers would otherwise cost a full timeout on every
// single request -- with dozens of downloads per run that is minutes of dead
// waiting. After this many consecutive silences we stop asking and just use
// the console's resolver, which handles every hostname that actually matters.
constexpr int kMaxDnsFailures = 2;
int           s_dnsFailures   = 0;

// --- DNS ---------------------------------------------------------------
//
// Packet building/parsing lives in dns.cpp so it can be tested on a host
// against real captured responses; this is just the socket I/O around it.

constexpr uint16_t kDnsPort      = 53;
constexpr int      kDnsTimeoutMs = 1500;
constexpr int      kDnsPollMs    = 25;

// Asks `server` for `host`'s A record.
//
// The socket is put in non-blocking mode and polled by hand rather than using
// poll()/select(): this platform has no SO_RCVTIMEO, and doing the waiting
// ourselves keeps the timeout behaviour obvious and independent of how well
// the console's poll() handles UDP.
bool QueryDnsServer(const std::string &server, const std::string &host, struct in_addr *out,
                    bool verbose) {
    if (server.empty()) return false;

    static uint16_t s_queryId = 0x1234;
    const uint16_t  id        = ++s_queryId;

    std::vector<uint8_t> query;
    if (!dns::BuildQuery(host, id, query)) return false;

    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;

    struct sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(kDnsPort);
    if (inet_pton(AF_INET, server.c_str(), &addr.sin_addr) != 1) {
        close(fd);
        return false;
    }

    int nonblock = 1;
    setsockopt(fd, SOL_SOCKET, SO_NONBLOCK, &nonblock, sizeof(nonblock));

    const int sent = sendto(fd, query.data(), query.size(), 0,
                            reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr));
    if (sent < 0) {
        if (verbose) LOG("dns: sendto(%s) failed", server.c_str());
        close(fd);
        return false;
    }

    uint8_t reply[512];
    int     received = -1;
    for (int waited = 0; waited < kDnsTimeoutMs; waited += kDnsPollMs) {
        received = recv(fd, reply, sizeof(reply), 0);
        if (received > 0) break;
        OSSleepTicks(OSMillisecondsToTicks(kDnsPollMs));
    }
    close(fd);

    if (received <= 0) {
        if (verbose) LOG("dns: %s did not answer within %dms", server.c_str(), kDnsTimeoutMs);
        return false;
    }

    uint8_t ipv4[4];
    if (!dns::ParseResponse(reply, static_cast<size_t>(received), id, ipv4)) {
        if (verbose) LOG("dns: %s sent an unusable reply (%d bytes)", server.c_str(), received);
        return false;
    }

    std::memcpy(&out->s_addr, ipv4, 4);
    return true;
}

// Convenience wrapper for the configured server, with the circuit breaker.
bool ResolveViaDns(const std::string &host, struct in_addr *out) {
    if (s_dnsServer.empty() || s_dnsFailures >= kMaxDnsFailures) return false;

    if (QueryDnsServer(s_dnsServer, host, out, true)) {
        s_dnsFailures = 0;
        return true;
    }

    if (++s_dnsFailures >= kMaxDnsFailures) {
        LOG("dns: %s has not answered %d times -- using the console's resolver from now on",
            s_dnsServer.c_str(), s_dnsFailures);
    }
    return false;
}

// --- HTTP ------------------------------------------------------------------

// Splits "http://host[:port]/path..." into host, port, and path (path
// includes the leading '/' verbatim -- WC24 URLs can have repeated slashes
// like "///1/049/forecast.bin" and that must be preserved exactly).
bool ParseHttpUrl(const std::string &url, std::string &host, uint16_t &port, std::string &path) {
    constexpr char   kPrefix[]  = "http://";
    constexpr size_t kPrefixLen = sizeof(kPrefix) - 1;

    if (url.compare(0, kPrefixLen, kPrefix) != 0) {
        return false;
    }

    const std::string rest      = url.substr(kPrefixLen);
    const size_t      slash     = rest.find('/');
    const std::string host_port = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    const size_t colon = host_port.find(':');
    if (colon == std::string::npos) {
        host = host_port;
        port = 80;
    } else {
        host = host_port.substr(0, colon);
        port = static_cast<uint16_t>(std::atoi(host_port.substr(colon + 1).c_str()));
    }

    return !host.empty();
}

// Sends the whole buffer, looping over partial send()s.
bool SendAll(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Finds the "\r\n\r\n" that ends the header block. Returns false if absent.
bool SplitHeaders(const std::vector<uint8_t> &raw, size_t &header_end) {
    static const uint8_t sep[4] = {'\r', '\n', '\r', '\n'};
    if (raw.size() < 4) return false;
    for (size_t i = 0; i + 4 <= raw.size(); ++i) {
        if (std::memcmp(raw.data() + i, sep, 4) == 0) {
            header_end = i + 4;
            return true;
        }
    }
    return false;
}

int ParseStatusCode(const std::string &status_line) {
    int major = 0, minor = 0, code = 0;
    std::sscanf(status_line.c_str(), "HTTP/%d.%d %d", &major, &minor, &code);
    return code;
}

}  // namespace

void Init() {
    if (s_inited) return;
    socket_lib_init();
    s_inited = true;
}

void SetDnsServer(const char *ipv4) {
    s_dnsServer = (ipv4 && *ipv4) ? ipv4 : "";
    if (s_dnsServer.empty()) {
        LOG("dns: using the console's own resolver");
    } else {
        LOG("dns: using %s (falling back to the console's resolver)", s_dnsServer.c_str());
    }
}

std::string HostOf(const std::string &url) {
    size_t start = url.find("://");
    if (start == std::string::npos) return {};
    start += 3;

    const size_t end   = url.find('/', start);
    std::string  hostp = (end == std::string::npos) ? url.substr(start) : url.substr(start, end - start);
    const size_t colon = hostp.find(':');
    if (colon != std::string::npos) hostp.resize(colon);
    return hostp;
}

bool ResolveVia(const std::string &server, const std::string &host, std::string &out_ip) {
    struct in_addr addr {};
    if (!QueryDnsServer(server, host, &addr, false)) {
        out_ip = "-";
        return false;
    }
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &addr, buf, sizeof(buf));
    out_ip = buf;
    return true;
}

bool ResolveViaSystem(const std::string &host, std::string &out_ip) {
    struct addrinfo hints{};
    hints.ai_family      = AF_INET;
    hints.ai_socktype    = SOCK_STREAM;
    struct addrinfo *res = nullptr;

    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        if (res) freeaddrinfo(res);
        out_ip = "-";
        return false;
    }

    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &reinterpret_cast<struct sockaddr_in *>(res->ai_addr)->sin_addr, buf,
              sizeof(buf));
    freeaddrinfo(res);
    out_ip = buf;
    return true;
}

namespace {

// Resolves `host` and opens a connection, or returns -1.
//
// Prefers the configured DNS (WiiLink's is meant to redirect the dead Nintendo
// hostnames) but never lets a DNS problem be fatal: it falls back to the
// console's own resolver, which handles the WiiLink-native domains fine.
int ConnectTo(const std::string &host, uint16_t port) {
    struct sockaddr_in target {};
    target.sin_family = AF_INET;
    target.sin_port   = htons(port);

    const char *via = "custom dns";
    if (!ResolveViaDns(host, &target.sin_addr)) {
        via = "system resolver";

        struct addrinfo hints{};
        hints.ai_family      = AF_INET;
        hints.ai_socktype    = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        const int gai = getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (gai != 0 || !res) {
            LOG("http: could not resolve %s (gai=%d)", host.c_str(), gai);
            if (res) freeaddrinfo(res);
            return -1;
        }
        target.sin_addr = reinterpret_cast<struct sockaddr_in *>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    char ip_str[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &target.sin_addr, ip_str, sizeof(ip_str));
    LOG("http: %s -> %s:%u (%s)", host.c_str(), ip_str, port, via);

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG("http: socket() failed (%d)", fd);
        return -1;
    }
    if (connect(fd, reinterpret_cast<struct sockaddr *>(&target), sizeof(target)) != 0) {
        LOG("http: connect() to %s:%u failed", ip_str, port);
        close(fd);
        return -1;
    }
    return fd;
}

// Reads until the peer closes (we always ask for "Connection: close", so there
// is no need to understand Content-Length or chunked framing), then splits the
// headers off.
bool ReadResponse(int fd, std::vector<uint8_t> &out_body, int &out_status) {
    std::vector<uint8_t> raw;
    uint8_t              chunk[4096];
    for (;;) {
        const int n = recv(fd, chunk, sizeof(chunk), 0);
        if (n < 0) {
            LOG("http: recv() failed after %zu bytes", raw.size());
            return false;
        }
        if (n == 0) break;  // peer closed -- end of response
        raw.insert(raw.end(), chunk, chunk + n);
    }

    size_t header_end = 0;
    if (!SplitHeaders(raw, header_end)) {
        LOG("http: no header terminator found in %zu bytes", raw.size());
        return false;
    }

    std::string status_line(reinterpret_cast<char *>(raw.data()),
                            std::min<size_t>(raw.size(), 64));
    const size_t eol = status_line.find("\r\n");
    if (eol != std::string::npos) status_line.resize(eol);
    out_status = ParseStatusCode(status_line);

    out_body.assign(raw.begin() + header_end, raw.end());
    return true;
}

}  // namespace

bool HttpGet(const std::string &url, std::vector<uint8_t> &out_body, int &out_status) {
    out_body.clear();
    out_status = 0;

    if (url.compare(0, 8, "https://") == 0) {
        LOG("HttpGet: HTTPS not supported yet (%s)", url.c_str());
        return false;
    }

    std::string host, path;
    uint16_t    port;
    if (!ParseHttpUrl(url, host, port, path)) {
        LOG("HttpGet: could not parse URL: %s", url.c_str());
        return false;
    }

    const int fd = ConnectTo(host, port);
    if (fd < 0) return false;

    char req[1024];
    std::snprintf(req, sizeof(req),
                  "GET %s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: wuc24/0.1\r\n"
                  "Accept: */*\r\n"
                  "Connection: close\r\n"
                  "\r\n",
                  path.c_str(), host.c_str());

    if (!SendAll(fd, req)) {
        LOG("HttpGet: send() failed");
        close(fd);
        return false;
    }

    const bool ok = ReadResponse(fd, out_body, out_status);
    close(fd);
    if (ok) {
        LOG("HttpGet: %s -> status %d, %zu body bytes", url.c_str(), out_status,
            out_body.size());
    }
    return ok;
}

bool HttpPostForm(const std::string &url, const std::string &form,
                  std::vector<uint8_t> &out_body, int &out_status) {
    out_body.clear();
    out_status = 0;

    if (url.compare(0, 8, "https://") == 0) {
        LOG("HttpPostForm: HTTPS not supported yet");
        return false;
    }

    std::string host, path;
    uint16_t    port;
    if (!ParseHttpUrl(url, host, port, path)) {
        LOG("HttpPostForm: could not parse URL: %s", url.c_str());
        return false;
    }

    const int fd = ConnectTo(host, port);
    if (fd < 0) return false;

    // The form body carries mail credentials, so only its length is logged.
    char head[1024];
    std::snprintf(head, sizeof(head),
                  "POST %s HTTP/1.1\r\n"
                  "Host: %s\r\n"
                  "User-Agent: wuc24/0.1\r\n"
                  "Content-Type: application/x-www-form-urlencoded\r\n"
                  "Content-Length: %zu\r\n"
                  "Connection: close\r\n"
                  "\r\n",
                  path.c_str(), host.c_str(), form.size());

    if (!SendAll(fd, std::string(head) + form)) {
        LOG("HttpPostForm: send() failed");
        close(fd);
        return false;
    }
    LOG("HttpPostForm: %s (%zu byte form)", url.c_str(), form.size());

    const bool ok = ReadResponse(fd, out_body, out_status);
    close(fd);
    if (ok) LOG("HttpPostForm: status %d, %zu body bytes", out_status, out_body.size());
    return ok;
}

}  // namespace net
