#!/usr/bin/env python3
"""Turn an extracted Wii message board (cdb.vff) into a single HTML page.

The Wii Menu stores its message board in /title/00000001/00000002/data/cdb.vff,
a VFF container holding one "CDBFILE" record per message, filed under a
YYYY/MM/DD/HH/MM path. Extract it first with the vff_ls tool:

    vff_ls cdb.vff --extract ./msgs
    ./cdb_to_html.py ./msgs board.html

Record kinds seen in the wild:
  ripl_board_record  a real message (text, sometimes with TPL images attached)
  playtimelog        the console's own "Today's Accomplishments" report
"""
import datetime
import html
import os
import re
import sys
from collections import Counter

# Offsets within a CDBFILE record. The text fields are not at fixed positions
# across record types, so the strings are found by scanning rather than by
# hardcoded offsets -- only the magic and type are relied upon.
MAGIC = b"CDBFILE"
TYPE_OFFSET = 0x14


def utf16be_runs(data, min_len=3):
    """Yield (offset, text) for runs of printable UTF-16BE."""
    runs = []
    i = 0
    while i < len(data) - 1:
        j = i
        chars = []
        while j < len(data) - 1:
            code = (data[j] << 8) | data[j + 1]
            if code in (0x0A, 0x0D):
                chars.append("\n")
            elif 0x20 <= code <= 0xFFFD and not (0xD800 <= code <= 0xDFFF):
                chars.append(chr(code))
            else:
                break
            j += 2
        if len(chars) >= min_len:
            runs.append((i, "".join(chars)))
            i = j + 2
        else:
            i += 2
    return runs


def parse_record(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if not data.startswith(MAGIC):
        return None

    rec_type = data[TYPE_OFFSET:TYPE_OFFSET + 24].split(b"\x00")[0].decode("ascii", "replace")

    # Sender: the last plausible address-looking ASCII string in the header area.
    sender = ""
    for m in re.finditer(rb"[A-Za-z0-9._%+@-]{6,}", data[:0x540]):
        candidate = m.group().decode("ascii", "replace")
        if "@" in candidate:
            sender = candidate.lstrip("\\|")
    if not sender:
        sender = "(console)" if rec_type == "playtimelog" else "(unknown)"

    # The short UTF-16BE string ahead of the body is the sender's DISPLAY NAME:
    # for received mail it is the X-Wii-AltName header, and for an address book
    # contact it is whatever name the user gave them. Play logs put the report
    # title ("Today's Accomplishments") in the same place.
    runs = [(off, text) for off, text in utf16be_runs(data) if off >= 0x400]
    display_name, body = "", ""
    if runs:
        body_off, body = max(runs, key=lambda r: len(r[1]))
        before = [t for off, t in runs if off < body_off and len(t) < 80]
        if before:
            display_name = before[-1].strip()
    body = body.strip("\n")

    # Attachments are Nintendo TPL textures embedded after the text.
    tpl_count = len(re.findall(re.escape(b"\x00\x20\xaf\x30"), data))

    # The filename is the arrival time as hex seconds since 2000-01-01, which
    # is authoritative and precise to the second. The directory path encodes
    # the same moment but with a ZERO-BASED month (C's tm_mon), so "2026/07/09"
    # there means 9 August 2026 -- reading the path as-is is off by a month.
    parts = os.path.basename(path).split("_")
    stamp = ""
    hex_id = parts[-1].split(".")[0] if parts else ""
    if re.fullmatch(r"[0-9A-Fa-f]{8}", hex_id or ""):
        when = datetime.datetime(2000, 1, 1) + datetime.timedelta(seconds=int(hex_id, 16))
        stamp = when.strftime("%Y-%m-%d %H:%M")
    elif len(parts) >= 5 and parts[0].isdigit():
        # Fall back to the path, correcting the zero-based month.
        try:
            when = datetime.datetime(int(parts[0]), int(parts[1]) + 1, int(parts[2]),
                                     int(parts[3]), int(parts[4]))
            stamp = when.strftime("%Y-%m-%d %H:%M")
        except ValueError:
            stamp = ""

    return {
        "file": os.path.basename(path),
        "type": rec_type,
        "kind": parts[-2] if len(parts) >= 2 else "?",
        "when": stamp,
        "sender": sender,
        "name": display_name,
        "body": body,
        "tpl": tpl_count,
        "size": len(data),
    }


CSS = """
:root{--bg:#f6f7f9;--card:#fff;--ink:#1c1f23;--muted:#5c6570;--line:#e2e6ea;
      --accent:#2f6fd0;--chip:#eef2f7;--log:#8a6d3b;--logbg:#fcf5e6}
:root:not([data-theme=light]){}
@media (prefers-color-scheme:dark){:root:not([data-theme=light]){
  --bg:#14171a;--card:#1c2024;--ink:#e6e9ec;--muted:#98a2ad;--line:#2b3138;
  --accent:#6ea8ff;--chip:#242a31;--log:#d8b473;--logbg:#2a2317}}
:root[data-theme=dark]{--bg:#14171a;--card:#1c2024;--ink:#e6e9ec;--muted:#98a2ad;
  --line:#2b3138;--accent:#6ea8ff;--chip:#242a31;--log:#d8b473;--logbg:#2a2317}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);
     font:15px/1.55 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
.wrap{max-width:860px;margin:0 auto;padding:32px 20px 80px}
h1{font-size:26px;margin:0 0 4px}
.sub{color:var(--muted);margin:0 0 24px}
.stats{display:flex;flex-wrap:wrap;gap:10px;margin-bottom:24px}
.stat{background:var(--card);border:1px solid var(--line);border-radius:10px;
      padding:10px 14px;min-width:96px}
.stat b{display:block;font-size:20px}
.stat span{color:var(--muted);font-size:12px}
.controls{display:flex;gap:8px;margin-bottom:20px;flex-wrap:wrap}
button{font:inherit;padding:7px 14px;border-radius:999px;border:1px solid var(--line);
       background:var(--card);color:var(--ink);cursor:pointer}
button.on{background:var(--accent);border-color:var(--accent);color:#fff}
.msg{background:var(--card);border:1px solid var(--line);border-radius:12px;
     padding:16px 18px;margin-bottom:14px}
.msg.playtimelog{background:var(--logbg)}
.meta{display:flex;flex-wrap:wrap;gap:8px;align-items:baseline;margin-bottom:8px}
.when{color:var(--muted);font-size:13px;font-variant-numeric:tabular-nums}
.from{font-weight:600;word-break:break-all}
.chip{font-size:11px;text-transform:uppercase;letter-spacing:.04em;color:var(--muted);
      background:var(--chip);border-radius:999px;padding:2px 9px}
.chip.log{color:var(--log)}
.subject{font-weight:600;margin-bottom:6px}
.addr{color:var(--muted);font-size:12px;word-break:break-all}
.body{white-space:pre-wrap;word-wrap:break-word;font-size:14px;margin:0}
.att{margin-top:10px;font-size:12px;color:var(--muted)}
.empty{color:var(--muted);font-style:italic}
footer{color:var(--muted);font-size:12px;margin-top:40px;border-top:1px solid var(--line);
       padding-top:16px}
"""

JS = """
const btns=document.querySelectorAll('button[data-filter]');
btns.forEach(b=>b.onclick=()=>{
  btns.forEach(x=>x.classList.toggle('on',x===b));
  const f=b.dataset.filter;
  document.querySelectorAll('.msg').forEach(m=>{
    m.style.display=(f==='all'||m.dataset.kind===f)?'':'none';
  });
});
"""


def render(records, out_path):
    records.sort(key=lambda r: r["when"], reverse=True)
    kinds = Counter(r["type"] for r in records)
    msgs = sum(v for k, v in kinds.items() if k != "playtimelog")
    total_tpl = sum(r["tpl"] for r in records)
    span = ""
    stamped = [r["when"] for r in records if r["when"]]
    if stamped:
        span = f"{min(stamped)[:7]} to {max(stamped)[:7]}"

    parts = [
        "<!-- Generated by tools/cdb_to_html.py from a Wii message board dump -->",
        f"<style>{CSS}</style>",
        "<div class=wrap>",
        "<h1>Wii Message Board archive</h1>",
        f"<p class=sub>Recovered from cdb.vff &middot; {span}</p>",
        "<div class=stats>",
        f"<div class=stat><b>{len(records)}</b><span>records</span></div>",
        f"<div class=stat><b>{msgs}</b><span>messages</span></div>",
        f"<div class=stat><b>{kinds.get('playtimelog',0)}</b><span>play logs</span></div>",
        f"<div class=stat><b>{total_tpl}</b><span>images</span></div>",
        "</div>",
        "<div class=controls>",
        "<button data-filter=all class=on>Everything</button>",
        "<button data-filter=ripl_board_record>Messages only</button>",
        "<button data-filter=playtimelog>Play history</button>",
        "</div>",
    ]

    for r in records:
        is_log = r["type"] == "playtimelog"
        parts.append(f'<div class="msg {r["type"]}" data-kind="{r["type"]}">')
        parts.append("<div class=meta>")
        parts.append(f'<span class=when>{html.escape(r["when"] or "?")}</span>')
        parts.append(f'<span class=from>{html.escape(r["name"] or r["sender"])}</span>')
        if r["name"] and r["name"] != r["sender"]:
            parts.append(f'<span class=addr>{html.escape(r["sender"])}</span>')
        parts.append(
            f'<span class="chip{" log" if is_log else ""}">'
            f'{"play log" if is_log else r["kind"]}</span>')
        parts.append("</div>")

        if r["body"]:
            parts.append(f'<pre class=body>{html.escape(r["body"])}</pre>')
        else:
            parts.append('<p class="body empty">(no text)</p>')
        if r["tpl"]:
            parts.append(
                f'<div class=att>{r["tpl"]} attached image(s), '
                f'Nintendo TPL format &middot; {r["size"]:,} bytes total</div>')
        parts.append("</div>")

    parts.append(
        "<footer>Extracted from the vWii NAND with wuc24. "
        "Play-history entries are generated by the console itself, not received mail."
        "</footer>")
    parts.append("</div>")
    parts.append(f"<script>{JS}</script>")

    with open(out_path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(parts))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    records = []
    for name in sorted(os.listdir(src)):
        if not name.endswith(".000"):
            continue
        rec = parse_record(os.path.join(src, name))
        if rec:
            records.append(rec)
    render(records, out)
    print(f"{len(records)} records -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
