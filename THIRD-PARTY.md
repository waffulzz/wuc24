# Third-party code and attribution

wuc24 itself is published without a licence of its own. This file records what
it was built from, who holds the copyright, and under what terms — the full
licence texts are in `licenses/`.

One thing to be aware of before redistributing it: parts of this are derived
from the Dolphin Emulator (GPL-2.0-or-later), and it links statically against
three LGPL-3.0 libraries. Those licences apply to their own code whatever wuc24
declares about itself, and the GPL's terms reach derived work when it is
distributed. Nothing here is a copy of a Dolphin source file — the derivations
are struct layouts and format handling, listed below so the question can be
judged on the specifics rather than guessed at.

---

## Derived from the Dolphin Emulator — GPL-2.0-or-later

<https://github.com/dolphin-emu/dolphin> · Copyright (c) Dolphin Emulator
Project

Dolphin reimplements the Wii's WiiConnect24 stack, which made it the reference
for several on-disk formats that are documented nowhere else. No file here is a
copy of a Dolphin source file, but the following are close enough derivations
that they are treated as such:

| Here | From |
|---|---|
| `source/wc24.h` — `DLList*`, `WC24File`, `WC24PubkMod` | `Source/Core/Core/IOS/Network/KD/NWC24DL.h`, `WC24File.h` |
| `source/wc24.h` — `MailListHeader`, `MailListEntry` | `Source/Core/Core/IOS/Network/KD/Mail/MailCommon.h` |
| `source/wc24.h` — `VffHeader` | `Source/Core/Core/IOS/Network/KD/VFF/VFFUtil.h` |
| `source/wc24.h` — entry flag meanings (RSA-signed, encrypted) | `NWC24DL.cpp` |
| `source/vff.cpp` — hand-populated FATFS state, the −480 sector skew | `VFF/VFFUtil.cpp` |
| `source/wc24dec.cpp` — 320-byte header strip, AES-OFB decrypt | `NetKDRequest.cpp` |
| `source/msgcfg.cpp` — `nwc24msg.cfg` layout and checksum | `NWC24Config.h/.cpp` |

Two deliberate departures from Dolphin's behaviour, both for safety on real
hardware rather than in an emulator:

- Dolphin deletes a VFF outright when a write into it fails. This does not, and
  never truncates or removes a container: it is a console's only copy.
- Every sector access is bounds-checked here.

Findings that are **not** from Dolphin, because Dolphin does not implement
receiving mail, are documented in the source where they are used: the
`packed_*` encoding, the structure of a received message, and the 32-byte
padding of stored messages. Those were derived from data captured off a
console.

## FatFs — 1-clause BSD-style

<http://elm-chan.org/fsw/ff/> · Copyright (C) ChaN

Vendored verbatim in `third_party/fatfs/`, licence text at
`third_party/fatfs/LICENSE.txt`, per-file copyright headers intact. Only
`ffconf.h` is configured; the sources are unmodified. The disk layer that backs
it (`source/vff.cpp`) is ours.

## WiiLink — MIT

- <https://github.com/WiiLink24/kaitais> · Copyright (c) 2023 WiiLink
- <https://github.com/WiiLink24/Mail-Server> · Copyright (c) 2023 WiiLink

Used as documentation rather than as source. The kaitai definitions confirmed
mail list layouts already derived from Dolphin, and supplied the meaning of the
`mail_flag` field. Mail-Server established the mail protocol: the CGI
endpoints, their parameters, and their response shapes. Protocol and format
facts are not copyrightable, and no code was taken, but the debt is real and
this project would have been guesswork without them.

## WiiLink WSR-Patcher — no licence stated

<https://github.com/WiiLink24/WSR-Patcher>

Carries no licence file, so no code was taken from it. What is used is a single
fact: that Wii Sports Resort's Check Mii Out data is served from
`miicontest24.wiilink.ca`, the host its own README describes it as patching in.
`kHostOverrides` in `source/main.cpp` applies that substitution at download
time.

## Linked libraries — LGPL-3.0

- [WiiUPluginSystem](https://github.com/wiiu-env/WiiUPluginSystem)
- [libmocha](https://github.com/wiiu-env/libmocha)
- [libnotifications](https://github.com/wiiu-env/libnotifications)

Statically linked. LGPL-3.0 section 4 allows this provided the recipient can
relink the work against a modified version of the library; complete
corresponding source for this project is available, and the build is
reproducible with the pinned toolchain in `Dockerfile`, which satisfies that.

## wut

<https://github.com/devkitPro/wut>

The Wii U toolchain headers and runtime, used through the devkitPro
distribution.
