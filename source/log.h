// log.h — tiny logging helpers.
//
// Every LOG() line goes to OSReport (in case a syslog/USB listener is up)
// AND to a plain text file on the SD card ("<sd root>/wuc24.log"), which is
// the reliable path when no logging module / network listener is present.
//
// Call LogInit() once from plugin init and LogDeinit() from plugin deinit;
// LOG() itself works even if LogInit() failed or wasn't called (it just
// silently skips the file and still OSReports).
#pragma once

// Mounts the SD card and opens (truncates) <sd root>/wuc24.log for this
// session. Safe to call even if it fails — LOG() degrades to OSReport-only.
void LogInit();

// Unmounts the SD card. Call once from plugin deinit.
void LogDeinit();

// Bare SD mount path (e.g. "fs:/vol/external01"), or nullptr if the SD card
// failed to mount / LogInit() wasn't called. Other modules build their own
// "<mount>/whatever" paths off this rather than re-mounting the card.
const char *GetSdMountPath();

// printf-style; do not call directly, use the LOG() macro below.
void LogPrintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define LOG(fmt, ...)  LogPrintf("[wuc24] " fmt, ##__VA_ARGS__)
