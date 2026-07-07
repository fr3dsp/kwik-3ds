#include "gml_runtime.h"
#include "engine_internal.h"

#include <string>
#include <vector>

namespace gml {

static const int kStreamBase = 200000;
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

GMLFN(audio_play_sound) { (void)self; (void)args; (void)argc; return Value(-1.0); }
GMLFN(audio_stop_sound) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_stop_all) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_is_playing) { (void)self; (void)args; (void)argc; return Value(0.0); }
GMLFN(audio_pause_sound) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_resume_sound) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_pause_all) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_resume_all) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_sound_gain) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_sound_pitch) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_sound_get_track_position) { (void)self; (void)args; (void)argc; return Value(0.0); }
GMLFN(audio_sound_set_track_position) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_set_master_gain) { (void)self; (void)args; (void)argc; return Value(); }
GMLFN(audio_create_stream) {
    (void)self;
    if (argc < 1) return Value(-1.0);
    return Value((double)kwik_stream_register((std::string)args[0]));
}
GMLFN(audio_destroy_stream) { return audio_stop_sound(self, args, argc); }
GMLFN(audio_group_is_loaded) { (void)self; (void)args; (void)argc; return Value(1.0); }
GMLFN(audio_group_load) { (void)self; (void)args; (void)argc; return Value(1.0); }
GMLFN(audio_group_set_gain) { (void)self; (void)args; (void)argc; return Value(); }

double kwik_voice_gain(int) { return 1.0; }
double kwik_voice_pitch(int) { return 1.0; }
bool kwik_voice_paused(int) { return false; }
double kwik_sound_length_seconds(int) { return 0.0; }
void kwik_audio_update() {}
void kwik_audio_shutdown() {}

} // namespace gml
