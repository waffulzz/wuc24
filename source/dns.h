// dns.h — just enough DNS to ask a specific server for an A record.
//
// The console's resolver cannot be pointed at a different server from inside a
// plugin, and WiiLink's DNS is what makes the dead Nintendo hostnames still
// listed in nwc24dl.bin resolve to WiiLink's servers. So we speak DNS
// ourselves: one query, one answer, first A record wins.
//
// These functions are pure (no sockets, no platform headers) so they can be
// exercised on a host against real captured responses -- see tools/dns_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dns {

// Builds a standard recursive query for `host`'s A record.
// Returns false if the hostname cannot be encoded (empty or over-long label).
bool BuildQuery(const std::string &host, uint16_t id, std::vector<uint8_t> &out);

// Parses a response produced for `id`. On success writes the first A record
// found into out_ipv4 (4 bytes, network order) and returns true.
//
// Returns false for a truncated packet, a mismatched id, a non-zero RCODE, or
// a response that carries no A record. CNAMEs and other record types in the
// answer section are stepped over rather than treated as failures -- a real
// lookup of a WiiLink host comes back as CNAME + A.
bool ParseResponse(const uint8_t *buf, size_t len, uint16_t id, uint8_t out_ipv4[4]);

}  // namespace dns
