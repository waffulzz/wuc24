# wuc24 (WiiUConnect24)
WiiConnect24 client for the vWii, in Wii U mode.

WiiLink still serves WiiConnect24 content to vWii consoles, but collecting it normally requires using a tool every time you boot vWii or waiting on the menu for hours. wuc24 automatically downloads the content from your NWC24 download list to SD card, copies it to NAND & fetches/sends your mailboxes. 

## Requirements
- a Wii U running Aroma
- WiiLink already installed on the vWii
- NotificationModule installed in aroma (should be included by default)

## Installing
Drop `wuc24.wps` in `sd:/wiiu/environments/aroma/plugins`

## Settings
Reachable from aroma Plugin config
- update at boot: on by default - automatically runs the wc24 download at console boot
- Include mail: send and recieve your Wii Mail during the download run
- Update now: starts a download immediately
- Restore from SD backups: puts back the last backup of every wc24 content container
- Channels: toggles for every individual channel
- Diagnostics: read only tools, for debugging purposes *only in dev builds*

## Brick Risk
wuc24 only writes to your vWii NAND, never the Wii U storage. The risk is a bricked vWii, which can be restored via homebrew tools from a backup or straight from ninty's download servers.

## What doesn't work
- Mario Kart Wii Competitions: not implemented, may implement in future release
- Wii Shop: discontinued

## Credit
**This project was written primarily with AI. All third party references used are documented in `THIRD-PARTY.md` and licenses are included in `licenses/`**
