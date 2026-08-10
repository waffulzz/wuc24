// vff_test.cpp — host-side test for the VFF layer.
//
// Builds with a normal host compiler (not devkitPPC) and runs against real
// data captured from a console, so the container format can be validated and
// iterated on without a build/flash/reboot cycle. See tools/README.md.
//
//   g++ -std=gnu++20 -I source -I third_party/fatfs -o /tmp/vff_test \
//       tools/vff_test.cpp source/vff.cpp third_party/fatfs/ff.c \
//       third_party/fatfs/ffsystem.c third_party/fatfs/ffunicode.c
//   /tmp/vff_test wuc24_forecast_vff_raw.bin wuc24_forecast_raw.bin
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "log.h"
#include "vff.h"

// Host stand-in for the plugin's logger (log.cpp is Wii U only).
void LogPrintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fputc('\n', stdout);
}

namespace {

int g_failures = 0;

void Check(bool condition, const std::string &what) {
    std::printf("  [%s] %s\n", condition ? " OK " : "FAIL", what.c_str());
    if (!condition) g_failures++;
}

// FAT names are case-insensitive; the container stores "3.bin" as a long
// filename, so compare accordingly.
bool NameEquals(const std::string &a, const char *b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// A WC24 channel payload is Nintendo LZ77: 0x10 then a 24-bit little-endian
// decompressed size.
struct Lz77Header {
    uint8_t  type;
    uint32_t decompressed_size;
};
Lz77Header ParseLz77(const std::vector<uint8_t> &data) {
    Lz77Header h{};
    if (data.size() >= 4) {
        h.type              = data[0];
        h.decompressed_size = data[1] | (data[2] << 8) | (data[3] << 16);
    }
    return h;
}

bool LoadFile(const char *path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        std::printf("cannot open %s\n", path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(size));
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::printf("usage: %s <vff-image> <raw-wc24-download>\n", argv[0]);
        return 2;
    }

    std::vector<uint8_t> original;
    if (!LoadFile(argv[1], original)) return 2;

    std::vector<uint8_t> download;
    if (!LoadFile(argv[2], download)) return 2;

    // The WC24 wrapper is a fixed 320-byte header; the payload follows.
    constexpr size_t kWc24HeaderSize = 320;
    if (download.size() <= kWc24HeaderSize) {
        std::printf("download too small to contain a WC24 header\n");
        return 2;
    }
    const std::vector<uint8_t> payload(download.begin() + kWc24HeaderSize, download.end());

    std::printf("VFF image: %s (%zu bytes)\n", argv[1], original.size());
    std::printf("payload:   %s (%zu bytes after 320-byte WC24 header)\n\n", argv[2], payload.size());

    // -- 1. mount + list ----------------------------------------------------
    std::printf("1. mount and list root directory\n");
    std::vector<uint8_t> buffer = original;
    std::vector<vff::Image::Entry> entries;
    uint32_t size_3bin_before = 0, size_4bin_before = 0;
    {
        vff::Image image(buffer);
        Check(image.ok(), "image mounted");
        if (!image.ok()) return 1;

        Check(image.List(entries), "listed root directory");
        for (const auto &e : entries) {
            std::printf("       %-16s %8u %s\n", e.name.c_str(), e.size, e.is_dir ? "<dir>" : "");
            if (NameEquals(e.name, "3.bin")) size_3bin_before = e.size;
            if (NameEquals(e.name, "4.bin")) size_4bin_before = e.size;
        }
        Check(!entries.empty(), "root directory is not empty");
    }

    // -- 2. read existing content ------------------------------------------
    std::printf("\n2. read existing files\n");
    std::vector<uint8_t> before_3bin, before_4bin;
    {
        vff::Image image(buffer);
        Check(image.ok(), "image re-mounted");
        Check(image.ReadFile("3.bin", before_3bin), "read 3.bin");
        Check(image.ReadFile("4.bin", before_4bin), "read 4.bin");
        Check(before_3bin.size() == size_3bin_before, "3.bin size matches directory entry");
        Check(before_4bin.size() == size_4bin_before, "4.bin size matches directory entry");
    }

    // Cross-check that what the channel already has on NAND and what the
    // server just gave us are the same KIND of thing. The bytes themselves
    // differ -- the on-NAND copy was downloaded earlier and the weather has
    // changed since -- so compare the format headers, not the content.
    std::printf("\n3. cross-check on-NAND 3.bin against freshly downloaded payload\n");
    const Lz77Header on_nand    = ParseLz77(before_3bin);
    const Lz77Header downloaded = ParseLz77(payload);
    std::printf("       on-NAND    : %zu bytes, LZ77 type 0x%02X, decompresses to %u\n",
                before_3bin.size(), on_nand.type, on_nand.decompressed_size);
    std::printf("       downloaded : %zu bytes, LZ77 type 0x%02X, decompresses to %u\n",
                payload.size(), downloaded.type, downloaded.decompressed_size);
    Check(on_nand.type == 0x10, "on-NAND payload is Nintendo LZ77 (0x10)");
    Check(downloaded.type == 0x10, "downloaded payload is Nintendo LZ77 (0x10)");
    Check(on_nand.decompressed_size == downloaded.decompressed_size,
          "both decompress to the same size (same channel data structure)");

    // -- 4. write the freshly downloaded payload ---------------------------
    std::printf("\n4. write downloaded payload into the container as 3.bin\n");
    {
        vff::Image image(buffer);
        Check(image.ok(), "image mounted for writing");
        Check(image.WriteFile("3.bin", payload.data(), static_cast<uint32_t>(payload.size())),
              "wrote 3.bin");
    }

    Check(buffer.size() == original.size(), "container size unchanged by write");
    Check(std::memcmp(buffer.data(), original.data(), 32) == 0, "VFF header unchanged by write");

    // -- 5. verify the write, and that neighbours survived ------------------
    std::printf("\n5. verify written content and neighbouring file\n");
    {
        vff::Image image(buffer);
        Check(image.ok(), "image re-mounted after write");

        std::vector<uint8_t> after_3bin;
        Check(image.ReadFile("3.bin", after_3bin), "read back 3.bin");
        Check(after_3bin.size() == payload.size(), "3.bin size == payload size");
        Check(after_3bin == payload, "3.bin content == payload content");

        std::vector<uint8_t> after_4bin;
        Check(image.ReadFile("4.bin", after_4bin), "read back 4.bin");
        Check(after_4bin == before_4bin, "4.bin untouched by writing 3.BIN");

        std::vector<vff::Image::Entry> after_entries;
        Check(image.List(after_entries), "listed root directory after write");
        Check(after_entries.size() == entries.size(), "directory entry count unchanged");

        // The channel looks the file up by the name recorded in nwc24dl.bin
        // ("3.bin"), so the stored name must keep its exact spelling.
        bool exact_name = false;
        for (const auto &e : after_entries) {
            if (e.name == "3.bin") exact_name = true;
        }
        Check(exact_name, "written entry is still named exactly '3.bin'");
    }

    // -- 6. failure must not corrupt ---------------------------------------
    std::printf("\n6. oversized write fails without destroying the container\n");
    {
        std::vector<uint8_t> buffer2 = original;
        std::vector<uint8_t> huge(original.size() * 2, 0xAB);
        {
            vff::Image image(buffer2);
            Check(image.ok(), "image mounted");
            Check(!image.WriteFile("BIG.BIN", huge.data(), static_cast<uint32_t>(huge.size())),
                  "oversized write correctly failed");
        }
        {
            vff::Image image(buffer2);
            Check(image.ok(), "image still mountable after failed write");
            std::vector<uint8_t> still_4bin;
            Check(image.ReadFile("4.bin", still_4bin), "4.bin still readable");
            Check(still_4bin == before_4bin, "4.bin intact after failed write");
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL CHECKS PASSED" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
