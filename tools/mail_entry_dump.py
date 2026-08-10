#!/usr/bin/env python3
"""Decode a WiiConnect24 mailbox index and check it against the stored messages.

    ./mail_entry_dump.py wc24recv.ctl [messages-dir]

The index (.ctl) holds a 128-byte header followed by fixed 128-byte entries.
Each entry locates header values inside its message with (length << 20) | offset
-- so given the message bodies (extract the .mbx with vff_ls) every packed field
can be resolved and checked, which is how the encoding was worked out.

Receive lists hold 255 entries and store bodies as mb/r<id:07d>.msg; send lists
hold 127 and use an 's' prefix.
"""
import datetime
import os
import struct
import sys

HEADER = "> I I I I I I I I I I 48s 40s"
ENTRY_FIELDS = [
    ("id", ">I", 0), ("flag", ">I", 4), ("msg_size", ">I", 8), ("app_id", "4s", 12),
    ("header_len", ">I", 16), ("tag", ">I", 20), ("wii_cmd", ">I", 24), ("crc32", ">I", 28),
    ("from_friend_code", ">Q", 32), ("minutes_since_1900", ">I", 40), ("padding", ">I", 44),
    ("always_1", ">B", 48), ("n_multipart", ">B", 49), ("app_group", ">H", 50),
    ("packed_from", ">I", 52), ("packed_to", ">I", 56), ("packed_subject", ">I", 60),
    ("packed_charset", ">I", 64), ("packed_xfer_encoding", ">I", 68),
    ("message_offset", ">I", 72), ("encoded_length", ">I", 76),
    ("multipart0_off", ">I", 80), ("multipart0_size", ">I", 84),
    ("multipart1_off", ">I", 88), ("multipart1_size", ">I", 92),
    ("message_length", ">I", 112), ("dwc_id", ">I", 116), ("always_0x80000000", ">I", 120),
]
PACKED = ["packed_from", "packed_to", "packed_subject", "packed_charset",
          "packed_xfer_encoding"]


def when(minutes):
    if not minutes:
        return "(unset)"
    return (datetime.datetime(1900, 1, 1) +
            datetime.timedelta(minutes=minutes)).strftime("%d %b %Y %H:%M UTC")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    data = open(sys.argv[1], "rb").read()
    msg_dir = sys.argv[2] if len(sys.argv) > 2 else None

    h = struct.unpack(HEADER, data[:128])
    capacity = (len(data) - 128) // 128
    print(f"{sys.argv[1]}: {len(data)} bytes, {capacity} entry slots")
    print(f"  magic=0x{h[0]:08X} version={h[1]} number_of_mail={h[2]} total_entries={h[3]}")
    print(f"  total_size_of_messages={h[4]} filesize={h[5]}")
    print(f"  next_entry_id={h[6]} next_entry_offset={h[7]} vff_free_space={h[9]}")
    print(f"  mail_flag={h[11].split(bytes(1))[0].decode('ascii','replace')!r}\n")

    found = 0
    for i in range(capacity):
        raw = data[128 + i * 128: 256 + i * 128]
        if struct.unpack(">I", raw[0:4])[0] == 0:
            continue
        found += 1
        vals = {}
        for name, fmt, off in ENTRY_FIELDS:
            vals[name] = struct.unpack(fmt, raw[off:off + struct.calcsize(fmt)])[0]

        print(f"=== slot {i} ===")
        for name, _, _ in ENTRY_FIELDS:
            v = vals[name]
            if isinstance(v, bytes):
                print(f"  {name:22s} {v!r}")
            elif name in PACKED:
                print(f"  {name:22s} 0x{v:08X}  offset={v & 0xFFFFF:<6} length={v >> 20}")
            elif name == "minutes_since_1900":
                print(f"  {name:22s} {v:<12} {when(v)}")
            else:
                print(f"  {name:22s} {v:<12} 0x{v:08X}")

        if not msg_dir:
            print()
            continue

        # Resolve the packed fields against the message this entry describes.
        prefix = "r" if capacity > 200 else "s"
        for candidate in (f"mb_{prefix}{vals['id']:07d}.msg", f"{prefix}{vals['id']:07d}.msg"):
            path = os.path.join(msg_dir, candidate)
            if os.path.exists(path):
                break
        else:
            print(f"  (no message file for id {vals['id']} in {msg_dir})\n")
            continue

        body = open(path, "rb").read()
        print(f"  --- {candidate}: {len(body)} bytes on disk, msg_size={vals['msg_size']}, "
              f"32-byte aligned: {len(body) % 32 == 0} ---")
        for name in PACKED:
            v = vals[name]
            off, ln = v & 0xFFFFF, v >> 20
            print(f"    {name:22s} -> {body[off:off+ln]!r}")
        off, ln = vals["message_offset"], vals["encoded_length"]
        print(f"    {'body':22s} -> {body[off:off+min(ln,60)]!r}")
        print()

    if not found:
        print("no populated entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
