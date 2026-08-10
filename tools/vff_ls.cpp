// vff_ls.cpp — list (and optionally extract) the contents of a VFF container.
//
// Runs the real VFF code on a host, so containers dumped off a console can be
// inspected without a build/copy/reboot cycle.
//
//   g++ -std=gnu++20 -I source -I third_party/fatfs -o /tmp/vff_ls \
//       tools/vff_ls.cpp source/vff.cpp third_party/fatfs/ff.c \
//       third_party/fatfs/ffsystem.c third_party/fatfs/ffunicode.c
//
//   /tmp/vff_ls <image>                  list everything
//   /tmp/vff_ls <image> <path> <outdir>  also extract that directory's files
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "log.h"
#include "vff.h"

void LogPrintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stdout, fmt, args);
    va_end(args);
    std::fputc('\n', stdout);
}

namespace {

bool LoadFile(const char *path, std::vector<uint8_t> &out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    out.resize(static_cast<size_t>(size));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

// Where extracted files go, or empty to only list.
std::string g_extract_dir;

// Turns "/2026/07/09/HAEA_#1/txt/320B05DA.000" into a single flat filename so
// the whole tree can be dropped in one directory without losing its path.
std::string FlatName(const std::string &path) {
    std::string out;
    for (char c : path) out += (c == '/') ? '_' : c;
    if (!out.empty() && out.front() == '_') out.erase(0, 1);
    return out;
}

// Depth-first walk of the container.
void Walk(vff::Image &image, const std::string &dir, int depth, size_t &files, size_t &bytes) {
    std::vector<vff::Image::Entry> entries;
    if (!image.List(entries, dir.c_str())) {
        std::printf("%*s(cannot list %s)\n", depth * 2, "", dir.c_str());
        return;
    }

    for (const auto &e : entries) {
        const std::string full = (dir == "/") ? "/" + e.name : dir + "/" + e.name;
        std::printf("%*s%-24s %10u %s\n", depth * 2, "", e.name.c_str(), e.size,
                    e.is_dir ? "<dir>" : "");
        if (e.is_dir) {
            Walk(image, full, depth + 1, files, bytes);
            continue;
        }

        files++;
        bytes += e.size;

        if (!g_extract_dir.empty()) {
            std::vector<uint8_t> content;
            if (!image.ReadFile(full.c_str(), content)) {
                std::printf("%*s  !! could not read\n", depth * 2, "");
                continue;
            }
            const std::string out_path = g_extract_dir + "/" + FlatName(full);
            FILE *f = std::fopen(out_path.c_str(), "wb");
            if (f) {
                std::fwrite(content.data(), 1, content.size(), f);
                std::fclose(f);
            }
        }
    }
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("usage: %s <vff-image> [extract-dir out-dir]\n", argv[0]);
        return 2;
    }

    if (argc >= 4 && std::string(argv[2]) == "--extract") {
        g_extract_dir = argv[3];
    }

    std::vector<uint8_t> image_data;
    if (!LoadFile(argv[1], image_data)) {
        std::printf("cannot read %s\n", argv[1]);
        return 2;
    }
    std::printf("%s: %zu bytes\n\n", argv[1], image_data.size());

    vff::Image image(image_data);
    if (!image.ok()) {
        std::printf("failed to mount as a VFF\n");
        return 1;
    }

    size_t files = 0, bytes = 0;
    Walk(image, "/", 0, files, bytes);
    std::printf("\n%zu file(s), %zu bytes total\n", files, bytes);

    if (argc >= 4) {
        std::vector<vff::Image::Entry> entries;
        if (!image.List(entries, argv[2])) {
            std::printf("cannot list %s\n", argv[2]);
            return 1;
        }
        for (const auto &e : entries) {
            if (e.is_dir) continue;
            const std::string in_path =
                std::string(argv[2]) + (std::string(argv[2]).back() == '/' ? "" : "/") + e.name;
            std::vector<uint8_t> content;
            if (!image.ReadFile(in_path.c_str(), content)) {
                std::printf("  failed to read %s\n", in_path.c_str());
                continue;
            }
            const std::string out_path = std::string(argv[3]) + "/" + e.name;
            FILE *f = std::fopen(out_path.c_str(), "wb");
            if (!f) {
                std::printf("  cannot write %s\n", out_path.c_str());
                continue;
            }
            std::fwrite(content.data(), 1, content.size(), f);
            std::fclose(f);
            std::printf("  extracted %s (%zu bytes)\n", out_path.c_str(), content.size());
        }
    }
    return 0;
}
