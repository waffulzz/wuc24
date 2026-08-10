// net.h — minimal HTTP client for fetching WC24 content from Cafe OS.
//
// This is deliberately small: plain HTTP/1.1 GET, "Connection: close" so we
// never have to parse Content-Length/chunked framing (we just read until the
// peer closes). HTTPS is not supported yet — that's a later milestone once
// we've proven DNS + plain sockets actually reach WiiLink from Wii U mode.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace net {

// WiiLink's public DNS. It resolves its own domains *and* hijacks the dead
// Nintendo ones (wapp.wii.com, nintendowifi.net) to its own servers, which is
// how a vWii configured with this DNS still reaches Mii Contest / Wii Sports
// Resort / Mario Kart content. A vWii has it set in its network settings; the
// Wii U side does not, so a plugin has to ask for it explicitly.
inline constexpr char kWiiLinkDns[] = "167.235.229.36";

// Brings up the Cafe OS socket library. Safe to call more than once
// (idempotent); call once from plugin init before any HttpGet().
void Init();

// Resolve hostnames through this DNS server (dotted IPv4) instead of the
// console's configured resolver. Pass nullptr to go back to the system
// resolver. Resolution always falls back to the system resolver if the custom
// server does not answer, so a wrong or unreachable value degrades rather than
// breaking downloads.
void SetDnsServer(const char *ipv4);

// Minimal HTTP/1.1 GET of `url` (must start with "http://"; "https://"
// returns false immediately). On success, `out_body` holds the raw response
// body and `out_status` the HTTP status code (e.g. 200).
bool HttpGet(const std::string &url, std::vector<uint8_t> &out_body, int &out_status);

// HTTP/1.1 POST of an application/x-www-form-urlencoded body, which is how the
// WC24 mail CGI endpoints are addressed. The form is never logged: it carries
// the console's mail credentials.
bool HttpPostForm(const std::string &url, const std::string &form,
                  std::vector<uint8_t> &out_body, int &out_status);

// A well-known public resolver, used as a control in the DNS diagnostics: if
// a lookup works through this but not through WiiLink's server, the DNS client
// itself is fine and the problem is the server or the network path to it.
inline constexpr char kControlDns[] = "1.1.1.1";

// Resolves `host` by querying `server` directly. Returns false if the server
// does not answer in time or the reply is unusable. `out_ip` gets the
// dotted-quad address on success.
bool ResolveVia(const std::string &server, const std::string &host, std::string &out_ip);

// Resolves `host` through the console's own resolver.
bool ResolveViaSystem(const std::string &host, std::string &out_ip);

// Extracts the hostname from an http(s) URL. Empty on failure.
std::string HostOf(const std::string &url);

}  // namespace net
