#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void w16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
}
static void w32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)v);
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 24));
}

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize n = f.tellg();
    if (n <= 0) return false;
    out.resize((size_t)n);
    f.seekg(0);
    f.read((char*)out.data(), n);
    return (bool)f;
}

static bool write_file(const std::string& path, const uint8_t* data, size_t size) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write((const char*)data, (std::streamsize)size);
    return (bool)f;
}

struct ImageEntry {
    uint16_t w = 0, h = 0, hot_x = 0, hot_y = 0, fmt = 0, reserved = 0;
    std::vector<uint8_t> payload;
};

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: %s <in Assets.dat> <out Assets.dat> <image_count> <sound_count> "
                     "<tex3ds path> [format=auto-etc1]\n",
                     argv[0]);
        return 1;
    }
    std::string in_path = argv[1];
    std::string out_path = argv[2];
    int image_count = std::atoi(argv[3]);
    int sound_count = std::atoi(argv[4]);
    std::string tex3ds_path = argv[5];
    std::string format = argc > 6 ? argv[6] : "auto-etc1";

    if (image_count < 0 || sound_count < 0) {
        std::fprintf(stderr, "repack_textures: bad image/sound count\n");
        return 1;
    }

    std::vector<uint8_t> in;
    if (!read_file(in_path, in)) {
        std::fprintf(stderr, "repack_textures: could not read %s\n", in_path.c_str());
        return 1;
    }

    size_t header_size = (size_t)image_count * 2 + (size_t)(image_count + sound_count) * 4;
    if (in.size() < header_size) {
        std::fprintf(stderr, "repack_textures: %s too small for header (image_count/sound_count "
                             "mismatch?)\n",
                     in_path.c_str());
        return 1;
    }

    fs::path tmp_dir =
        fs::temp_directory_path() / ("kwik_repack_tmp_" + fs::path(out_path).filename().string());
    std::error_code ec;
    fs::create_directories(tmp_dir, ec);

    std::vector<ImageEntry> images(image_count);
    int converted = 0, kept = 0, failed = 0;

    for (int i = 0; i < image_count; ++i) {
        size_t off_pos = (size_t)image_count * 2 + (size_t)i * 4;
        uint32_t off = rd32(&in[off_pos]);
        if ((size_t)off + 16 > in.size()) {
            std::fprintf(stderr, "repack_textures: image %d offset out of range\n", i);
            return 1;
        }
        ImageEntry& e = images[i];
        e.w = rd16(&in[off]);
        e.h = rd16(&in[off + 2]);
        e.hot_x = rd16(&in[off + 4]);
        e.hot_y = rd16(&in[off + 6]);
        e.fmt = rd16(&in[off + 8]);
        e.reserved = rd16(&in[off + 10]);
        uint32_t size = rd32(&in[off + 12]);
        if ((size_t)off + 16 + size > in.size()) {
            std::fprintf(stderr, "repack_textures: image %d payload out of range\n", i);
            return 1;
        }
        const uint8_t* payload = &in[off + 16];

        if (e.fmt != 0) {
            e.payload.assign(payload, payload + size);
            ++kept;
            continue;
        }

        if (e.w < 8 || e.h < 8) {
            e.payload.assign(payload, payload + size);
            ++kept;
            continue;
        }

        fs::path png_path = tmp_dir / ("img_" + std::to_string(i) + ".png");
        fs::path t3x_path = tmp_dir / ("img_" + std::to_string(i) + ".t3x");
        if (!write_file(png_path.string(), payload, size)) {
            std::fprintf(stderr, "repack_textures: could not write temp file for image %d\n", i);
            e.payload.assign(payload, payload + size);
            ++failed;
            continue;
        }

        std::string cmd = "\"" + tex3ds_path + "\" -f " + format + " -o \"" + t3x_path.string() +
                          "\" \"" + png_path.string() + "\"";
        int rc = std::system(cmd.c_str());
        std::vector<uint8_t> t3x;
        if (rc == 0 && read_file(t3x_path.string(), t3x) && !t3x.empty()) {
            e.payload = std::move(t3x);
            e.fmt = 1;
            ++converted;
        } else {
            std::fprintf(stderr, "repack_textures: tex3ds failed for image %d (rc=%d), keeping PNG\n",
                         i, rc);
            e.payload.assign(payload, payload + size);
            ++failed;
        }
        fs::remove(png_path, ec);
        fs::remove(t3x_path, ec);
    }
    fs::remove(tmp_dir, ec);

    std::vector<uint32_t> sound_offsets(sound_count);
    for (int i = 0; i < sound_count; ++i) {
        size_t off_pos = (size_t)image_count * 2 + (size_t)(image_count + i) * 4;
        sound_offsets[i] = rd32(&in[off_pos]);
    }

    uint32_t blob_region_start = (uint32_t)in.size();
    for (int i = 0; i < sound_count; ++i)
        blob_region_start = std::min(blob_region_start, sound_offsets[i]);
    if (sound_count > 0 && blob_region_start > in.size()) {
        std::fprintf(stderr, "repack_textures: blob region start out of range\n");
        return 1;
    }
    std::vector<uint8_t> blob_region(in.begin() + blob_region_start, in.end());

    size_t new_header_size = (size_t)image_count * 2 + (size_t)(image_count + sound_count) * 4;
    std::vector<uint8_t> header, data;
    for (int i = 0; i < image_count; ++i) w16(header, (uint16_t)i);
    for (int i = 0; i < image_count; ++i) {
        const ImageEntry& e = images[i];
        w32(header, (uint32_t)(data.size() + new_header_size));
        w16(data, e.w);
        w16(data, e.h);
        w16(data, e.hot_x);
        w16(data, e.hot_y);
        w16(data, e.fmt);
        w16(data, e.reserved);
        w32(data, (uint32_t)e.payload.size());
        data.insert(data.end(), e.payload.begin(), e.payload.end());
    }
    size_t blob_region_new_start = data.size() + new_header_size;
    for (int i = 0; i < sound_count; ++i)
        w32(header, (uint32_t)(blob_region_new_start + (sound_offsets[i] - blob_region_start)));
    data.insert(data.end(), blob_region.begin(), blob_region.end());

    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        std::fprintf(stderr, "repack_textures: could not write %s\n", out_path.c_str());
        return 1;
    }
    out.write((const char*)header.data(), (std::streamsize)header.size());
    out.write((const char*)data.data(), (std::streamsize)data.size());
    out.close();

    std::fprintf(stderr, "repack_textures: %d converted, %d already-packed, %d failed (kept PNG)\n",
                 converted, kept, failed);
    return 0;
}
