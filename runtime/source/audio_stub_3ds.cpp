#include "gml_runtime.h"
#include "engine_internal.h"
#include "render.h"

#include <3ds.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
typedef struct stb_vorbis stb_vorbis_h;
struct StbVorbisInfoAbi {
    unsigned int sample_rate;
    int channels;
    unsigned int setup_memory_required;
    unsigned int setup_temp_memory_required;
    unsigned int temp_memory_required;
    int max_frame_size;
};
stb_vorbis_h* stb_vorbis_open_memory(const unsigned char* data, int len, int* error,
                                     const void* alloc_buffer);
StbVorbisInfoAbi stb_vorbis_get_info(stb_vorbis_h* f);
int stb_vorbis_get_samples_short_interleaved(stb_vorbis_h* f, int channels, short* buffer,
                                             int num_shorts);
void stb_vorbis_close(stb_vorbis_h* f);
int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate,
                             short** output);
}

namespace gml {

static const int kStreamBase = 200000;
static const int kHandleBase = 300000;
static const int kMaxChannels = 24;
static const int kStreamBufSamples = 22050;

static std::vector<std::string> g_streams;

int kwik_stream_register(const std::string& path) {
    g_streams.push_back(path);
    return kStreamBase + (int)g_streams.size() - 1;
}

const std::string* kwik_stream_path(int id) {
    int i = id - kStreamBase;
    if (i < 0 || (size_t)i >= g_streams.size()) return nullptr;
    return &g_streams[i];
}

struct Voice {
    int channel = -1;
    ndspWaveBuf wavebuf[2]{};
    short* pcm[2] = {nullptr, nullptr};
    bool is_stream = false;

    stb_vorbis_h* vorbis = nullptr;
    std::vector<unsigned char> encoded;
    int buf_shorts = 0;

    int channels = 1;
    int rate = 22050;
    bool loop = false;
    int asset = -1;
    int stream = -1;
    bool active = false;
    bool paused = false;
    float base_gain = 1.0f;
    float gain = 1.0f;
    float pitch = 1.0f;
    float fade_target = -1.0f;
    float fade_step = 0.0f;
};

static Voice* g_voices[kMaxChannels] = {};
static bool g_ndsp_ok = false;
static bool g_ndsp_tried = false;
static float g_master_gain = 1.0f;

static bool ensure_ndsp() {
    if (g_ndsp_tried) return g_ndsp_ok;
    g_ndsp_tried = true;
    Result rc = ndspInit();
    g_ndsp_ok = R_SUCCEEDED(rc);
    render_debug_log("audio: ndspInit rc=0x%08lX ok=%d", (unsigned long)rc, g_ndsp_ok ? 1 : 0);
    if (g_ndsp_ok) ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    return g_ndsp_ok;
}

static int alloc_channel() {
    for (int i = 0; i < kMaxChannels; ++i)
        if (!g_voices[i]) return i;
    return -1;
}

static void set_channel_mix(int chn, float vol) {
    float mix[12] = {0};
    mix[0] = vol;
    mix[1] = vol;
    ndspChnSetMix(chn, mix);
}

static void free_voice(Voice* v) {
    if (v->channel >= 0) {
        ndspChnWaveBufClear(v->channel);
        ndspChnReset(v->channel);
        g_voices[v->channel] = nullptr;
    }
    if (v->vorbis) stb_vorbis_close(v->vorbis);
    for (int i = 0; i < 2; ++i)
        if (v->pcm[i]) linearFree(v->pcm[i]);
    delete v;
}

struct WavInfo {
    const short* data;
    size_t sample_count;
    int channels;
    int rate;
};

static bool parse_wav(const unsigned char* d, size_t size, WavInfo& out) {
    if (size < 44 || std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WAVE", 4)) return false;
    size_t pos = 12;
    int channels = 0, rate = 0, bits = 0;
    const unsigned char* data_ptr = nullptr;
    size_t data_size = 0;
    while (pos + 8 <= size) {
        const unsigned char* id = d + pos;
        uint32_t csize;
        std::memcpy(&csize, d + pos + 4, 4);
        size_t body = pos + 8;
        if (body + csize > size) break;
        if (!std::memcmp(id, "fmt ", 4) && csize >= 16) {
            uint16_t ch, bps;
            uint32_t sr;
            std::memcpy(&ch, d + body + 2, 2);
            std::memcpy(&sr, d + body + 4, 4);
            std::memcpy(&bps, d + body + 14, 2);
            channels = ch;
            rate = (int)sr;
            bits = bps;
        } else if (!std::memcmp(id, "data", 4)) {
            data_ptr = d + body;
            data_size = csize;
        }
        pos = body + csize + (csize & 1);
    }
    if (!data_ptr || channels <= 0 || rate <= 0 || bits != 16) return false;
    out.data = (const short*)data_ptr;
    out.sample_count = data_size / 2 / (size_t)channels;
    out.channels = channels;
    out.rate = rate;
    return true;
}

static bool decode_full(const unsigned char* data, unsigned size, int type,
                        std::vector<short>& pcm, int& channels, int& rate) {
    if (type == 2 || (size >= 4 && !std::memcmp(data, "OggS", 4))) {
        short* out = nullptr;
        int ch = 0, sr = 0;
        int frames = stb_vorbis_decode_memory(data, (int)size, &ch, &sr, &out);
        if (frames <= 0 || !out) return false;
        pcm.assign(out, out + (size_t)frames * ch);
        std::free(out);
        channels = ch;
        rate = sr;
        return true;
    }
    WavInfo wi;
    if (!parse_wav(data, size, wi)) return false;
    pcm.assign(wi.data, wi.data + wi.sample_count * (size_t)wi.channels);
    channels = wi.channels;
    rate = wi.rate;
    return true;
}

static bool read_file_bytes(const std::string& path, std::vector<unsigned char>& out) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize(n);
    size_t got = std::fread(out.data(), 1, n, f);
    std::fclose(f);
    out.resize(got);
    return !out.empty();
}

static bool start_channel(Voice* v, float vol, float pitch) {
    int chn = alloc_channel();
    if (chn < 0) {
        render_debug_log("audio: no free ndsp channel (all %d in use)", kMaxChannels);
        return false;
    }
    v->channel = chn;
    g_voices[chn] = v;
    ndspChnReset(chn);
    ndspChnSetInterp(chn, NDSP_INTERP_LINEAR);
    ndspChnSetRate(chn, (float)v->rate * pitch);
    ndspChnSetFormat(chn, v->channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    set_channel_mix(chn, vol);
    return true;
}

static Voice* start_sfx_voice(const unsigned char* data, unsigned size, int type, float vol,
                              float pitch, bool loop) {
    std::vector<short> pcm;
    int channels = 1, rate = 22050;
    if (!decode_full(data, size, type, pcm, channels, rate) || pcm.empty()) {
        render_debug_log("audio: sfx decode FAILED size=%u type=%d", size, type);
        return nullptr;
    }
    render_debug_log("audio: sfx decoded samples=%zu channels=%d rate=%d", pcm.size(), channels,
                     rate);

    Voice* v = new Voice();
    v->channels = channels;
    v->rate = rate;
    v->loop = loop;

    size_t bytes = pcm.size() * sizeof(short);
    short* lin = (short*)linearAlloc(bytes);
    if (!lin) {
        delete v;
        return nullptr;
    }
    std::memcpy(lin, pcm.data(), bytes);
    DSP_FlushDataCache(lin, bytes);
    v->pcm[0] = lin;

    if (!start_channel(v, vol, pitch)) {
        linearFree(lin);
        delete v;
        return nullptr;
    }

    ndspWaveBuf& wb = v->wavebuf[0];
    std::memset(&wb, 0, sizeof(wb));
    wb.data_vaddr = lin;
    wb.nsamples = (u32)(pcm.size() / (size_t)channels);
    wb.looping = loop;
    ndspChnWaveBufAdd(v->channel, &wb);
    return v;
}

static bool stream_fill(Voice* v, int idx) {
    short* buf = v->pcm[idx];
    int want = v->buf_shorts;
    int got_total = 0;
    while (got_total < want) {
        int got = stb_vorbis_get_samples_short_interleaved(v->vorbis, v->channels,
                                                            buf + got_total, want - got_total);
        if (got <= 0) {
            if (!v->loop) break;
            stb_vorbis_close(v->vorbis);
            v->vorbis = stb_vorbis_open_memory(v->encoded.data(), (int)v->encoded.size(), nullptr,
                                               nullptr);
            if (!v->vorbis) break;
            continue;
        }
        got_total += got * v->channels;
    }
    if (got_total == 0) return false;
    DSP_FlushDataCache(buf, (u32)got_total * sizeof(short));
    ndspWaveBuf& wb = v->wavebuf[idx];
    std::memset(&wb, 0, sizeof(wb));
    wb.data_vaddr = buf;
    wb.nsamples = (u32)(got_total / v->channels);
    wb.looping = false;
    ndspChnWaveBufAdd(v->channel, &wb);
    return true;
}

static Voice* start_stream_voice(std::vector<unsigned char>&& encoded, float vol, float pitch,
                                 bool loop) {
    render_debug_log("audio: start_stream_voice bytes=%zu", encoded.size());
    int error = 0;
    stb_vorbis_h* vb = stb_vorbis_open_memory(encoded.data(), (int)encoded.size(), &error, nullptr);
    if (!vb) {
        render_debug_log("audio: stb_vorbis_open_memory FAILED error=%d", error);
        return nullptr;
    }
    StbVorbisInfoAbi info = stb_vorbis_get_info(vb);
    render_debug_log("audio: vorbis opened channels=%d rate=%u", info.channels,
                     info.sample_rate);
    if (info.channels <= 0 || info.sample_rate == 0) {
        stb_vorbis_close(vb);
        return nullptr;
    }

    Voice* v = new Voice();
    v->is_stream = true;
    v->vorbis = vb;
    v->encoded = std::move(encoded);
    v->channels = info.channels;
    v->rate = (int)info.sample_rate;
    v->loop = loop;
    v->buf_shorts = kStreamBufSamples * v->channels;

    for (int i = 0; i < 2; ++i) {
        v->pcm[i] = (short*)linearAlloc((size_t)v->buf_shorts * sizeof(short));
        if (!v->pcm[i]) {
            free_voice(v);
            return nullptr;
        }
    }

    if (!start_channel(v, vol, pitch)) {
        free_voice(v);
        return nullptr;
    }

    bool any = false;
    any |= stream_fill(v, 0);
    any |= stream_fill(v, 1);
    if (!any) {
        free_voice(v);
        return nullptr;
    }
    return v;
}

static Voice* start_voice(int what, bool loop) {
    if (!ensure_ndsp()) return nullptr;

    Voice* v = nullptr;
    float vol = 1.0f, pitch = 1.0f;

    if (what >= kStreamBase && what < kHandleBase) {
        const std::string* path = kwik_stream_path(what);
        std::vector<unsigned char> bytes;
        if (path) {
            render_debug_log("audio: stream requested path='%s' game_dir='%s'", path->c_str(),
                             g_game_dir.c_str());
            if (!read_file_bytes(*path, bytes)) {
                size_t slash = path->find_last_of('/');
                std::string base = slash == std::string::npos ? *path : path->substr(slash + 1);
                if (!read_file_bytes("mus/" + base, bytes))
                    if (!read_file_bytes("../mus/" + base, bytes))
                        if (!read_file_bytes(base, bytes) && !g_game_dir.empty()) {
                            if (!read_file_bytes(g_game_dir + "/" + *path, bytes))
                                if (!read_file_bytes(g_game_dir + "/mus/" + base, bytes))
                                    read_file_bytes(g_game_dir + "/" + base, bytes);
                        }
            }
            render_debug_log("audio: stream file read bytes=%zu", bytes.size());
            if (!bytes.empty()) v = start_stream_voice(std::move(bytes), vol, pitch, loop);
        } else {
            render_debug_log("audio: kwik_stream_path(%d) returned null", what);
        }
        if (v) v->stream = what;
    } else if (what >= 0 && what < g_sound_count) {
        const KwikSound& s = g_sound_table[what];
        vol = s.volume > 0 ? s.volume : 1.0f;
        pitch = s.pitch > 0 ? s.pitch : 1.0f;
        if (s.blob >= 0) {
            unsigned int size = 0;
            int type = 0;
            const unsigned char* data = kwik_sound_blob(s.blob, size, type);
            if (data && size) v = start_sfx_voice(data, size, type, vol, pitch, loop);
        } else if (s.file && *s.file) {
            std::vector<unsigned char> bytes;
            std::string fn = s.file;
            if (!read_file_bytes(fn, bytes)) {
                size_t dot = fn.find_last_of('.');
                std::string base = dot == std::string::npos ? fn : fn.substr(0, dot);
                if (!read_file_bytes(base + ".ogg", bytes))
                    if (!read_file_bytes(base + ".wav", bytes) && !g_game_dir.empty()) {
                        if (!read_file_bytes(g_game_dir + "/" + fn, bytes))
                            if (!read_file_bytes(g_game_dir + "/" + base + ".ogg", bytes))
                                read_file_bytes(g_game_dir + "/" + base + ".wav", bytes);
                    }
            }
            if (!bytes.empty())
                v = start_sfx_voice(bytes.data(), (unsigned)bytes.size(), 0, vol, pitch, loop);
        }
        if (v) v->asset = what;
    }

    if (!v) return nullptr;
    v->base_gain = vol;
    v->gain = 1.0f;
    v->pitch = pitch;
    v->active = true;
    return v;
}

static int store_voice(Voice* v) { return kHandleBase + v->channel; }

static Voice* voice_of_handle(int h) {
    int i = h - kHandleBase;
    if (i < 0 || i >= kMaxChannels) return nullptr;
    return g_voices[i];
}

template <typename F>
static void for_matching(int what, F f) {
    if (what >= kHandleBase) {
        Voice* v = voice_of_handle(what);
        if (v) f(v);
        return;
    }
    for (int i = 0; i < kMaxChannels; ++i) {
        Voice* v = g_voices[i];
        if (!v) continue;
        if ((what >= kStreamBase && v->stream == what) ||
            (what >= 0 && what < kStreamBase && v->asset == what))
            f(v);
    }
}

static double A(const Value* args, int argc, int i, double dflt = 0.0) {
    return i < argc ? (double)args[i] : dflt;
}

GMLFN(audio_play_sound) {
    (void)self;
    if (argc < 1) return Value(-1.0);
    int what = (int)(double)args[0];
    bool loop = argc > 2 && gml_truthy(args[2]);
    render_debug_log("audio: audio_play_sound what=%d loop=%d", what, loop ? 1 : 0);
    Voice* v = start_voice(what, loop);
    if (!v) {
        render_debug_log("audio: audio_play_sound FAILED what=%d", what);
        return Value(-1.0);
    }
    return Value((double)store_voice(v));
}

GMLFN(audio_stop_sound) {
    (void)self;
    if (argc < 1) return Value();
    int what = (int)(double)args[0];
    for (int i = 0; i < kMaxChannels; ++i) {
        Voice* v = g_voices[i];
        if (!v) continue;
        bool match = (what >= kHandleBase && i == what - kHandleBase) ||
                     (what >= kStreamBase && what < kHandleBase && v->stream == what) ||
                     (what >= 0 && what < kStreamBase && v->asset == what);
        if (match) free_voice(v);
    }
    return Value();
}

GMLFN(audio_stop_all) {
    (void)self; (void)args; (void)argc;
    for (int i = 0; i < kMaxChannels; ++i)
        if (g_voices[i]) free_voice(g_voices[i]);
    return Value();
}

GMLFN(audio_is_playing) {
    (void)self;
    if (argc < 1) return Value(0.0);
    bool playing = false;
    for_matching((int)(double)args[0], [&](Voice* v) {
        if (v->active) playing = true;
    });
    return Value(playing);
}

GMLFN(audio_pause_sound) {
    (void)self;
    if (argc < 1) return Value();
    for_matching((int)(double)args[0], [](Voice* v) {
        if (v->active && !v->paused) {
            ndspChnSetPaused(v->channel, true);
            v->paused = true;
        }
    });
    return Value();
}

GMLFN(audio_resume_sound) {
    (void)self;
    if (argc < 1) return Value();
    for_matching((int)(double)args[0], [](Voice* v) {
        if (v->active && v->paused) {
            ndspChnSetPaused(v->channel, false);
            v->paused = false;
        }
    });
    return Value();
}

GMLFN(audio_pause_all) {
    (void)self; (void)args; (void)argc;
    for (int i = 0; i < kMaxChannels; ++i) {
        Voice* v = g_voices[i];
        if (v && v->active && !v->paused) {
            ndspChnSetPaused(v->channel, true);
            v->paused = true;
        }
    }
    return Value();
}

GMLFN(audio_resume_all) {
    (void)self; (void)args; (void)argc;
    for (int i = 0; i < kMaxChannels; ++i) {
        Voice* v = g_voices[i];
        if (v && v->active && v->paused) {
            ndspChnSetPaused(v->channel, false);
            v->paused = false;
        }
    }
    return Value();
}

GMLFN(audio_sound_gain) {
    (void)self;
    if (argc < 2) return Value();
    float target = (float)(double)args[1];
    double time_ms = A(args, argc, 2);
    for_matching((int)(double)args[0], [&](Voice* v) {
        if (time_ms <= 0) {
            v->gain = target;
            v->fade_target = -1;
            set_channel_mix(v->channel, v->base_gain * v->gain * g_master_gain);
        } else {
            v->fade_target = target;
            double steps = time_ms / 1000.0 * 60.0;
            v->fade_step = (float)((target - v->gain) / (steps > 1 ? steps : 1));
        }
    });
    return Value();
}

GMLFN(audio_sound_pitch) {
    (void)self;
    if (argc < 2) return Value();
    float p = (float)(double)args[1];
    for_matching((int)(double)args[0], [&](Voice* v) {
        v->pitch = p;
        ndspChnSetRate(v->channel, (float)v->rate * p);
    });
    return Value();
}

GMLFN(audio_sound_get_track_position) {
    (void)self;
    (void)argc;
    return Value(0.0);
}

GMLFN(audio_sound_set_track_position) {
    (void)self; (void)args; (void)argc;
    return Value();
}

GMLFN(audio_set_master_gain) {
    (void)self;
    if (argc >= 1) {
        g_master_gain = (float)(double)args[argc - 1];
        for (int i = 0; i < kMaxChannels; ++i) {
            Voice* v = g_voices[i];
            if (v) set_channel_mix(v->channel, v->base_gain * v->gain * g_master_gain);
        }
    }
    return Value();
}

GMLFN(audio_create_stream) {
    (void)self;
    if (argc < 1) return Value(-1.0);
    return Value((double)kwik_stream_register((std::string)args[0]));
}

GMLFN(audio_destroy_stream) { return audio_stop_sound(self, args, argc); }

GMLFN(audio_group_is_loaded) { (void)self; (void)args; (void)argc; return Value(1.0); }
GMLFN(audio_group_load) { (void)self; (void)args; (void)argc; return Value(1.0); }
GMLFN(audio_group_set_gain) { (void)self; (void)args; (void)argc; return Value(); }

double kwik_voice_gain(int what) {
    double g = 1.0;
    for_matching(what, [&](Voice* v) { g = v->gain; });
    return g;
}

double kwik_voice_pitch(int what) {
    double p = 1.0;
    for_matching(what, [&](Voice* v) { p = v->pitch; });
    return p;
}

bool kwik_voice_paused(int what) {
    bool paused = false;
    for_matching(what, [&](Voice* v) { paused = v->paused; });
    return paused;
}

double kwik_sound_length_seconds(int) { return 0.0; }

void kwik_audio_update() {
    if (!g_ndsp_ok) return;
    for (int i = 0; i < kMaxChannels; ++i) {
        Voice* v = g_voices[i];
        if (!v) continue;

        if (v->is_stream && v->active && !v->paused) {
            for (int b = 0; b < 2; ++b) {
                if (v->wavebuf[b].status == NDSP_WBUF_DONE) {
                    if (!stream_fill(v, b)) {
                        // nan
                    }
                }
            }
        }

        if (v->active && !v->paused && !ndspChnIsPlaying(v->channel)) {
            free_voice(v);
            continue;
        }

        if (v->fade_target >= 0) {
            v->gain += v->fade_step;
            bool done = (v->fade_step >= 0 && v->gain >= v->fade_target) ||
                        (v->fade_step < 0 && v->gain <= v->fade_target);
            if (done) {
                v->gain = v->fade_target;
                v->fade_target = -1;
            }
            set_channel_mix(v->channel, v->base_gain * v->gain * g_master_gain);
        }
    }
}

void kwik_audio_shutdown() {
    for (int i = 0; i < kMaxChannels; ++i)
        if (g_voices[i]) free_voice(g_voices[i]);
    if (g_ndsp_ok) {
        ndspExit();
        g_ndsp_ok = false;
        g_ndsp_tried = false;
    }
}

}
