# wuc24

An **Aroma (WUPS) plugin** for the Wii U that downloads **WiiConnect24** content
from **WiiLink** and writes it into the **vWii NAND** — all from Wii U (Cafe OS)
mode, without booting into vWii.

> **Status: in development — still does not write to NAND.** The whole pipeline
> (read tasks → download → decode → update the channel's container) works and is
> exercised by a **dry run** that writes its result to the SD card instead of the
> NAND. See [Roadmap](#roadmap).

> **Warning.** The end goal writes to the vWii's internal NAND. That can brick
> vWii if the on-disk formats are wrong. Do not run the write features (when they
> exist) without a full NAND backup. This is experimental homebrew.

## Why this is non-trivial

On a real Wii/vWii, an app like [NWC24-Manager] barely does anything: the Wii's
**KD** IOS daemon (`/dev/net/kd/request`, running on the Starlet coprocessor)
does all the real work — HTTPS to the servers, signature checks, decode, and the
NAND writes. The app just fires IOCTLs (`DownloadNow`, `SaveMailNow`).

**In Wii U / Cafe OS mode, KD is not running and the vWii NAND isn't mounted.**
So that shortcut is gone. This plugin has to *become* KD:

1. **Mount the vWii NAND** — the WC24 files live in the console's `SLCCMPT` NAND
   partition, which Cafe OS doesn't normally mount. We use [libmocha] to mount
   `/dev/slccmpt01` and unlock an FSA client so it may read/write there.
2. **Do the networking ourselves** — HTTPS(+HTTP) from Cafe OS to WiiLink's WC24
   servers.
3. **Decode the WC24 download format** — parse the download wrapper and
   decompress to the raw per-channel data.
4. **Write into the channel's VFF** — WC24 content lives inside `VFF` (virtual
   FAT) containers in the vWii NAND.

## Architecture

```
Wii U (Cafe OS) — Aroma plugin
  config menu (WUPS)                       source/main.cpp
   └─ RunScan / RunFetchTest / RunDumpVff / RunDryRun
        ├─ VwiiNand   mount slccmpt01 via libmocha + FSA    source/nand.{h,cpp}
        ├─ tasks      parse /shared2/wc24/nwc24dl.bin       source/tasks.{h,cpp}
        ├─ net        HTTP GET from WiiLink                 source/net.{h,cpp}
        ├─ wc24dec    strip the WC24 download wrapper       source/wc24dec.{h,cpp}
        └─ vff        read/write files inside a channel VFF source/vff.{h,cpp}
                      (FatFs + a diskio shim)              third_party/fatfs/

on-NAND formats + paths                                     source/wc24.h
host-side format tests (no console needed)                  tools/vff_test.cpp
```

### DNS

`nwc24dl.bin` still lists Nintendo's original hostnames for some tasks
(`*.wapp.wii.com`, `*.nintendowifi.net`). Those resolve to dead Nintendo
infrastructure through normal DNS, but **WiiLink's DNS (167.235.229.36)
redirects them** to WiiLink's own servers — which is how a vWii configured with
that DNS still gets Mii Contest, Wii Sports Resort and Mario Kart content.

A vWii has that DNS set in its network settings; the Wii U side does not, and a
plugin cannot repoint the console's resolver. So this plugin speaks DNS itself
(`source/dns.{h,cpp}`) and queries WiiLink's server directly, falling back to
the console's resolver whenever the custom server doesn't answer — a wrong or
unreachable value degrades rather than breaking downloads.

Use **DNS TEST** to see every task hostname resolved both ways side by side.

### Subtasks

A single `nwc24dl.bin` entry can stand for a *set* of numbered downloads. When
its `subtask_bitmask` is non-zero, each set bit is a valid subtask id, and both
the URL and the stored filename gain a `.NN` suffix:

| Channel | `subtask_bitmask` | Fetches | Stores |
|---|---|---|---|
| Forecast | `0x00000000` | `forecast.bin` | `3.bin` |
| Everybody Votes | `0x00000000` | `voting.bin` | `voting.bin` |
| News | `0x00FFFFFF` | `news.bin.00` … `.23` | `2.bin.00` … `.23` |

This is confirmed from a live console: the News container holds exactly
`2.bin.00`–`2.bin.23`, the plain `news.bin` URL is a 404, `news.bin.23` exists
and `news.bin.24` does not. Suffixing must be driven by the bitmask — the
servers happily serve *any* index, and Everybody Votes' `voting.bin.00` is a
different, smaller file than its `voting.bin`, so blindly appending `.00` would
quietly break a channel that currently works.

### The download format

A task's HTTP response is a fixed **320-byte `WC24File` header** followed by the
payload (the header is skipped wholesale — see `source/wc24dec.cpp` for why the
RSA signature isn't verified). The payload is what goes into the channel's
container, generally Nintendo LZ77 (`0x10`) data which the vWii channel itself
decompresses at runtime — decoding it is not this plugin's job.

Tasks flagged *encrypted* (`nwc24dl.bin` entry flag bit 3) are additionally
AES-128-OFB encrypted. The key is the `aes_key` field of the title's
`/title/<low>/<high>/data/wc24pubk.mod`, and the IV travels in the header being
stripped. That file only exists once the title has been opted in to the
service, so a missing one is reported as exactly that.

### Stale hostnames

Some entries still point at Nintendo hostnames that have been dead since 2013.
WiiLink serves the same content on its own domains and its official patchers
rewrite the entries in place on the console — notably [WSR-Patcher] for Wii
Sports Resort's Check Mii Out data. This plugin substitutes the hostname at
download time instead (`kHostOverrides` in `source/main.cpp`), so nothing extra
has to be written to NAND:

| Entry | Original (dead) | Used instead |
|---|---|---|
| Wii Sports Resort | `miicontest.wapp.wii.com` | `miicontest24.wiilink.ca` |

Note this is a *hostname* swap with the path preserved — the same thing
WSR-Patcher does, which is why the two names are exactly the same length.

[WSR-Patcher]: https://github.com/WiiLink24/WSR-Patcher

### Testing without a console

`tools/vff_test.cpp` builds with a normal host compiler and runs the real VFF
code against data captured from a console, so container/format work can be
iterated on without a build → copy → reboot cycle:

```sh
g++ -std=gnu++20 -I source -I third_party/fatfs -o /tmp/vff_test \
    tools/vff_test.cpp source/vff.cpp third_party/fatfs/ff.c \
    third_party/fatfs/ffsystem.c third_party/fatfs/ffunicode.c
/tmp/vff_test wuc24_forecast_vff_raw.bin wuc24_forecast_raw.bin
```

It checks that a payload can be written into a real container, that neighbouring
files come back byte-identical, that the filename's exact spelling is preserved,
and that a write which doesn't fit fails **without** damaging the container.

`tools/dns_test.cpp` does the same for the DNS code, using real captured
responses (including the CNAME chain a WiiLink hostname returns). Worth running
under sanitizers, since it parses untrusted network data:

```sh
g++ -std=gnu++20 -fsanitize=address,undefined -I source -o /tmp/dns_test \
    tools/dns_test.cpp source/dns.cpp && /tmp/dns_test
```

The struct layouts in `source/wc24.h` are derived from the Dolphin Emulator
project via [NWC24-Manager], cross-checked against [WiiBrew]. Cafe OS is
big-endian (same as the Wii), so the on-NAND big-endian structs are read/written
directly with no byte-swapping — **except** the FAT area inside a VFF, which has
a 16-bit byte-order quirk handled in the (future) VFF disk-IO layer.

## Building

Requires Docker. The build runs entirely inside the official `wiiu-env` images
(devkitPPC + wut + WUPS + libmocha), pinned in the `Dockerfile`.

```sh
docker build -t wuc24-builder .
docker run --rm -v "$PWD":/project wuc24-builder make
```

Output: `wuc24.wps`. Install it to `sd:/wiiu/environments/aroma/plugins/`.

To use: boot Aroma, open the plugin config menu (**L + Down + Minus**), toggle an
item ON, then close the menu (the action runs on close). Everything is logged to
`wuc24.log` in the root of the SD card.

| Menu item | What it does | Touches NAND? |
|---|---|---|
| Scan | Lists the vWii's WC24 download tasks | reads only |
| Fetch test | Downloads one task's raw bytes to SD | no |
| Dump VFF | Copies the Forecast container to SD | reads only |
| Dry run | Download → decode → update **copies** of the containers → SD | reads only |
| Arm NAND write | Safety gate; the actions below refuse without it | no |
| COMMIT | Same as dry run, then writes the results back to NAND | **writes** |
| CLEAR | Deletes the WC24 files from the containers (for testing) | **writes** |
| RESTORE | Writes the SD backups back to NAND | **writes** |

Tasks that share a container are applied **together** in one read/modify/write —
the Forecast Channel's `3.bin` and `4.bin` both live in the same `wc24dl.vff`,
so writing them separately would undo the first write. Every payload for a
container is downloaded and decoded *before* the container is touched, so a
failed download leaves it completely alone.

Encrypted tasks and `https://` tasks are reported and skipped, not silently
ignored.

Dry run and COMMIT execute the *same* code path and differ only in the final
step, so what the dry run verifies is exactly what COMMIT would write. Both
leave `wuc24_vff_before.bin` (the untouched original) and `wuc24_vff_after.bin`
on the SD card.

COMMIT will not proceed unless the backup was written to SD first. After
writing it reads the container straight back off NAND and compares byte for
byte; if the write or the verification fails it attempts to restore the
original automatically, and says so loudly if that also fails.

### What a bad write would actually cost

The only thing written is one channel's `wc24dl.vff` data file — no system
titles, no IOS, no filesystem metadata beyond that file — and it is rewritten
at exactly its original size. The realistic failure mode is the Forecast
Channel showing stale or broken data, recoverable by restoring
`wuc24_vff_before.bin` or by letting the channel download again. Take a full
vWii NAND backup first anyway.

## Roadmap

- [x] Plugin scaffold + config menu, builds to `.wps`
- [x] vWii NAND (slccmpt) mount via libmocha + FSA
- [x] Read & summarise `nwc24dl.bin` download tasks (read-only)
- [x] VFF container read/write (FatFs + diskio shim), validated offline
- [x] HTTP client (Cafe OS sockets) — verified against WiiLink
- [x] WC24 download-file decode (strip the 320-byte wrapper)
- [x] End-to-end dry run: download → decode → container update → SD
- [x] Write the updated container back to NAND (backup + arming + verify +
      auto-restore) — *written, not yet exercised on hardware*
- [x] Run every eligible task, grouped per container
- [x] Subtask fan-out (`url.NN` → `filename.NN`) — News Channel
- [x] AES-128-OFB for encrypted tasks (`wc24pubk.mod`)
- [ ] HTTPS, for the tasks that need it (Mario Kart Wii)
- [ ] Handle mail entries (`wc24recv.mbx`)

## Credits / references

- [NWC24-Manager] by Noah Pistilli (BSD-2) — WC24 formats & IOCTL reference
- [evWii] by GaryOderNichts (GPLv2) — Aroma plugin + libmocha patterns
- [WiiBrew] WiiConnect24 / VFF / `/dev/net/kd/request` documentation
- [WiiLink] — the WiiConnect24 revival service this targets

[NWC24-Manager]: https://github.com/noahpistilli/NWC24-Manager
[evWii]: https://github.com/garyodernichts/evwii
[libmocha]: https://github.com/wiiu-env/libmocha
[WiiBrew]: https://wiibrew.org/wiki/WiiConnect24
[WiiLink]: https://wiilink.ca/
