#pragma once

#include <windows.h>

#include <string>

#include "game.hpp"
#include "rtti.hpp"
#include "trainer.hpp"
#include "vox.hpp"

namespace tr::core {

void set_dir(const std::wstring& dir);
void load_config();
bool init(HMODULE game);
bool install_hook();
void frame(void* world);

// Test hooks.
void set_key_override(const Keys* keys);
bool key_edge(bool& prev, bool focus, short st, bool tap_counts);
void forget_voices();
void voice_playing(void* voice);
void mixer_rendered(void* master);
void set_clock(int64_t (*ns)());
uint64_t stretch_read(void* self, void* buf, int bytes);
void set_stretch_original(FnFeederRead read, FnSetRate set_rate);
double stretch_lag();
void input_update(void* self, FnVoid orig);
void crash_update(void* self, FnVoid orig);
uint8_t world_finished(void* self, uint8_t (*orig)(void*));
void effects_rebuild(void* fumen, float before);
void world_init(void* world, void* arg, FnWorldInit orig);
bool fixed_scroll(void* world, bool on);
void set_fumen_rebuild(FnVoid fn);
void forget_session();
void set_audio_stop(FnVoid fn);
void set_config(const char* key, int value);  // vk_pause
struct Guards {
    int pause_req;
    bool paused;
    double scale;
    double window_factor;
    size_t window_bounds;
    bool lane_held;
    int crash_undone, chord_blocked, restarts;
    float lane_multiplier;
};
Guards guards();
const Trainer& trainer();
const GameApi& api();
const RttiIndex& rtti();
struct State {
    void* bgm;
    double offset;
    bool offset_ok;
    const VoxTimeline* timeline;
    bool sync_lock;
};
State state();

}  // namespace tr::core
