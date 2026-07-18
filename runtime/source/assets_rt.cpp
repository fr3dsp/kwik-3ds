#include "gml_runtime.h"
#include "engine_internal.h"
#include "render.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#ifdef _WIN32
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <vector>
#include <deque>
#include <algorithm>
#include <unordered_map>

namespace gml {

struct LoadedImage {
    unsigned int tex = 0;
    int w = 0;
    int h = 0;
    bool tried = false;
    bool ok = false;
    int fails = 0;
};

static std::FILE* g_assets_file = nullptr;
static size_t g_assets_size = 0;
static bool g_assets_tried = false;
static std::vector<LoadedImage> g_images;

static const char* g_read_fail = "";
static int g_read_errno = 0;

static bool read_range(size_t o, size_t len, void* out) {
    if (!g_assets_file) { g_read_fail = "nofile"; return false; }
    if (o + len > g_assets_size) { g_read_fail = "bounds"; return false; }
    std::clearerr(g_assets_file);
    errno = 0;
    if (std::fseek(g_assets_file, (long)o, SEEK_SET) != 0) {
        g_read_fail = "seek";
        g_read_errno = errno;
        return false;
    }
    size_t got = std::fread(out, 1, len, g_assets_file);
    if (got != len) {
        g_read_fail = "short";
        g_read_errno = errno;
        return false;
    }
    return true;
}

static uint32_t rd32(size_t o) {
    unsigned char b[4];
    if (!read_range(o, 4, b)) return 0;
    return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t rd16(size_t o) {
    unsigned char b[2];
    if (!read_range(o, 2, b)) return 0;
    return (uint16_t)(b[0] | (b[1] << 8));
}

static void ensure_assets() {
    if (g_assets_tried) return;
    g_assets_tried = true;
    const char* path = g_assets_path.empty() ? "Assets.dat" : g_assets_path.c_str();
    g_assets_file = std::fopen(path, "rb");
    if (g_assets_file) {
        std::fseek(g_assets_file, 0, SEEK_END);
        long n = std::ftell(g_assets_file);
        g_assets_size = n > 0 ? (size_t)n : 0;
    } else {
        std::fprintf(stderr, "[kwik] could not open %s\n", path);
    }
    render_debug_log("assets: path='%s' g_image_count=%d g_assets_size=%zu toc_bytes=%zu",
                     path, g_image_count, g_assets_size, (size_t)g_image_count * 2 * 4);
    g_images.resize(g_image_count);
}

const unsigned char* kwik_sound_blob(int blob_index, unsigned int& size, int& type) {
    ensure_assets();
    size = 0;
    type = 0;
    if (blob_index < 0) return nullptr;
    size_t off = rd32((size_t)g_image_count * 2 + (size_t)(g_image_count + blob_index) * 4);
    if (off == 0 || off + 8 > g_assets_size) return nullptr;
    type = (int)rd32(off);
    uint32_t sz = rd32(off + 4);
    if (off + 8 + sz > g_assets_size) return nullptr;
    static std::vector<unsigned char> scratch;
    if (scratch.capacity() > (1u << 20) && sz * 2 < scratch.capacity())
        std::vector<unsigned char>().swap(scratch);
    scratch.resize(sz);
    if (sz && !read_range(off + 8, sz, scratch.data())) {
        static int log_budget = 16;
        if (log_budget > 0) {
            --log_budget;
            render_debug_log("assets: blob %d read failed (%s errno=%d off=%u sz=%u)",
                             blob_index, g_read_fail, g_read_errno, (unsigned)off, sz);
        }
        return nullptr;
    }
    size = sz;
    return scratch.data();
}

static std::vector<KwikSprite> g_dyn_sprites;
static std::deque<std::string> g_dyn_sprite_names;

static std::unordered_map<int, KwikSprite> g_sprite_overrides;

const KwikSprite* kwik_sprite_at(int idx) {
    auto ov = g_sprite_overrides.find(idx);
    if (ov != g_sprite_overrides.end()) return &ov->second;
    if (idx >= 0 && idx < g_sprite_count) return &g_sprites[idx];
    int d = idx - g_sprite_count;
    if (d >= 0 && d < (int)g_dyn_sprites.size()) return &g_dyn_sprites[d];
    return nullptr;
}

static KwikSprite* sprite_mutable(int idx) {
    int d = idx - g_sprite_count;
    if (d >= 0 && d < (int)g_dyn_sprites.size()) return &g_dyn_sprites[d];
    const KwikSprite* s = kwik_sprite_at(idx);
    if (!s) return nullptr;
    auto it = g_sprite_overrides.find(idx);
    if (it == g_sprite_overrides.end()) it = g_sprite_overrides.insert({idx, *s}).first;
    return &it->second;
}

void kwik_sprite_override_bbox(int spr, int l, int t, int r, int b) {
    KwikSprite* s = sprite_mutable(spr);
    if (!s) return;
    s->bbox_left = l;
    s->bbox_top = t;
    s->bbox_right = r;
    s->bbox_bottom = b;
}

void kwik_sprite_override_offset(int spr, int ox, int oy) {
    KwikSprite* s = sprite_mutable(spr);
    if (!s) return;
    s->origin_x = ox;
    s->origin_y = oy;
}

int kwik_sprite_total() { return g_sprite_count + (int)g_dyn_sprites.size(); }

int kwik_register_dynamic_sprite(const KwikSprite& s) {
    g_dyn_sprites.push_back(s);
    g_dyn_sprite_names.push_back(s.name ? s.name : "dyn_sprite");
    g_dyn_sprites.back().name = g_dyn_sprite_names.back().c_str();
    return g_sprite_count + (int)g_dyn_sprites.size() - 1;
}

int kwik_register_dynamic_image(unsigned int tex, int w, int h) {
    ensure_assets();
    LoadedImage img;
    img.tex = tex;
    img.w = w;
    img.h = h;
    img.tried = true;
    img.ok = tex != 0;
    g_images.push_back(img);
    return (int)g_images.size() - 1;
}

static void invalidate_loaded_image(void* user_data) {
    int index = (int)(intptr_t)user_data;
    if (index < 0 || index >= (int)g_images.size()) return;
    g_images[index].tried = false;
    g_images[index].ok = false;
    g_images[index].tex = 0;
}

#ifdef __3DS__
struct PayloadCacheEntry {
    std::vector<unsigned char> bytes;
    unsigned long long last_used = 0;
};
static std::unordered_map<int, PayloadCacheEntry> g_payload_cache;
static size_t g_payload_cache_bytes = 0;
static const size_t kPayloadCacheBudget = 4u * 1024u * 1024u;

static void payload_cache_evict_to_fit(size_t need) {
    while (g_payload_cache_bytes + need > kPayloadCacheBudget && !g_payload_cache.empty()) {
        auto oldest = g_payload_cache.begin();
        for (auto it = g_payload_cache.begin(); it != g_payload_cache.end(); ++it)
            if (it->second.last_used < oldest->second.last_used) oldest = it;
        g_payload_cache_bytes -= oldest->second.bytes.size();
        g_payload_cache.erase(oldest);
    }
}

static const std::vector<unsigned char>* payload_cache_get(int index) {
    auto it = g_payload_cache.find(index);
    if (it == g_payload_cache.end()) return nullptr;
    it->second.last_used = g_frame_counter;
    return &it->second.bytes;
}

static void payload_cache_put(int index, std::vector<unsigned char>&& bytes) {
    if (bytes.size() > kPayloadCacheBudget) return;
    payload_cache_evict_to_fit(bytes.size());
    g_payload_cache_bytes += bytes.size();
    g_payload_cache[index] = {std::move(bytes), g_frame_counter};
}
#endif

static LoadedImage& load_image(int index) {
    ensure_assets();
    static LoadedImage dummy;
    if (index < 0 || index >= (int)g_images.size()) return dummy;
    LoadedImage& img = g_images[index];
    if (img.tried) {
        if (img.ok) render_touch_texture(img.tex);
        return img;
    }

    auto fail_soft = [&](const char* why) -> LoadedImage& {
        if (img.fails < 2)
            render_debug_log("assets: image %d load failed (%s %s errno=%d), attempt %d", index,
                             why, g_read_fail, g_read_errno, img.fails + 1);
        if (++img.fails >= 8) img.tried = true;
        return img;
    };

    size_t off = rd32((size_t)g_image_count * 2 + (size_t)index * 4);
    if (off == 0 || off + 16 > g_assets_size) { img.tried = true; return img; }
    uint16_t format = rd16(off + 8);
    uint32_t payload_size = rd32(off + 12);
    if (off + 16 + payload_size > g_assets_size) { img.tried = true; return img; }

    const unsigned char* payload_ptr = nullptr;
    std::vector<unsigned char> payload_storage;
#ifdef __3DS__
    if (const std::vector<unsigned char>* cached = payload_cache_get(index))
        payload_ptr = cached->data();
#endif
    if (!payload_ptr) {
        payload_storage.resize(payload_size);
        if (payload_size && !read_range(off + 16, payload_size, payload_storage.data()))
            return fail_soft("read");
#ifdef __3DS__
        payload_cache_put(index, std::vector<unsigned char>(payload_storage));
#endif
        payload_ptr = payload_storage.data();
    }

    unsigned int tex = 0;
    int w = 0, h = 0;
    if (format == 1) {
        tex = render_upload_texture_t3x(payload_ptr, payload_size);
        if (tex == 0) return fail_soft("t3x");
        w = rd16(off);
        h = rd16(off + 2);
    } else {
        int ch;
        unsigned char* pixels =
            stbi_load_from_memory(payload_ptr, (int)payload_size, &w, &h, &ch, 4);
        if (!pixels) return fail_soft("decode");
        tex = render_upload_texture(pixels, w, h);
        stbi_image_free(pixels);
        if (tex == 0) return fail_soft("upload");
        int lw = rd16(off), lh = rd16(off + 2);
        if (lw > 0 && lh > 0) {
            w = lw;
            h = lh;
        }
    }
    img.tex = tex;
    img.w = w;
    img.h = h;
    img.ok = true;
    img.tried = true;
    img.fails = 0;
    render_register_evictable(tex, invalidate_loaded_image, (void*)(intptr_t)index);
    return img;
}

struct CachedMask {
    MaskSet ms;
    std::vector<unsigned char> bytes;
    int attempts = 0;
    bool permanent_fail = false;
    unsigned long long next_retry_frame = 0;
};

const MaskSet* kwik_sprite_masks(int spr) {
    static std::unordered_map<int, CachedMask> cache;
    CachedMask& slot = cache[spr];
    if (slot.ms.count > 0) return &slot.ms;
    if (slot.permanent_fail) return nullptr;
    if (slot.attempts >= 3 && g_frame_counter < slot.next_retry_frame) return nullptr;
    const KwikSprite* s = kwik_sprite_at(spr);
    if (!s || s->sep_masks != 1 || s->mask_blob < 0) {
        slot.permanent_fail = true;
        return nullptr;
    }
    {
        unsigned int size = 0;
        int type = 0;
        const unsigned char* d = kwik_sound_blob(s->mask_blob, size, type);
        if (!d) {
            if (slot.attempts == 0)
                render_debug_log("assets: mask blob %d read failed for sprite %d", s->mask_blob,
                                 spr);
            ++slot.attempts;
            slot.next_retry_frame = g_frame_counter + 180;
            return nullptr;
        }
        slot.attempts = 0;
        if (d && type == 4 && size >= 12) {
            auto r32 = [&](int o) {
                return (unsigned)d[o] | ((unsigned)d[o + 1] << 8) | ((unsigned)d[o + 2] << 16) |
                       ((unsigned)d[o + 3] << 24);
            };
            MaskSet ms;
            ms.count = (int)r32(0);
            ms.w = (int)r32(4);
            ms.h = (int)r32(8);
            ms.rowbytes = (ms.w + 7) / 8;
            size_t mask_bytes = (size_t)ms.count * ms.rowbytes * ms.h;
            if (ms.count > 0 && ms.w > 0 && ms.h > 0 && 12 + mask_bytes <= size) {
                slot.bytes.assign(d + 12, d + 12 + mask_bytes);
                ms.data = slot.bytes.data();
                slot.ms = ms;
            }
        }
    }
    return slot.ms.count > 0 ? &slot.ms : nullptr;
}

int kwik_sprite_frame_image(int spr, int sub) {
    const KwikSprite* s = kwik_sprite_at(spr);
    if (!s || s->frame_count <= 0) return -1;
    return s->first_frame + ((sub % s->frame_count) + s->frame_count) % s->frame_count;
}

void kwik_flush_textures() {
    int n = std::min((int)g_images.size(), g_image_count);
    for (int i = 0; i < n; ++i) {
        LoadedImage& img = g_images[i];
        if (!img.tried) continue;
        if (img.ok && img.tex) render_free_texture(img.tex);
        img.tex = 0;
        img.w = 0;
        img.h = 0;
        img.ok = false;
        img.tried = false;
    }
}

unsigned int kwik_image_texture(int image, int& w, int& h) {
    LoadedImage& img = load_image(image);
    w = img.w;
    h = img.h;
    return img.ok ? img.tex : 0;
}

void kwik_draw_image_part(int image, double sx, double sy, double sw, double sh, double dx,
                          double dy, double xs, double ys, unsigned int blend, double alpha) {
    LoadedImage& img = load_image(image);
    if (!img.ok || img.w <= 0 || img.h <= 0) return;
    float u0 = (float)(sx / img.w), v0 = (float)(sy / img.h);
    float u1 = (float)((sx + sw) / img.w), v1 = (float)((sy + sh) / img.h);
    render_draw_quad(img.tex, dx, dy, sw, sh, 0, 0, xs, ys, 0, u0, v0, u1, v1, blend, alpha);
}

uint32_t kwik_tileset_frame_index(const KwikTileset& ts, uint32_t idx) {
    if (ts.map_blob < 0 || ts.tile_count <= 0 || idx >= (uint32_t)ts.tile_count) return idx;
    int frames = ts.frames > 0 ? ts.frames : 1;
    const uint32_t* map = kwik_tilemap_grid(ts.map_blob, ts.tile_count * frames);
    if (!map) return idx;
    int fr = 0;
    if (frames > 1 && ts.frame_ms > 0)
        fr = (int)((long long)(now_ms() / ts.frame_ms) % frames);
    return map[idx * frames + fr];
}

void kwik_draw_image_part_rot(int image, double sx, double sy, double sw, double sh, double dx,
                              double dy, double ox, double oy, double xs, double ys, double angle,
                              unsigned int blend, double alpha) {
    LoadedImage& img = load_image(image);
    if (!img.ok || img.w <= 0 || img.h <= 0) return;
    float u0 = (float)(sx / img.w), v0 = (float)(sy / img.h);
    float u1 = (float)((sx + sw) / img.w), v1 = (float)((sy + sh) / img.h);
    render_draw_quad(img.tex, dx, dy, sw, sh, ox, oy, xs, ys, angle, u0, v0, u1, v1, blend,
                     alpha);
}

static std::unordered_map<int, std::vector<uint32_t>> g_tilemap_cache;

const uint32_t* kwik_tilemap_grid(int blob, int cells) {
    auto it = g_tilemap_cache.find(blob);
    if (it != g_tilemap_cache.end()) return it->second.empty() ? nullptr : it->second.data();
    std::vector<uint32_t>& g = g_tilemap_cache[blob];
    unsigned int size = 0;
    int type = 0;
    const unsigned char* d = kwik_sound_blob(blob, size, type);
    if (d && type == 6 && (int)(size / 4) >= cells) {
        g.resize(cells);
        for (int i = 0; i < cells; ++i)
            g[i] = (unsigned)d[i * 4] | ((unsigned)d[i * 4 + 1] << 8) |
                   ((unsigned)d[i * 4 + 2] << 16) | ((unsigned)d[i * 4 + 3] << 24);
    }
    return g.empty() ? nullptr : g.data();
}

uint32_t* kwik_tilemap_grid_mut(int blob, int cells) {
    kwik_tilemap_grid(blob, cells);
    auto it = g_tilemap_cache.find(blob);
    if (it == g_tilemap_cache.end() || it->second.empty()) return nullptr;
    return it->second.data();
}

int kwik_sprite_add_file(const std::string& path, int imgnum, int xorig, int yorig) {
    (void)imgnum;
    std::FILE* f = std::fopen(kwik_resolve_read(path).c_str(), "rb");
    if (!f) return -1;
    std::vector<unsigned char> bytes;
    char tmp[8192];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) bytes.insert(bytes.end(), tmp, tmp + n);
    std::fclose(f);
    int w, h, ch;
    unsigned char* pixels =
        stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &ch, 4);
    if (!pixels) return -1;
    unsigned int tex = render_upload_texture(pixels, w, h);
    stbi_image_free(pixels);
    int img = kwik_register_dynamic_image(tex, w, h);
    KwikSprite s{};
    s.first_frame = img;
    s.frame_count = 1;
    s.origin_x = xorig;
    s.origin_y = yorig;
    s.speed = 1;
    s.speed_type = 1;
    s.bbox_left = 0;
    s.bbox_top = 0;
    s.bbox_right = w - 1;
    s.bbox_bottom = h - 1;
    s.width = w;
    s.height = h;
    s.sep_masks = 0;
    s.mask_blob = -1;
    s.name = "dyn_sprite";
    return kwik_register_dynamic_sprite(s);
}

static int frame_of(int spr, int sub, const KwikSprite** out) {
    const KwikSprite* s = kwik_sprite_at(spr);
    if (!s || s->frame_count <= 0) return -1;
    *out = s;
    return s->first_frame + ((sub % s->frame_count) + s->frame_count) % s->frame_count;
}

bool kwik_sprite_size(int spr, int& w, int& h) {
    const KwikSprite* s = kwik_sprite_at(spr);
    if (!s) return false;
    w = s->width;
    h = s->height;
    return true;
}

void kwik_draw_sprite_general(int spr, int sub, double x, double y, double xs, double ys,
                              double angle, unsigned int blend, double alpha) {
    const KwikSprite* s;
    int frame = frame_of(spr, sub, &s);
    if (frame < 0) return;
    LoadedImage& img = load_image(frame);
    if (!img.ok) {
        static long spr_fail_log = 40;
        if (spr_fail_log > 0) {
            --spr_fail_log;
            render_debug_log("sprite draw skipped (img not ok): spr=%d '%s' frame=%d xy=(%.1f,%.1f) "
                             "xs=%.2f ys=%.2f alpha=%.3f",
                             spr, s->name ? s->name : "?", frame, x, y, xs, ys, alpha);
        }
        return;
    }
    if (s->tile_repeat && angle == 0 && img.w > 0 && img.h > 0 &&
        (std::fabs(xs) != 1.0 || std::fabs(ys) != 1.0)) {
        double totw = img.w * std::fabs(xs), toth = img.h * std::fabs(ys);
        double ox = x - s->origin_x * std::fabs(xs);
        double oy = y - s->origin_y * std::fabs(ys);
        for (double py = 0; py < toth; py += img.h) {
            double ch = std::min((double)img.h, toth - py);
            for (double px = 0; px < totw; px += img.w) {
                double cw = std::min((double)img.w, totw - px);
                float u1 = (float)(cw / img.w), v1 = (float)(ch / img.h);
                render_draw_quad(img.tex, ox + px, oy + py, cw, ch, 0, 0, 1, 1, 0, 0, 0, u1, v1,
                                 blend, alpha);
            }
        }
        return;
    }
    double eff_w = img.w * std::fabs(xs), eff_h = img.h * std::fabs(ys);
    if (eff_w > 100 && eff_h > 75) {
        static long scaled_log = 60;
        if (scaled_log > 0) {
            --scaled_log;
            render_debug_log("sprite_ext_scaled spr=%d '%s' frame=%d xy=(%.1f,%.1f) img=(%d,%d) "
                             "xs=%.2f ys=%.2f eff=(%.1f,%.1f) blend=%06x alpha=%.3f angle=%.1f",
                             spr, s->name ? s->name : "?", frame, x, y, img.w, img.h, xs, ys, eff_w,
                             eff_h, blend, alpha, angle);
        }
    }
    render_draw_quad(img.tex, x, y, img.w, img.h, s->origin_x, s->origin_y, xs, ys, angle, 0, 0, 1,
                     1, blend, alpha);
}

void kwik_draw_sprite_part(int spr, int sub, double left, double top, double w, double h,
                           double x, double y, double xs, double ys, unsigned int blend,
                           double alpha) {
    const KwikSprite* s;
    int frame = frame_of(spr, sub, &s);
    if (frame < 0) return;
    LoadedImage& img = load_image(frame);
    if (!img.ok || img.w <= 0 || img.h <= 0) return;
    if (left < 0) { w += left; x -= left * xs; left = 0; }
    if (top < 0) { h += top; y -= top * ys; top = 0; }
    if (left + w > img.w) w = img.w - left;
    if (top + h > img.h) h = img.h - top;
    if (w <= 0 || h <= 0) return;
    float u0 = (float)(left / img.w), v0 = (float)(top / img.h);
    float u1 = (float)((left + w) / img.w), v1 = (float)((top + h) / img.h);
    render_draw_quad(img.tex, x, y, w, h, 0, 0, xs, ys, 0, u0, v0, u1, v1, blend, alpha);
}

void kwik_draw_sprite_stretched(int spr, int sub, double x, double y, double w, double h,
                                unsigned int blend, double alpha) {
    const KwikSprite* s;
    int frame = frame_of(spr, sub, &s);
    if (frame < 0) return;
    LoadedImage& img = load_image(frame);
    static long stretch_log = 60;
    if (stretch_log > 0 && std::fabs(w) > 100 && std::fabs(h) > 75) {
        --stretch_log;
        render_debug_log("sprite_stretched spr=%d '%s' frame=%d xy=(%.1f,%.1f) wh=(%.1f,%.1f) "
                         "blend=%06x alpha=%.3f img.ok=%d img.w=%d img.h=%d",
                         spr, s->name ? s->name : "?", frame, x, y, w, h, blend, alpha, img.ok,
                         img.w, img.h);
    }
    if (!img.ok || img.w <= 0 || img.h <= 0) return;
    render_draw_quad(img.tex, x, y, w, h, 0, 0, 1, 1, 0, 0, 0, 1, 1, blend, alpha);
}

void kwik_draw_sprite_tiled(int spr, int sub, double x, double y, double xs, double ys,
                            unsigned int blend, double alpha) {
    const KwikSprite* s;
    int frame = frame_of(spr, sub, &s);
    if (frame < 0) return;
    LoadedImage& img = load_image(frame);
    if (!img.ok || img.w <= 0 || img.h <= 0) return;
    double tw = img.w * xs, th = img.h * ys;
    if (tw <= 0.01 || th <= 0.01) return;
    Camera& cam = g_cameras[g_view_camera[0]];
    double vx1 = cam.x + cam.w, vy1 = cam.y + cam.h;
    double sx = x - std::ceil((x - cam.x) / tw) * tw;
    double sy = y - std::ceil((y - cam.y) / th) * th;
    for (double py = sy; py < vy1; py += th)
        for (double px = sx; px < vx1; px += tw)
            render_draw_quad(img.tex, px, py, img.w, img.h, 0, 0, xs, ys, 0, 0, 0, 1, 1, blend,
                             alpha);
}

void draw_self_instance(Instance* inst) {
    if (!inst) return;
    static const int ID_sprite_index = kwik_intern_varname("sprite_index");
    static const int ID_image_index = kwik_intern_varname("image_index");
    static const int ID_image_xscale = kwik_intern_varname("image_xscale");
    static const int ID_image_yscale = kwik_intern_varname("image_yscale");
    static const int ID_image_angle = kwik_intern_varname("image_angle");
    static const int ID_image_blend = kwik_intern_varname("image_blend");
    static const int ID_image_alpha = kwik_intern_varname("image_alpha");
    auto gv = [&](int id, double d) {
        auto it = inst->vars.find(id);
        return it == inst->vars.end() ? d : (double)it->second;
    };
    int spr = (int)gv(ID_sprite_index, -1);
    if (spr < 0) return;
    kwik_draw_sprite_general(spr, (int)gv(ID_image_index, 0), inst->x, inst->y,
                             gv(ID_image_xscale, 1), gv(ID_image_yscale, 1), gv(ID_image_angle, 0),
                             (unsigned int)gv(ID_image_blend, 16777215), gv(ID_image_alpha, 1));
}

struct RtGlyph {
    int ch;
    int image;
    float u0, v0, u1, v1;
    double w, h;
    double shift, offset;
};

struct RtFont {
    std::vector<RtGlyph> glyphs;
    int index[256];
    double line_height = 16;
};

static void build_glyph_index(RtFont& f) {
    for (int i = 0; i < 256; ++i) f.index[i] = -1;
    for (size_t i = 0; i < f.glyphs.size(); ++i) {
        int ch = f.glyphs[i].ch;
        if (ch >= 0 && ch < 256 && f.index[ch] < 0) f.index[ch] = (int)i;
    }
}

static std::vector<RtFont> g_rt_fonts;
static std::vector<int> g_asset_font_map;
static int g_cur_font = -1;
static bool g_fonts_built = false;

static void build_fonts() {
    if (g_fonts_built) return;
    g_fonts_built = true;
    g_asset_font_map.resize(g_font_count, -1);
    for (int f = 0; f < g_font_count; ++f) {
        const KwikFont& kf = g_fonts[f];
        RtFont rf;
        LoadedImage& atlas = load_image(kf.atlas_image);
        double maxh = 1;
        for (int gi = 0; gi < kf.glyph_count; ++gi) {
            const KwikGlyph& g = g_glyphs[kf.glyph_start + gi];
            RtGlyph rg;
            rg.ch = g.ch;
            rg.image = kf.atlas_image;
            if (atlas.ok && atlas.w > 0 && atlas.h > 0) {
                rg.u0 = (float)g.x / atlas.w;
                rg.v0 = (float)g.y / atlas.h;
                rg.u1 = (float)(g.x + g.w) / atlas.w;
                rg.v1 = (float)(g.y + g.h) / atlas.h;
            } else {
                rg.u0 = rg.v0 = 0;
                rg.u1 = rg.v1 = 1;
            }
            rg.w = g.w;
            rg.h = g.h;
            rg.shift = g.shift;
            rg.offset = g.offset;
            if (g.h > maxh) maxh = g.h;
            rf.glyphs.push_back(rg);
        }
        rf.line_height = kf.size > 0 ? kf.size : maxh;
        build_glyph_index(rf);
        g_asset_font_map[f] = (int)g_rt_fonts.size();
        g_rt_fonts.push_back(std::move(rf));
    }
}

int kwik_font_for_asset(int font_asset) {
    build_fonts();
    if (font_asset < 0 || font_asset >= (int)g_asset_font_map.size()) return -1;
    return g_asset_font_map[font_asset];
}

int kwik_font_add_sprite(int spr, const std::string& mapping, bool prop, int sep) {
    build_fonts();
    const KwikSprite* sp = kwik_sprite_at(spr);
    if (!sp) return -1;
    const KwikSprite& s = *sp;
    RtFont rf;
    double maxh = 1;
    for (size_t i = 0; i < mapping.size() && (int)i < s.frame_count; ++i) {
        int frame = s.first_frame + (int)i;
        LoadedImage& img = load_image(frame);
        RtGlyph rg;
        rg.ch = (unsigned char)mapping[i];
        rg.image = frame;
        rg.u0 = rg.v0 = 0;
        rg.u1 = rg.v1 = 1;
        rg.w = img.ok ? img.w : s.width;
        rg.h = img.ok ? img.h : s.height;
        rg.shift = (prop ? rg.w : (double)s.width) + sep;
        rg.offset = 0;
        if (rg.h > maxh) maxh = rg.h;
        rf.glyphs.push_back(rg);
    }
    rf.line_height = maxh;
    build_glyph_index(rf);
    g_rt_fonts.push_back(std::move(rf));
    return (int)g_rt_fonts.size() - 1;
}

void kwik_set_font_rt(int rt_font) {
    build_fonts();
    g_cur_font = (rt_font >= 0 && rt_font < (int)g_rt_fonts.size()) ? rt_font : -1;
}

int kwik_get_font_rt() { return g_cur_font; }

static const RtGlyph* find_glyph(const RtFont& f, int ch) {
    if (ch >= 0 && ch < 256) {
        int i = f.index[ch];
        return i < 0 ? nullptr : &f.glyphs[i];
    }
    for (const RtGlyph& g : f.glyphs)
        if (g.ch == ch) return &g;
    return nullptr;
}

static double line_width(const RtFont& f, const std::string& text, size_t a, size_t b) {
    double w = 0;
    for (size_t i = a; i < b; ++i) {
        const RtGlyph* g = find_glyph(f, (unsigned char)text[i]);
        if (g) w += g->shift;
    }
    return w;
}

static void split_lines(const std::string& text, std::vector<std::pair<size_t, size_t>>& lines) {
    size_t ls = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            size_t le = i;
            if (le > ls && text[le - 1] == '\r') le--;
            lines.push_back({ls, le});
            ls = i + 1;
        }
    }
}

double kwik_string_width(const std::string& s) {
    build_fonts();
    if (g_cur_font < 0) return 0;
    const RtFont& f = g_rt_fonts[g_cur_font];
    std::vector<std::pair<size_t, size_t>> lines;
    split_lines(s, lines);
    double best = 0;
    for (auto& ln : lines) best = std::max(best, line_width(f, s, ln.first, ln.second));
    return best;
}

double kwik_string_height(const std::string& s) {
    build_fonts();
    if (g_cur_font < 0) return 0;
    const RtFont& f = g_rt_fonts[g_cur_font];
    std::vector<std::pair<size_t, size_t>> lines;
    split_lines(s, lines);
    return f.line_height * (double)lines.size();
}

void kwik_draw_text_ext_rt(double x, double y, const std::string& text, double sep, double wrapw,
                           double xs, double ys, double angle) {
    build_fonts();
    if (g_cur_font < 0) return;
    const RtFont& f = g_rt_fonts[g_cur_font];
    std::string wrapped;
    if (wrapw > 0) {
        double linew = 0;
        size_t last_space = std::string::npos;
        double width_at_space = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            wrapped.push_back(c);
            if (c == '\n') {
                linew = 0;
                last_space = std::string::npos;
                continue;
            }
            const RtGlyph* g = find_glyph(f, (unsigned char)c);
            double adv = g ? g->shift : 0;
            if (c == ' ') {
                last_space = wrapped.size() - 1;
                width_at_space = linew;
            }
            linew += adv;
            if (linew > wrapw && last_space != std::string::npos) {
                wrapped[last_space] = '\n';
                linew -= width_at_space + (g && text[last_space] ? 0 : 0);
                linew = linew - width_at_space;
                last_space = std::string::npos;
            }
        }
    } else {
        wrapped = text;
    }
    double saved = -1;
    if (sep > 0) {
        saved = g_rt_fonts[g_cur_font].line_height;
        g_rt_fonts[g_cur_font].line_height = sep;
    }
    kwik_draw_text_rt(x, y, wrapped, xs, ys, angle);
    if (saved >= 0) g_rt_fonts[g_cur_font].line_height = saved;
}

void kwik_draw_text_rt(double x, double y, const std::string& text, double xs, double ys,
                       double angle) {
    build_fonts();
    static int dbg_left = std::getenv("KWIK_DEBUG_TEXT") ? 40 : 0;
    if (dbg_left > 0 && !text.empty()) {
        --dbg_left;
        int found = 0;
        if (g_cur_font >= 0)
            for (char c : text)
                if (find_glyph(g_rt_fonts[g_cur_font], (unsigned char)c)) ++found;
        std::fprintf(stderr, "[text] font=%d at(%.0f,%.0f) glyphs=%d/%zu \"%.40s\"\n", g_cur_font,
                     x, y, found, text.size(), text.c_str());
    }
    if (g_cur_font < 0) return;
    const RtFont& f = g_rt_fonts[g_cur_font];
    std::vector<std::pair<size_t, size_t>> lines;
    split_lines(text, lines);

    int halign = render_get_halign(), valign = render_get_valign();
    double line_h = f.line_height * ys;
    double total_h = line_h * (double)lines.size();
    double oy = 0;
    if (valign == 1) oy -= total_h * 0.5;
    else if (valign == 2) oy -= total_h;

    double rad = angle * 3.14159265358979 / 180.0;
    double ca = std::cos(rad), sa = std::sin(rad);
    unsigned int col = render_get_color();
    double alpha = render_get_alpha();

    unsigned int batch_tex = 0;
    std::vector<GlyphQuad> batch;
    auto flush_batch = [&]() {
        if (!batch.empty()) render_draw_glyphs_colored(batch_tex, batch.data(),
                                                        (int)batch.size(), col, alpha);
        batch.clear();
    };

    for (size_t li = 0; li < lines.size(); ++li) {
        double ox = 0;
        double w = line_width(f, text, lines[li].first, lines[li].second) * xs;
        if (halign == 1) ox -= w * 0.5;
        else if (halign == 2) ox -= w;
        double liney = oy + (double)li * line_h;
        double pen = ox;
        for (size_t i = lines[li].first; i < lines[li].second; ++i) {
            const RtGlyph* g = find_glyph(f, (unsigned char)text[i]);
            if (!g) continue;
            if (g->w > 0 && g->h > 0) {
                LoadedImage& img = load_image(g->image);
                if (img.ok) {
                    double lx = pen + g->offset * xs;
                    double ly = liney;
                    if (angle == 0.0) {
                        if (img.tex != batch_tex) flush_batch();
                        batch_tex = img.tex;
                        batch.push_back({x + lx, y + ly, g->w * xs, g->h * ys, g->u0, g->v0, g->u1,
                                        g->v1});
                    } else {
                        double gx = x + lx * ca + ly * sa;
                        double gy = y - lx * sa + ly * ca;
                        render_draw_quad(img.tex, gx, gy, g->w, g->h, 0, 0, xs, ys, angle, g->u0,
                                         g->v0, g->u1, g->v1, col, alpha);
                    }
                }
            }
            pen += g->shift * xs;
        }
    }
    flush_batch();
}

}
