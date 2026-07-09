#include "render.h"
#include "engine_internal.h"

#include <3ds.h>
#include <citro3d.h>
#include <tex3ds.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <strings.h>
#include <vector>

extern "C" {
extern const u8 vshader_shbin[];
extern const u8 vshader_shbin_end[];
extern const u32 vshader_shbin_size;

u32 __stacksize__ = 4 * 1024 * 1024;
}

namespace gml {

static FILE* g_klog_file = nullptr;

static void klog_open() {
    if (g_klog_file) return;
    std::string path = kwik_save_path("kwik_3ds_log.txt");
    if (path.empty() || path == "kwik_3ds_log.txt") path = "sdmc:/3ds/kwik_3ds_log.txt";
    g_klog_file = std::fopen(path.c_str(), "w");
}

static void klog(const char* fmt, ...) {
    klog_open();
    if (!g_klog_file) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_klog_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_klog_file);
    std::fflush(g_klog_file);
}

void render_debug_log(const char* fmt, ...) {
    klog_open();
    if (!g_klog_file) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_klog_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_klog_file);
    std::fflush(g_klog_file);
}

enum { MTX_MODELVIEW = 0, MTX_PROJECTION = 1, MTX_TEXTURE = 2, MTX_MODE_COUNT = 3 };

static DVLB_s* g_vshader_dvlb = nullptr;
static shaderProgram_s g_program;
static C3D_MtxStack g_mtx_stacks[MTX_MODE_COUNT];
static int g_uloc_mtx[MTX_MODE_COUNT];
static C3D_RenderTarget* g_screen_rt = nullptr;

static bool g_inited = false;

static int g_gui_w = 640, g_gui_h = 480;
static int g_room_w = 640, g_room_h = 480;
static double g_view_x = 0, g_view_y = 0, g_view_w = 640, g_view_h = 480;
static bool g_fog_on = false;
static unsigned char g_fog_col[3] = {0, 0, 0};

static unsigned int g_color_bgr = 0xFFFFFF;
static float g_alpha = 1.0f;
static int g_halign = 0;
static int g_valign = 0;

static bool g_keys_now[512] = {false};
static bool g_keys_prev[512] = {false};
static bool g_mouse_now[3] = {false};
static bool g_mouse_prev[3] = {false};
static bool g_touch_down = false;
static float g_touch_x = 0, g_touch_y = 0;

static double g_last_time = 0.0;
static double g_dt = 0.0;

static PrintConsole g_bottom_console;
static double g_fps_disp = 0.0;
static double g_fps_accum = 0.0;
static int g_fps_frames = 0;

static int g_blend_src = 5, g_blend_dst = 6, g_blend_asrc = 5, g_blend_adst = 6;
static bool g_colormask[4] = {true, true, true, true};

struct RtTexture {
    C3D_Tex tex{};
    C3D_RenderTarget* rt = nullptr;
    int w = 0, h = 0;
    int pw = 0, ph = 0;
    bool alive = false;
    GPU_TEXCOLOR fmt = GPU_RGBA8;
};
static std::vector<std::unique_ptr<RtTexture>> g_textures;

static RtTexture* tex_of(unsigned int id) {
    if (id == 0 || (size_t)id > g_textures.size()) return nullptr;
    RtTexture* t = g_textures[id - 1].get();
    return t->alive ? t : nullptr;
}

static unsigned int tex_alloc_slot() {
    for (size_t i = 0; i < g_textures.size(); ++i)
        if (!g_textures[i]->alive) return (unsigned int)i + 1;
    g_textures.push_back(std::make_unique<RtTexture>());
    return (unsigned int)g_textures.size();
}

struct EvictEntry {
    unsigned int tex_id;
    TextureEvictFn cb;
    void* user;
    long last_used;
    long touched_frame;
};
static std::vector<EvictEntry> g_evictable;
static long g_evict_clock = 0;
static long g_evict_frame_gen = 0;

static void evict_bump_frame() { ++g_evict_frame_gen; }

void render_register_evictable(unsigned int tex_id, TextureEvictFn on_evict, void* user_data) {
    g_evictable.push_back({tex_id, on_evict, user_data, ++g_evict_clock, g_evict_frame_gen});
}

void render_touch_texture(unsigned int tex_id) {
    ++g_evict_clock;
    for (auto& e : g_evictable)
        if (e.tex_id == tex_id) {
            e.last_used = g_evict_clock;
            e.touched_frame = g_evict_frame_gen;
            return;
        }
}

static bool evict_oldest_texture() {
    size_t oldest = (size_t)-1;
    for (size_t i = 0; i < g_evictable.size(); ++i) {
        if (g_evictable[i].touched_frame == g_evict_frame_gen) continue;
        if (oldest == (size_t)-1 || g_evictable[i].last_used < g_evictable[oldest].last_used)
            oldest = i;
    }
    if (oldest == (size_t)-1) return false;
    EvictEntry e = g_evictable[oldest];
    g_evictable.erase(g_evictable.begin() + oldest);
    RtTexture* t = tex_of(e.tex_id);
    if (t) {
        if (t->rt) { C3D_RenderTargetDelete(t->rt); t->rt = nullptr; }
        C3D_TexDelete(&t->tex);
        t->alive = false;
    }
    if (e.cb) e.cb(e.user);
    return true;
}

struct RtSurface {
    unsigned int tex_id = 0;
    int w = 0, h = 0;
    bool alive = false;
};
static std::vector<RtSurface> g_surfaces;
static std::vector<int> g_target_stack;

static unsigned int g_app_tex = 0;
static unsigned int g_white_tex = 0;
static int g_fbo_w = 0, g_fbo_h = 0;

struct ViewXf {
    double ox = 0, oy = 0;
    double sx = 1, sy = 1;
};
static ViewXf g_xf;
static std::vector<ViewXf> g_xf_stack;

static float tx(double x) { return (float)((x - g_xf.ox) * g_xf.sx); }
static float ty(double y) { return (float)((y - g_xf.oy) * g_xf.sy); }

static int next_pot(int v) {
    int p = 8;
    while (p < v) p <<= 1;
    return p;
}

static const int kMaxTexDim = 1024;

static double time_seconds() { return osGetTime() / 1000.0; }

static GPU_BLENDFACTOR gm_blend_factor(int f) {
    switch (f) {
        case 1: return GPU_ZERO;
        case 2: return GPU_ONE;
        case 3: return GPU_SRC_COLOR;
        case 4: return GPU_ONE_MINUS_SRC_COLOR;
        case 5: return GPU_SRC_ALPHA;
        case 6: return GPU_ONE_MINUS_SRC_ALPHA;
        case 7: return GPU_DST_ALPHA;
        case 8: return GPU_ONE_MINUS_DST_ALPHA;
        case 9: return GPU_DST_COLOR;
        case 10: return GPU_ONE_MINUS_DST_COLOR;
        case 11: return GPU_ONE;
        default: return GPU_ONE;
    }
}

static void apply_blend_state() {
    GPU_BLENDFACTOR cs = gm_blend_factor(g_blend_src);
    GPU_BLENDFACTOR cd = gm_blend_factor(g_blend_dst);
    GPU_BLENDFACTOR as = gm_blend_factor(g_blend_asrc);
    GPU_BLENDFACTOR ad = gm_blend_factor(g_blend_adst);
    bool rgb = g_colormask[0] || g_colormask[1] || g_colormask[2];
    if (!rgb) {
        cs = GPU_ZERO;
        cd = GPU_ONE;
    }
    if (!g_colormask[3]) {
        as = GPU_ZERO;
        ad = GPU_ONE;
    }
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, cs, cd, as, ad);
}

static u32 pack_clear_rgba5551(u8 r, u8 g, u8 b, u8 a) {
    u32 r5 = ((u32)r * 31 + 127) / 255;
    u32 g5 = ((u32)g * 31 + 127) / 255;
    u32 b5 = ((u32)b * 31 + 127) / 255;
    u32 a1 = a >= 128 ? 1u : 0u;
    return (r5 << 11) | (g5 << 6) | (b5 << 1) | a1;
}

static u32 pack_clear_color(GPU_TEXCOLOR fmt, u8 r, u8 g, u8 b, u8 a) {
    if (fmt == GPU_RGBA5551) return pack_clear_rgba5551(r, g, b, a);
    return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | a;
}

struct Vtx {
    float x, y;
    float u, v;
    u8 r, g, b, a;
};

static long g_draw_calls_this_frame = 0;
static long g_frame_no = 0;

static u8* g_frame_arena = nullptr;
static size_t g_frame_arena_size = 0;
static size_t g_frame_arena_offset = 0;

static void* arena_alloc(size_t bytes) {
    size_t aligned = (bytes + 7) & ~(size_t)7;
    if (g_frame_arena_offset + aligned > g_frame_arena_size) {
        klog("frame arena exhausted (offset=%zu size=%zu req=%zu), wrapping",
             g_frame_arena_offset, g_frame_arena_size, bytes);
        g_frame_arena_offset = 0;
    }
    void* p = g_frame_arena + g_frame_arena_offset;
    g_frame_arena_offset += aligned;
    return p;
}

static void submit(const Vtx* verts, int nverts, const u16* idx, int nidx, C3D_Tex* tex) {
    if (nverts <= 0 || nidx <= 0) return;
    g_draw_calls_this_frame++;
    apply_blend_state();
    C3D_TexBind(0, tex);

    Vtx* buf = (Vtx*)arena_alloc(sizeof(Vtx) * nverts);
    std::memcpy(buf, verts, sizeof(Vtx) * nverts);
    u16* ibuf = (u16*)arena_alloc(sizeof(u16) * nidx);
    std::memcpy(ibuf, idx, sizeof(u16) * nidx);

    C3D_AttrInfo* attrInfo = C3D_GetAttrInfo();
    AttrInfo_Init(attrInfo);
    AttrInfo_AddLoader(attrInfo, 0, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attrInfo, 1, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attrInfo, 2, GPU_UNSIGNED_BYTE, 4);

    C3D_BufInfo* bufInfo = C3D_GetBufInfo();
    BufInfo_Init(bufInfo);
    BufInfo_Add(bufInfo, buf, sizeof(Vtx), 3, 0x210);

    for (int i = 0; i < MTX_MODE_COUNT; ++i) MtxStack_Update(&g_mtx_stacks[i]);
    C3D_DrawElements(GPU_TRIANGLES, nidx, C3D_UNSIGNED_SHORT, ibuf);
}

static u8 vcol_component(unsigned int bgr, int shift) { return (u8)((bgr >> shift) & 0xFF); }

static void vcol(unsigned int bgr, double alpha, u8& r, u8& g, u8& b, u8& a) {
    if (g_fog_on) {
        r = g_fog_col[0];
        g = g_fog_col[1];
        b = g_fog_col[2];
    } else {
        r = vcol_component(bgr, 0);
        g = vcol_component(bgr, 8);
        b = vcol_component(bgr, 16);
    }
    a = (u8)std::lround(std::clamp(alpha, 0.0, 1.0) * 255.0);
}


bool render_app_surface_available() { return g_app_tex != 0; }
unsigned int render_app_texture() { return g_app_tex; }
int render_app_width() { return g_fbo_w > 0 ? g_fbo_w : g_gui_w; }
int render_app_height() { return g_fbo_h > 0 ? g_fbo_h : g_gui_h; }

static unsigned int create_texture(int w, int h, bool as_target) {
    unsigned int id = tex_alloc_slot();
    RtTexture& t = *g_textures[id - 1];
    t.alive = false;
    t.rt = nullptr;
    t.w = w;
    t.h = h;
    t.pw = next_pot(w);
    t.ph = next_pot(h);
    if (t.pw > kMaxTexDim || t.ph > kMaxTexDim) {
        klog("create_texture: clamping oversized request w=%d h=%d pw=%d ph=%d to PICA200 max %d",
             w, h, t.pw, t.ph, kMaxTexDim);
        if (t.pw > kMaxTexDim) t.pw = kMaxTexDim;
        if (t.ph > kMaxTexDim) t.ph = kMaxTexDim;
        if (t.w > t.pw) t.w = t.pw;
        if (t.h > t.ph) t.h = t.ph;
    }
    GPU_TEXCOLOR fmt = GPU_RGBA8;
    bool ok = false;
    int evict_budget = (int)g_evictable.size() + 1;
    for (;;) {
        t.tex = C3D_Tex{};
        fmt = GPU_RGBA8;
        ok = C3D_TexInitWithParams(&t.tex, nullptr,
                                    (C3D_TexInitParams){(u16)t.pw, (u16)t.ph, 0, fmt, GPU_TEX_2D,
                                                        as_target});
        if (!ok && as_target) {
            klog("C3D_TexInitWithParams FAILED at RGBA8 w=%d h=%d pw=%d ph=%d, retrying RGBA5551",
                 w, h, t.pw, t.ph);
            t.tex = C3D_Tex{};
            fmt = GPU_RGBA5551;
            ok = C3D_TexInitWithParams(&t.tex, nullptr,
                                       (C3D_TexInitParams){(u16)t.pw, (u16)t.ph, 0, fmt, GPU_TEX_2D,
                                                           as_target});
        }
        if (ok || evict_budget-- <= 0 || !evict_oldest_texture()) break;
        klog("create_texture: evicted a texture to free memory, retrying w=%d h=%d", w, h);
    }
    if (!ok) {
        klog("C3D_TexInitWithParams FAILED w=%d h=%d pw=%d ph=%d target=%d", w, h, t.pw, t.ph,
             as_target ? 1 : 0);
        return 0;
    }
    t.fmt = fmt;
    C3D_TexSetWrap(&t.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&t.tex, GPU_NEAREST, GPU_NEAREST);
    if (as_target) {
        t.rt = C3D_RenderTargetCreateFromTex(&t.tex, GPU_TEXFACE_2D, 0, -1);
        if (!t.rt) klog("C3D_RenderTargetCreateFromTex FAILED w=%d h=%d pw=%d ph=%d", w, h, t.pw, t.ph);
    }
    t.alive = true;
    return id;
}

int render_surface_create(int w, int h) {
    if (w <= 0 || h <= 0) return -1;
    unsigned int tid = create_texture(w, h, true);
    if (!tid) return -1;
    RtTexture* t = tex_of(tid);
    RtSurface sf;
    sf.tex_id = tid;
    sf.w = t ? t->w : w;
    sf.h = t ? t->h : h;
    sf.alive = true;
    for (size_t i = 0; i < g_surfaces.size(); ++i)
        if (!g_surfaces[i].alive) {
            g_surfaces[i] = sf;
            return (int)i + 1;
        }
    g_surfaces.push_back(sf);
    return (int)g_surfaces.size();
}

static RtSurface* surf_of(int id) {
    int i = id - 1;
    if (i < 0 || (size_t)i >= g_surfaces.size() || !g_surfaces[i].alive) return nullptr;
    return &g_surfaces[i];
}

bool render_surface_exists(int id) {
    if (id == 0) return g_app_tex != 0;
    return surf_of(id) != nullptr;
}

void render_surface_free(int id) {
    RtSurface* sf = surf_of(id);
    if (!sf) return;
    RtTexture* t = tex_of(sf->tex_id);
    if (t) {
        if (t->rt) C3D_RenderTargetDelete(t->rt);
        C3D_TexDelete(&t->tex);
        t->alive = false;
        t->rt = nullptr;
    }
    sf->alive = false;
}

unsigned int render_surface_texture(int id) {
    if (id == 0) return g_app_tex;
    RtSurface* sf = surf_of(id);
    return sf ? sf->tex_id : 0;
}

int render_surface_width(int id) {
    if (id == 0) return render_app_width();
    RtSurface* sf = surf_of(id);
    return sf ? sf->w : 0;
}

int render_surface_height(int id) {
    if (id == 0) return render_app_height();
    RtSurface* sf = surf_of(id);
    return sf ? sf->h : 0;
}

static void set_surface_ortho(int w, int h) {
    C3D_Mtx* p = MtxStack_Cur(&g_mtx_stacks[MTX_PROJECTION]);
    Mtx_Ortho(p, 0, (float)w, (float)h, 0, -1.0f, 1.0f, true);
    g_xf.ox = 0;
    g_xf.oy = 0;
    g_xf.sx = 1;
    g_xf.sy = 1;
}

bool render_surface_set_target(int id) {
    RtSurface* sf = surf_of(id);
    unsigned int tid = id == 0 ? g_app_tex : (sf ? sf->tex_id : 0);
    RtTexture* t = tex_of(tid);
    if (!t || !t->rt) return false;
    g_target_stack.push_back(id);
    g_xf_stack.push_back(g_xf);
    C3D_FrameDrawOn(t->rt);
    C3D_SetViewport(0, 0, t->w, t->h);
    set_surface_ortho(t->w, t->h);
    return true;
}

void render_surface_reset_target() {
    if (g_target_stack.empty()) return;
    g_target_stack.pop_back();
    if (!g_xf_stack.empty()) {
        g_xf = g_xf_stack.back();
        g_xf_stack.pop_back();
    }
    int prev = g_target_stack.empty() ? 0 : g_target_stack.back();
    unsigned int tid = prev == 0 ? g_app_tex : render_surface_texture(prev);
    RtTexture* t = tex_of(tid);
    if (t && t->rt) {
        C3D_FrameDrawOn(t->rt);
        C3D_SetViewport(0, 0, t->w, t->h);
    }
}

static bool readback_rgba(C3D_RenderTarget* rt, int w, int h, GPU_TEXCOLOR fmt,
                          unsigned char* rgba_out) {
    if (!rt) return false;
    u32 in_fmt = fmt == GPU_RGBA5551 ? GX_TRANSFER_FMT_RGB5A1 : GX_TRANSFER_FMT_RGBA8;
    size_t bpp = fmt == GPU_RGBA5551 ? 2 : 4;
    u32* linear_out = (u32*)linearAlloc((size_t)w * h * 4);
    if (!linear_out) return false;
    GSPGPU_FlushDataCache(rt->frameBuf.colorBuf, (size_t)w * h * bpp);
    C3D_SyncDisplayTransfer((u32*)rt->frameBuf.colorBuf, GX_BUFFER_DIM(w, h), linear_out,
                            GX_BUFFER_DIM(w, h),
                            GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(0) |
                                GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(in_fmt) |
                                GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    gspWaitForPPF();
    std::memcpy(rgba_out, linear_out, (size_t)w * h * 4);
    linearFree(linear_out);
    return true;
}

bool render_surface_getpixel(int id, int x, int y, unsigned char* rgba_out) {
    unsigned int tid = id == 0 ? g_app_tex : render_surface_texture(id);
    RtTexture* t = tex_of(tid);
    if (!t || !t->rt) return false;
    std::vector<unsigned char> buf((size_t)t->w * t->h * 4);
    if (!readback_rgba(t->rt, t->w, t->h, t->fmt, buf.data())) return false;
    if (x < 0 || y < 0 || x >= t->w || y >= t->h) return false;
    std::memcpy(rgba_out, &buf[((size_t)y * t->w + x) * 4], 4);
    return true;
}

void render_surface_clear(unsigned int bgr, double alpha) {
    int cur = g_target_stack.empty() ? 0 : g_target_stack.back();
    unsigned int tid = cur == 0 ? g_app_tex : render_surface_texture(cur);
    RtTexture* t = tex_of(tid);
    if (!t || !t->rt) return;
    u8 r, g, b, a;
    vcol(bgr, alpha, r, g, b, a);
    C3D_RenderTargetClear(t->rt, C3D_CLEAR_COLOR, pack_clear_color(t->fmt, r, g, b, a), 0);
}

bool render_surface_snapshot(int id, int x, int y, int w, int h, unsigned char* rgba_out) {
    unsigned int tid = id == 0 ? g_app_tex : render_surface_texture(id);
    RtTexture* t = tex_of(tid);
    if (!t || !t->rt) return false;
    std::vector<unsigned char> buf((size_t)t->w * t->h * 4);
    if (!readback_rgba(t->rt, t->w, t->h, t->fmt, buf.data())) return false;
    for (int row = 0; row < h; ++row) {
        int sy = y + row;
        if (sy < 0 || sy >= t->h) continue;
        for (int col = 0; col < w; ++col) {
            int sx = x + col;
            if (sx < 0 || sx >= t->w) continue;
            std::memcpy(&rgba_out[(row * w + col) * 4], &buf[((size_t)sy * t->w + sx) * 4], 4);
        }
    }
    return true;
}

bool render_app_snapshot(int x, int y, int w, int h, unsigned char* rgba_out) {
    return render_surface_snapshot(0, x, y, w, h, rgba_out);
}

static void swizzle_upload_abgr8(const unsigned char* src, int src_w, int src_h, u32* dst,
                                 int pot_w) {
    for (int y = 0; y < src_h; ++y) {
        u32 base_offset_y = ((u32)y & ~7u) * (u32)pot_w;
        for (int x = 0; x < src_w; ++x) {
            u32 mx = (u32)x & 7, my = (u32)y & 7;
            mx = (mx | (mx << 4)) & 0x0F;
            mx = (mx | (mx << 2)) & 0x33;
            mx = (mx | (mx << 1)) & 0x55;
            my = (my | (my << 4)) & 0x0F;
            my = (my | (my << 2)) & 0x33;
            my = (my | (my << 1)) & 0x55;
            u32 morton = mx | (my << 1);
            u32 coarse_x = ((u32)x & ~7u) * 8;
            const unsigned char* p = &src[((size_t)y * src_w + x) * 4];
            dst[base_offset_y + coarse_x + morton] =
                ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
        }
    }
}

unsigned int render_upload_texture(const unsigned char* rgba, int w, int h) {
    std::vector<unsigned char> scaled;
    const unsigned char* src = rgba;
    int sw = w, sh = h;
    if (w > kMaxTexDim || h > kMaxTexDim) {
        sw = w > kMaxTexDim ? kMaxTexDim : w;
        sh = h > kMaxTexDim ? kMaxTexDim : h;
        klog("render_upload_texture: downscaling %dx%d -> %dx%d (3DS texture size limit)", w, h,
             sw, sh);
        scaled.resize((size_t)sw * sh * 4);
        for (int y = 0; y < sh; ++y) {
            int sy = (int)((int64_t)y * h / sh);
            if (sy >= h) sy = h - 1;
            for (int x = 0; x < sw; ++x) {
                int sx = (int)((int64_t)x * w / sw);
                if (sx >= w) sx = w - 1;
                std::memcpy(&scaled[((size_t)y * sw + x) * 4], &rgba[((size_t)sy * w + sx) * 4],
                            4);
            }
        }
        src = scaled.data();
    }
    unsigned int id = create_texture(sw, sh, false);
    if (!id) return 0;
    RtTexture& t = *g_textures[id - 1];
    size_t padded_size = (size_t)t.pw * t.ph * 4;
    unsigned char* padded = (unsigned char*)linearAlloc(padded_size);
    if (!padded) {
        klog("linearAlloc FAILED for texture upload staging buffer size=%zu", padded_size);
        return id;
    }
    std::memset(padded, 0, padded_size);
    int upload_w = sw < t.pw ? sw : t.pw;
    int upload_h = sh < t.ph ? sh : t.ph;
    swizzle_upload_abgr8(src, upload_w, upload_h, (u32*)padded, t.pw);
    C3D_TexUpload(&t.tex, padded);
    C3D_TexFlush(&t.tex);
    linearFree(padded);
    return id;
}

unsigned int render_upload_texture_t3x(const unsigned char* data, unsigned int size) {
    unsigned int id = tex_alloc_slot();
    RtTexture& t = *g_textures[id - 1];
    t.alive = false;
    t.rt = nullptr;
    Tex3DS_Texture t3x = nullptr;
    int evict_budget = (int)g_evictable.size() + 1;
    for (;;) {
        t.tex = C3D_Tex{};
        t3x = Tex3DS_TextureImport(data, size, &t.tex, nullptr, false);
        if (t3x || evict_budget-- <= 0 || !evict_oldest_texture()) break;
        klog("render_upload_texture_t3x: evicted a texture to free memory, retrying size=%u", size);
    }
    if (!t3x) {
        klog("Tex3DS_TextureImport FAILED size=%u", size);
        return 0;
    }
    const Tex3DS_SubTexture* sub = Tex3DS_GetSubTexture(t3x, 0);
    t.pw = (int)t.tex.width;
    t.ph = (int)t.tex.height;
    t.w = sub ? sub->width : t.pw;
    t.h = sub ? sub->height : t.ph;
    t.fmt = t.tex.fmt;
    Tex3DS_TextureFree(t3x);
    C3D_TexSetWrap(&t.tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexSetFilter(&t.tex, GPU_NEAREST, GPU_NEAREST);
    t.alive = true;
    return id;
}


static void draw_tex_quad(RtTexture* t, const float* xs, const float* ys, float u0, float v0,
                          float u1, float v1, unsigned int blend_bgr, double alpha) {
    float su = t->w > 0 ? (float)t->w / t->pw : 1.0f;
    float sv = t->h > 0 ? (float)t->h / t->ph : 1.0f;
    if (t->rt) {
        C3D_TexFlush(&t->tex);
        v0 = 1.0f - v0;
        v1 = 1.0f - v1;
        u0 *= su;
        u1 *= su;
        v0 *= sv;
        v1 *= sv;
    } else {
        u0 *= su;
        u1 *= su;
        v0 *= sv;
        v1 *= sv;
        v0 = 1.0f - v0;
        v1 = 1.0f - v1;
    }
    u8 r, g, b, a;
    vcol(blend_bgr, alpha, r, g, b, a);
    float us[4] = {u0, u1, u1, u0};
    float vs[4] = {v0, v0, v1, v1};
    Vtx v[4];
    for (int i = 0; i < 4; ++i) {
        v[i].x = xs[i];
        v[i].y = ys[i];
        v[i].u = us[i];
        v[i].v = vs[i];
        v[i].r = r;
        v[i].g = g;
        v[i].b = b;
        v[i].a = a;
    }
    const u16 idx[6] = {0, 1, 2, 0, 2, 3};
    submit(v, 4, idx, 6, &t->tex);
}

void render_draw_quad(unsigned int tex, double x, double y, double dw, double dh, double origin_x,
                      double origin_y, double xscale, double yscale, double angle_deg, float u0,
                      float v0, float u1, float v1, unsigned int blend_bgr, double alpha) {
    RtTexture* t = tex_of(tex);
    if (!t) return;
    render_touch_texture(tex);
    double rad = angle_deg * 3.14159265358979323846 / 180.0;
    double c = std::cos(rad), s = std::sin(rad);
    double lx[4] = {0.0, dw, dw, 0.0};
    double ly[4] = {0.0, 0.0, dh, dh};
    float vx[4], vy[4];
    for (int i = 0; i < 4; ++i) {
        double px = (lx[i] - origin_x) * xscale;
        double py = (ly[i] - origin_y) * yscale;
        vx[i] = tx(x + px * c + py * s);
        vy[i] = ty(y - px * s + py * c);
    }
    draw_tex_quad(t, vx, vy, u0, v0, u1, v1, blend_bgr, alpha);
}

static long g_glyph_log_count = 0;

void render_draw_glyph_colored(unsigned int tex, double dx, double dy, double dw, double dh,
                               float u0, float v0, float u1, float v1, unsigned int bgr,
                               double alpha) {
    RtTexture* t = tex_of(tex);
    if (g_frame_no <= 1 && g_glyph_log_count < 30) {
        g_glyph_log_count++;
        klog("glyph #%ld: tex=%u resolved=%d atlas_w=%d atlas_h=%d pw=%d ph=%d "
             "uv=(%.3f,%.3f,%.3f,%.3f) dwh=(%.1f,%.1f) bgr=%06x",
             g_glyph_log_count, tex, t ? 1 : 0, t ? t->w : -1, t ? t->h : -1, t ? t->pw : -1,
             t ? t->ph : -1, u0, v0, u1, v1, dw, dh, bgr);
    }
    if (!t) return;
    render_touch_texture(tex);
    float vx[4] = {tx(dx), tx(dx + dw), tx(dx + dw), tx(dx)};
    float vy[4] = {ty(dy), ty(dy), ty(dy + dh), ty(dy + dh)};
    draw_tex_quad(t, vx, vy, u0, v0, u1, v1, bgr, alpha);
}

void render_draw_glyph(unsigned int tex, double dx, double dy, double dw, double dh, float u0,
                       float v0, float u1, float v1) {
    render_draw_glyph_colored(tex, dx, dy, dw, dh, u0, v0, u1, v1, g_color_bgr, g_alpha);
}

static Vtx mkv(double x, double y, unsigned int bgr, double alpha) {
    Vtx v;
    v.x = tx(x);
    v.y = ty(y);
    v.u = 0;
    v.v = 0;
    vcol(bgr, alpha, v.r, v.g, v.b, v.a);
    return v;
}

static C3D_Tex* white_c3dtex() {
    RtTexture* t = tex_of(g_white_tex);
    return t ? &t->tex : nullptr;
}

static long g_fade_log_count = 0;

void render_draw_rectangle_color(double x1, double y1, double x2, double y2, unsigned int c1,
                                 unsigned int c2, unsigned int c3, unsigned int c4, bool outline) {
    if (c1 == 0 && c2 == 0 && c3 == 0 && c4 == 0 && x1 == 0 && y1 == 0 && !outline &&
        g_fade_log_count < 400) {
        g_fade_log_count++;
        klog("fade-rect #%ld: frame=%ld xy=(%.1f,%.1f)-(%.1f,%.1f) alpha=%.3f gui=(%d,%d) "
             "xf=(ox=%.1f,oy=%.1f,sx=%.3f,sy=%.3f) blend=(src=%d,dst=%d,asrc=%d,adst=%d) "
             "colormask=(%d,%d,%d,%d) fog_on=%d",
             g_fade_log_count, g_frame_no, x1, y1, x2, y2, g_alpha, g_gui_w, g_gui_h, g_xf.ox,
             g_xf.oy, g_xf.sx, g_xf.sy, g_blend_src, g_blend_dst, g_blend_asrc, g_blend_adst,
             g_colormask[0] ? 1 : 0, g_colormask[1] ? 1 : 0, g_colormask[2] ? 1 : 0,
             g_colormask[3] ? 1 : 0, g_fog_on ? 1 : 0);
    }
    bool is_fade = (c1 == 0 && c2 == 0 && c3 == 0 && c4 == 0 && x1 == 0 && y1 == 0 && !outline);
    if (outline) {
        render_draw_line(x1, y1, x2, y1, 1, c1, c2);
        render_draw_line(x2, y1, x2, y2, 1, c2, c3);
        render_draw_line(x2, y2, x1, y2, 1, c3, c4);
        render_draw_line(x1, y2, x1, y1, 1, c4, c1);
        return;
    }
    if (is_fade) {
        Vtx v[4] = {mkv(x1, y1, 0xFF00FF, 1.0), mkv(x2 + 1, y1, 0xFF00FF, 1.0),
                   mkv(x2 + 1, y2 + 1, 0xFF00FF, 1.0), mkv(x1, y2 + 1, 0xFF00FF, 1.0)};
        const u16 idx[6] = {0, 1, 2, 0, 2, 3};
        submit(v, 4, idx, 6, white_c3dtex());
        return;
    }
    Vtx v[4] = {mkv(x1, y1, c1, g_alpha), mkv(x2 + 1, y1, c2, g_alpha),
               mkv(x2 + 1, y2 + 1, c3, g_alpha), mkv(x1, y2 + 1, c4, g_alpha)};
    const u16 idx[6] = {0, 1, 2, 0, 2, 3};
    submit(v, 4, idx, 6, white_c3dtex());
}

void render_draw_rectangle(double x1, double y1, double x2, double y2, bool outline) {
    render_draw_rectangle_color(x1, y1, x2, y2, g_color_bgr, g_color_bgr, g_color_bgr, g_color_bgr,
                                outline);
}

void render_draw_line(double x1, double y1, double x2, double y2, double width, unsigned int c1,
                      unsigned int c2) {
    double dx = x2 - x1, dy = y2 - y1;
    double len = std::hypot(dx, dy);
    if (len < 1e-9) len = 1;
    double w = width <= 1.0 ? 1.0 : width;
    double nx = -dy / len * w * 0.5, ny = dx / len * w * 0.5;
    Vtx v[4] = {mkv(x1 + nx, y1 + ny, c1, g_alpha), mkv(x1 - nx, y1 - ny, c1, g_alpha),
               mkv(x2 - nx, y2 - ny, c2, g_alpha), mkv(x2 + nx, y2 + ny, c2, g_alpha)};
    const u16 idx[6] = {0, 1, 2, 0, 2, 3};
    submit(v, 4, idx, 6, white_c3dtex());
}

void render_draw_ellipse(double x1, double y1, double x2, double y2, unsigned int c1,
                         unsigned int c2, bool outline) {
    double cx = (x1 + x2) * 0.5, cy = (y1 + y2) * 0.5;
    double rx = std::fabs(x2 - x1) * 0.5, ry = std::fabs(y2 - y1) * 0.5;
    const int seg = 48;
    if (outline) {
        for (int i = 0; i < seg; ++i) {
            double a0 = i * 2.0 * 3.14159265358979 / seg;
            double a1 = (i + 1) * 2.0 * 3.14159265358979 / seg;
            render_draw_line(cx + std::cos(a0) * rx, cy + std::sin(a0) * ry, cx + std::cos(a1) * rx,
                             cy + std::sin(a1) * ry, 1, c2, c2);
        }
        return;
    }
    std::vector<Vtx> v;
    std::vector<u16> idx;
    v.push_back(mkv(cx, cy, c1, g_alpha));
    for (int i = 0; i <= seg; ++i) {
        double a = i * 2.0 * 3.14159265358979 / seg;
        v.push_back(mkv(cx + std::cos(a) * rx, cy + std::sin(a) * ry, c2, g_alpha));
    }
    for (int i = 1; i <= seg; ++i) {
        idx.push_back(0);
        idx.push_back((u16)i);
        idx.push_back((u16)(i + 1));
    }
    submit(v.data(), (int)v.size(), idx.data(), (int)idx.size(), white_c3dtex());
}

void render_draw_triangle(double x1, double y1, double x2, double y2, double x3, double y3,
                          unsigned int c1, unsigned int c2, unsigned int c3, bool outline) {
    if (outline) {
        render_draw_line(x1, y1, x2, y2, 1, c1, c2);
        render_draw_line(x2, y2, x3, y3, 1, c2, c3);
        render_draw_line(x3, y3, x1, y1, 1, c3, c1);
        return;
    }
    Vtx v[3] = {mkv(x1, y1, c1, g_alpha), mkv(x2, y2, c2, g_alpha), mkv(x3, y3, c3, g_alpha)};
    const u16 idx[3] = {0, 1, 2};
    submit(v, 3, idx, 3, white_c3dtex());
}

void render_draw_point(double x, double y, unsigned int c) {
    double sx = std::max(1e-6, g_xf.sx), sy = std::max(1e-6, g_xf.sy);
    Vtx v[4] = {mkv(x, y, c, g_alpha), mkv(x + 1.0 / sx, y, c, g_alpha),
               mkv(x + 1.0 / sx, y + 1.0 / sy, c, g_alpha), mkv(x, y + 1.0 / sy, c, g_alpha)};
    const u16 idx[6] = {0, 1, 2, 0, 2, 3};
    submit(v, 4, idx, 6, white_c3dtex());
}

struct PrimVert {
    double x, y, u, v;
    unsigned int color;
    double alpha;
};
static int g_prim_kind = 0;
static unsigned int g_prim_tex = 0;
static std::vector<PrimVert> g_prim_verts;

void render_primitive_begin(int kind, unsigned int tex) {
    g_prim_kind = kind;
    g_prim_tex = tex;
    g_prim_verts.clear();
}

void render_primitive_vertex(double x, double y, double u, double v, unsigned int color,
                             double alpha, bool textured) {
    PrimVert pv;
    pv.x = x;
    pv.y = y;
    pv.u = textured ? u : 0;
    pv.v = textured ? v : 0;
    pv.color = color;
    pv.alpha = alpha;
    g_prim_verts.push_back(pv);
}

void render_primitive_end() {
    int n = (int)g_prim_verts.size();
    if (n == 0) return;
    RtTexture* t = g_prim_tex ? tex_of(g_prim_tex) : nullptr;

    if (g_prim_kind == 1) {
        for (const PrimVert& pv : g_prim_verts) render_draw_point(pv.x, pv.y, pv.color);
        return;
    }
    if (g_prim_kind == 2) {
        for (int i = 0; i + 1 < n; i += 2)
            render_draw_line(g_prim_verts[i].x, g_prim_verts[i].y, g_prim_verts[i + 1].x,
                             g_prim_verts[i + 1].y, 1, g_prim_verts[i].color,
                             g_prim_verts[i + 1].color);
        return;
    }
    if (g_prim_kind == 3) {
        for (int i = 0; i + 1 < n; ++i)
            render_draw_line(g_prim_verts[i].x, g_prim_verts[i].y, g_prim_verts[i + 1].x,
                             g_prim_verts[i + 1].y, 1, g_prim_verts[i].color,
                             g_prim_verts[i + 1].color);
        return;
    }

    RtTexture* bind_tex = t ? t : tex_of(g_white_tex);
    float su = bind_tex->w > 0 ? (float)bind_tex->w / bind_tex->pw : 1.0f;
    float sv = bind_tex->h > 0 ? (float)bind_tex->h / bind_tex->ph : 1.0f;

    std::vector<Vtx> verts(n);
    for (int i = 0; i < n; ++i) {
        const PrimVert& pv = g_prim_verts[i];
        verts[i].x = tx(pv.x);
        verts[i].y = ty(pv.y);
        float vv = (float)pv.v;
        if (bind_tex->rt) {
            vv = 1.0f - vv;
            verts[i].v = vv * sv;
        } else {
            verts[i].v = 1.0f - (vv * sv);
        }
        verts[i].u = (float)pv.u * su;
        vcol(pv.color, pv.alpha, verts[i].r, verts[i].g, verts[i].b, verts[i].a);
    }
    std::vector<u16> idx;
    if (g_prim_kind == 5) {
        for (int i = 0; i + 2 < n; ++i) {
            idx.push_back((u16)i);
            idx.push_back((u16)(i + 1));
            idx.push_back((u16)(i + 2));
        }
    } else if (g_prim_kind == 6) {
        for (int i = 1; i + 1 < n; ++i) {
            idx.push_back(0);
            idx.push_back((u16)i);
            idx.push_back((u16)(i + 1));
        }
    } else {
        for (int i = 0; i + 2 < n; i += 3) {
            idx.push_back((u16)i);
            idx.push_back((u16)(i + 1));
            idx.push_back((u16)(i + 2));
        }
    }
    if (idx.empty()) return;
    submit(verts.data(), n, idx.data(), (int)idx.size(), &bind_tex->tex);
}


struct NamedBit { const char* name; u32 mask; };
static const NamedBit kButtonNames[] = {
    {"A", KEY_A}, {"B", KEY_B}, {"X", KEY_X}, {"Y", KEY_Y},
    {"L", KEY_L}, {"R", KEY_R}, {"ZL", KEY_ZL}, {"ZR", KEY_ZR},
    {"START", KEY_START}, {"SELECT", KEY_SELECT},
    {"DPAD_UP", KEY_DUP}, {"DPAD_DOWN", KEY_DDOWN},
    {"DPAD_LEFT", KEY_DLEFT}, {"DPAD_RIGHT", KEY_DRIGHT},
    {"CPAD_UP", KEY_CPAD_UP}, {"CPAD_DOWN", KEY_CPAD_DOWN},
    {"CPAD_LEFT", KEY_CPAD_LEFT}, {"CPAD_RIGHT", KEY_CPAD_RIGHT},
    {"CSTICK_UP", KEY_CSTICK_UP}, {"CSTICK_DOWN", KEY_CSTICK_DOWN},
    {"CSTICK_LEFT", KEY_CSTICK_LEFT}, {"CSTICK_RIGHT", KEY_CSTICK_RIGHT},
    {"TOUCH", KEY_TOUCH},
};

struct NamedVk { const char* name; int vk; };
static const NamedVk kKeyNames[] = {
    {"enter", 13}, {"escape", 27}, {"space", 32},
    {"shift", 16}, {"control", 17}, {"ctrl", 17}, {"alt", 18},
    {"tab", 9}, {"backspace", 8},
    {"left", 37}, {"up", 38}, {"right", 39}, {"down", 40},
    {"home", 36}, {"end", 35}, {"pageup", 33}, {"pagedown", 34},
    {"insert", 45}, {"delete", 46},
    {"f1", 112}, {"f2", 113}, {"f3", 114}, {"f4", 115},
    {"f5", 116}, {"f6", 117}, {"f7", 118}, {"f8", 119},
    {"f9", 120}, {"f10", 121}, {"f11", 122}, {"f12", 123},
};

static const char* kDefaultInputIni =
    "; kwik input mapping - N3DS button = keyboard key\n"
    "; buttons: A B X Y L R ZL ZR START SELECT\n"
    ";          DPAD_UP DPAD_DOWN DPAD_LEFT DPAD_RIGHT\n"
    ";          CPAD_UP CPAD_DOWN CPAD_LEFT CPAD_RIGHT\n"
    ";          CSTICK_UP CSTICK_DOWN CSTICK_LEFT CSTICK_RIGHT (New3DS)\n"
    ";          TOUCH\n"
    "; keys: a-z 0-9 enter escape space shift control alt tab backspace\n"
    ";       left up right down home end pageup pagedown insert delete f1-f12\n"
    "A = enter\n"
    "START = enter\n"
    "B = escape\n"
    "X = space\n"
    "Y = y\n"
    "L = q\n"
    "R = e\n"
    "DPAD_LEFT = left\n"
    "DPAD_UP = up\n"
    "DPAD_RIGHT = right\n"
    "DPAD_DOWN = down\n"
    "CPAD_LEFT = left\n"
    "CPAD_UP = up\n"
    "CPAD_RIGHT = right\n"
    "CPAD_DOWN = down\n";

static u32 g_vk_mask[512];

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static u32 find_button_mask(const std::string& name) {
    for (auto& b : kButtonNames)
        if (strcasecmp(b.name, name.c_str()) == 0) return b.mask;
    return 0;
}

static bool find_vk(const std::string& name, int* out_vk) {
    if (name.size() == 1) {
        char c = name[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            *out_vk = c;
            return true;
        }
    }
    for (auto& k : kKeyNames)
        if (strcasecmp(k.name, name.c_str()) == 0) {
            *out_vk = k.vk;
            return true;
        }
    return false;
}

static void parse_input_ini(const std::string& contents) {
    size_t pos = 0;
    while (pos <= contents.size()) {
        size_t nl = contents.find('\n', pos);
        std::string line = nl == std::string::npos ? contents.substr(pos) : contents.substr(pos, nl - pos);
        pos = nl == std::string::npos ? contents.size() + 1 : nl + 1;

        line = trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));
        u32 mask = find_button_mask(key);
        int vk = 0;
        if (mask != 0 && find_vk(val, &vk) && vk >= 0 && vk < 512)
            g_vk_mask[vk] |= mask;
    }
}

static void load_input_map() {
    for (int i = 0; i < 512; ++i) g_vk_mask[i] = 0;

    std::string path = g_game_dir.empty() ? "input.ini" : g_game_dir + "/input.ini";
    FILE* f = std::fopen(path.c_str(), "r");
    if (f) {
        std::string contents;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), f)) contents += buf;
        std::fclose(f);
        parse_input_ini(contents);
    } else {
        parse_input_ini(kDefaultInputIni);
        FILE* w = std::fopen(path.c_str(), "w");
        if (w) {
            std::fwrite(kDefaultInputIni, 1, std::strlen(kDefaultInputIni), w);
            std::fclose(w);
        }
    }
}

static bool key_state(int vk) {
    if (vk < 0 || vk >= 512) return false;
    return (hidKeysHeld() & g_vk_mask[vk]) != 0;
}

static void console_frame_update() {
    g_fps_frames++;
    g_fps_accum += g_dt;
    if (g_fps_accum >= 0.5) {
        g_fps_disp = g_fps_frames / g_fps_accum;
        g_fps_frames = 0;
        g_fps_accum = 0.0;
    }
    consoleSelect(&g_bottom_console);
    printf("\x1b[0;0Hkwik debug console      ");
    printf("\x1b[1;0Hfps: %5.1f  frame: %5.2fms   ", g_fps_disp, g_dt * 1000.0);
    printf("\x1b[2;0Hroom: %dx%d  view: %dx%d   ", g_room_w, g_room_h, (int)g_view_w,
           (int)g_view_h);
    int tex_alive = 0;
    for (auto& tp : g_textures)
        if (tp->alive) ++tex_alive;
    int surf_alive = 0;
    for (auto& sf : g_surfaces)
        if (sf.alive) ++surf_alive;
    printf("\x1b[3;0Htextures: %3d  surfaces: %3d   ", tex_alive, surf_alive);
    printf("\x1b[4;0H------------------------------------\n");
}


bool render_init(const char* title, int width, int height, unsigned int bg_color) {
    (void)title;
    klog("render_init: width=%d height=%d", width, height);
    gfxInitDefault();
    gfxSet3D(false);
    gfxSetDoubleBuffering(GFX_TOP, false);
    consoleInit(GFX_BOTTOM, &g_bottom_console);
    klog("gfxInitDefault done");

    load_input_map();
    klog("input.ini loaded");

    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) {
        klog("C3D_Init FAILED");
        return false;
    }
    klog("C3D_Init ok");

    g_frame_arena_size = 4 * 1024 * 1024;
    g_frame_arena = (u8*)linearAlloc(g_frame_arena_size);
    g_frame_arena_offset = 0;
    klog("frame arena: %p size=%zu", (void*)g_frame_arena, g_frame_arena_size);

    int phys_w = 400, phys_h = 240;
    g_screen_rt = C3D_RenderTargetCreate(phys_h, phys_w, GPU_RB_RGBA8, -1);
    klog("screen render target: %p", (void*)g_screen_rt);
    if (!g_screen_rt) klog("C3D_RenderTargetCreate FAILED for screen target");
    C3D_RenderTargetSetOutput(g_screen_rt, GFX_TOP, GFX_LEFT,
                              GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                                  GX_TRANSFER_RAW_COPY(0) |
                                  GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                                  GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                                  GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
    klog("screen render target output set");

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, (GPU_TEVSRC)0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);

    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_ALL);

    g_vshader_dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    klog("shader dvlb: %p numDVLE=%d", (void*)g_vshader_dvlb,
         g_vshader_dvlb ? (int)g_vshader_dvlb->numDVLE : -1);
    shaderProgramInit(&g_program);
    int rc = shaderProgramSetVsh(&g_program, &g_vshader_dvlb->DVLE[0]);
    klog("shaderProgramSetVsh rc=%d", rc);
    C3D_BindProgram(&g_program);

    const char* mtx_names[MTX_MODE_COUNT] = {"mv", "p", "tex"};
    for (int i = 0; i < MTX_MODE_COUNT; ++i) {
        MtxStack_Init(&g_mtx_stacks[i]);
        g_uloc_mtx[i] = shaderInstanceGetUniformLocation(g_program.vertexShader, mtx_names[i]);
        klog("uniform %s loc=%d", mtx_names[i], g_uloc_mtx[i]);
        MtxStack_Bind(&g_mtx_stacks[i], GPU_VERTEX_SHADER, g_uloc_mtx[i], 4);
        Mtx_Identity(MtxStack_Cur(&g_mtx_stacks[i]));
    }

    g_gui_w = width;
    g_gui_h = height;
    g_room_w = width;
    g_room_h = height;
    g_view_w = width;
    g_view_h = height;

    unsigned char white_px[4] = {255, 255, 255, 255};
    g_white_tex = render_upload_texture(white_px, 1, 1);
    klog("white tex id=%u", g_white_tex);

    double aspect_scale = std::min((double)phys_w / width, (double)phys_h / height);
    int fbo_w = std::max(1, (int)std::lround(width * aspect_scale));
    int fbo_h = std::max(1, (int)std::lround(height * aspect_scale));
    g_app_tex = create_texture(fbo_w, fbo_h, true);
    g_fbo_w = fbo_w;
    g_fbo_h = fbo_h;
    klog("app tex id=%u", g_app_tex);
    if (g_app_tex) {
        RtTexture* at = tex_of(g_app_tex);
        klog("app tex rt=%p pw=%d ph=%d", at ? (void*)at->rt : nullptr, at ? at->pw : -1,
             at ? at->ph : -1);
    } else {
        klog("app tex creation FAILED");
    }

    render_set_room(width, height, bg_color);
    g_inited = true;
    klog("render_init complete");
    return true;
}

bool render_should_close() { return !g_inited || !aptMainLoop(); }

static void apply_view_xf() {
    double vw = g_view_w > 0 ? g_view_w : g_room_w;
    double vh = g_view_h > 0 ? g_view_h : g_room_h;
    int tw = g_fbo_w > 0 ? g_fbo_w : g_gui_w;
    int th = g_fbo_h > 0 ? g_fbo_h : g_gui_h;
    g_xf.ox = g_view_x;
    g_xf.oy = g_view_y;
    g_xf.sx = vw > 0 ? tw / vw : 1;
    g_xf.sy = vh > 0 ? th / vh : 1;
}

void render_begin_frame() {
    evict_bump_frame();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    g_frame_arena_offset = 0;
    g_target_stack.clear();
    g_xf_stack.clear();
    g_fog_on = false;
    RtTexture* t = tex_of(g_app_tex);
    if (t && t->rt) {
        C3D_FrameDrawOn(t->rt);
        C3D_SetViewport(0, 0, t->w, t->h);
        C3D_RenderTargetClear(t->rt, C3D_CLEAR_COLOR, pack_clear_color(t->fmt, 0, 0, 0, 255), 0);
        set_surface_ortho(t->w, t->h);
    }
    apply_view_xf();
}

void render_begin_gui() {
    g_target_stack.clear();
    g_xf_stack.clear();
    RtTexture* t = tex_of(g_app_tex);
    if (t && t->rt) {
        C3D_FrameDrawOn(t->rt);
        C3D_SetViewport(0, 0, t->w, t->h);
        set_surface_ortho(t->w, t->h);
    }
    g_xf.ox = 0;
    g_xf.oy = 0;
    g_xf.sx = g_gui_w > 0 ? (double)(g_fbo_w > 0 ? g_fbo_w : g_gui_w) / g_gui_w : 1;
    g_xf.sy = g_gui_h > 0 ? (double)(g_fbo_h > 0 ? g_fbo_h : g_gui_h) / g_gui_h : 1;
}

void render_set_title(const char*) {}

void render_set_view(double x, double y, double w, double h) {
    g_view_x = x;
    g_view_y = y;
    if (w > 0) g_view_w = w;
    if (h > 0) g_view_h = h;
    apply_view_xf();
}

double render_delta_time() { return g_dt; }
double render_time_ms() { return time_seconds() * 1000.0; }

static void blit_app_to_screen() {
    RtTexture* t = tex_of(g_app_tex);
    if (!t) return;
    C3D_FrameDrawOn(g_screen_rt);
    C3D_RenderTargetClear(g_screen_rt, C3D_CLEAR_COLOR, 0x000000FF, 0);
    C3D_SetViewport(0, 0, 240, 400);
    C3D_Mtx* p = MtxStack_Cur(&g_mtx_stacks[MTX_PROJECTION]);
    Mtx_OrthoTilt(p, 0, 400.0f, 240.0f, 0, -1.0f, 1.0f, true);
    C3D_Mtx* mv = MtxStack_Cur(&g_mtx_stacks[MTX_MODELVIEW]);
    Mtx_Identity(mv);
    g_xf.ox = 0;
    g_xf.oy = 0;
    g_xf.sx = 1;
    g_xf.sy = 1;

    double scale = std::min(400.0 / t->w, 240.0 / t->h);
    float dw = (float)(t->w * scale);
    float dh = (float)(t->h * scale);
    float ox = (float)((400.0 - dw) * 0.5);
    float oy = (float)((240.0 - dh) * 0.5);
    bool save_fog = g_fog_on;
    g_fog_on = false;
    float vx[4] = {ox, ox + dw, ox + dw, ox};
    float vy[4] = {oy, oy, oy + dh, oy + dh};
    draw_tex_quad(t, vx, vy, 0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFF, 1.0);
    g_fog_on = save_fog;
}

void render_present_last() {
    if (!g_inited) return;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    blit_app_to_screen();
    C3D_FrameEnd(0);
    hidScanInput();
}

void render_end_frame() {
    blit_app_to_screen();
    C3D_FrameEnd(0);

    g_frame_no++;
    if (g_frame_no <= 10 || g_frame_no % 120 == 0)
        klog("frame %ld: draws_this_frame=%ld app_tex=%u screen_rt=%p", g_frame_no,
             g_draw_calls_this_frame, g_app_tex, (void*)g_screen_rt);
    g_draw_calls_this_frame = 0;

    double now = time_seconds();
    g_dt = g_last_time > 0.0 ? now - g_last_time : 0.0;
    if (g_dt > 0.25) g_dt = 0.25;
    g_last_time = now;
    console_frame_update();

    for (int i = 0; i < 512; ++i) g_keys_prev[i] = g_keys_now[i];
    for (int i = 0; i < 3; ++i) g_mouse_prev[i] = g_mouse_now[i];

    hidScanInput();
    for (int i = 0; i < 512; ++i) g_keys_now[i] = key_state(i);
    bool any = false;
    for (int i = 2; i < 512; ++i) any = any || g_keys_now[i];
    g_keys_now[1] = any;
    g_keys_now[0] = false;

    touchPosition touch;
    hidTouchRead(&touch);
    g_touch_down = (hidKeysHeld() & KEY_TOUCH) != 0;
    g_touch_x = (float)touch.px;
    g_touch_y = (float)touch.py;

    g_mouse_now[0] = g_touch_down;
    g_mouse_now[1] = false;
    g_mouse_now[2] = false;
}

double render_wheel_delta() { return 0.0; }

void render_idle() { hidScanInput(); }

bool render_has_focus() { return true; }

bool render_key_down(int vk) {
    if (vk == 0) {
        for (int i = 2; i < 512; ++i)
            if (g_keys_now[i]) return false;
        return true;
    }
    if (vk < 0 || vk >= 512) return false;
    return g_keys_now[vk];
}
bool render_key_pressed(int vk) {
    if (vk < 0 || vk >= 512) return false;
    return g_keys_now[vk] && !g_keys_prev[vk];
}
bool render_key_released(int vk) {
    if (vk < 0 || vk >= 512) return false;
    return !g_keys_now[vk] && g_keys_prev[vk];
}
void render_keyboard_clear(int vk) {
    if (vk < 0) {
        for (int i = 0; i < 512; ++i) g_keys_now[i] = g_keys_prev[i] = false;
        return;
    }
    if (vk >= 512) return;
    g_keys_now[vk] = g_keys_prev[vk] = false;
}

static void mouse_to_gui(double& gx, double& gy) {
    int cw = g_fbo_w > 0 ? g_fbo_w : g_gui_w;
    int ch = g_fbo_h > 0 ? g_fbo_h : g_gui_h;
    double scale = std::min(400.0 / cw, 240.0 / ch);
    if (scale <= 0) {
        gx = gy = 0;
        return;
    }
    double ox = (400.0 - cw * scale) * 0.5;
    double oy = (240.0 - ch * scale) * 0.5;
    gx = (g_touch_x - ox) / scale;
    gy = (g_touch_y - oy) / scale;
}

double render_mouse_x() {
    double gx, gy;
    mouse_to_gui(gx, gy);
    return gx;
}
double render_mouse_y() {
    double gx, gy;
    mouse_to_gui(gx, gy);
    return gy;
}
bool render_mouse_down(int b) { return b >= 0 && b < 3 && g_mouse_now[b]; }
bool render_mouse_pressed(int b) { return b >= 0 && b < 3 && g_mouse_now[b] && !g_mouse_prev[b]; }
bool render_mouse_released(int b) { return b >= 0 && b < 3 && !g_mouse_now[b] && g_mouse_prev[b]; }

void render_set_color(unsigned int bgr) { g_color_bgr = bgr; }
unsigned int render_get_color() { return g_color_bgr; }
void render_set_alpha(double alpha) { g_alpha = static_cast<float>(alpha); }
double render_get_alpha() { return g_alpha; }
void render_set_halign(int align) { g_halign = align; }
void render_set_valign(int align) { g_valign = align; }
int render_get_halign() { return g_halign; }
int render_get_valign() { return g_valign; }

void render_set_blendmode(int mode) {
    switch (mode) {
        case 1:
            g_blend_src = 5; g_blend_dst = 2; g_blend_asrc = 5; g_blend_adst = 2;
            break;
        case 2:
            g_blend_src = 5; g_blend_dst = 4; g_blend_asrc = 5; g_blend_adst = 4;
            break;
        case 3:
            g_blend_src = 1; g_blend_dst = 4; g_blend_asrc = 1; g_blend_adst = 4;
            break;
        default:
            g_blend_src = 5; g_blend_dst = 6; g_blend_asrc = 5; g_blend_adst = 6;
            break;
    }
}

void render_set_blendmode_ext(int src, int dst) {
    g_blend_src = src;
    g_blend_dst = dst;
    g_blend_asrc = src;
    g_blend_adst = dst;
}

void render_set_blendmode_sepalpha(int src, int dst, int asrc, int adst) {
    g_blend_src = src;
    g_blend_dst = dst;
    g_blend_asrc = asrc;
    g_blend_adst = adst;
}

void render_set_colorwrite(bool r, bool g, bool b, bool a) {
    g_colormask[0] = r;
    g_colormask[1] = g;
    g_colormask[2] = b;
    g_colormask[3] = a;
}

void render_set_fog(bool on, unsigned int bgr) {
    static bool disabled = std::getenv("KWIK_NO_FOG") != nullptr;
    klog("render_set_fog: on=%d bgr=%06x disabled=%d frame=%ld", on ? 1 : 0, bgr, disabled ? 1 : 0,
         g_frame_no);
    if (disabled) return;
    g_fog_on = on;
    g_fog_col[0] = (u8)(bgr & 0xFF);
    g_fog_col[1] = (u8)((bgr >> 8) & 0xFF);
    g_fog_col[2] = (u8)((bgr >> 16) & 0xFF);
}

int render_gui_width() { return g_gui_w; }
int render_gui_height() { return g_gui_h; }

void render_set_window_size(int, int) {}
void render_set_fullscreen(bool) {}
bool render_get_fullscreen() { return true; }
void render_center_window() {}

int render_window_width() { return 400; }
int render_window_height() { return 240; }
int render_display_width() { return 400; }
int render_display_height() { return 240; }

void render_set_room(int width, int height, unsigned int) {
    g_room_w = width;
    g_room_h = height;
}

void render_shutdown() {
    klog("render_shutdown: total frames=%ld", g_frame_no);
    if (g_frame_arena) {
        linearFree(g_frame_arena);
        g_frame_arena = nullptr;
    }
    for (auto& sf : g_surfaces)
        if (sf.alive) render_surface_free((int)(&sf - &g_surfaces[0]) + 1);
    for (auto& tp : g_textures)
        if (tp->alive) {
            if (tp->rt) C3D_RenderTargetDelete(tp->rt);
            C3D_TexDelete(&tp->tex);
            tp->alive = false;
        }
    g_textures.clear();
    g_surfaces.clear();

    if (g_screen_rt) {
        C3D_RenderTargetDelete(g_screen_rt);
        g_screen_rt = nullptr;
    }
    shaderProgramFree(&g_program);
    if (g_vshader_dvlb) {
        DVLB_Free(g_vshader_dvlb);
        g_vshader_dvlb = nullptr;
    }
    C3D_Fini();
    gfxExit();
    g_inited = false;
    if (g_klog_file) {
        std::fclose(g_klog_file);
        g_klog_file = nullptr;
    }
}

}
