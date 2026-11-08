#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core.hpp"
#include "game.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "remote.hpp"
#include "rtti.hpp"
#include "trainer.hpp"
#include "vox.hpp"

namespace tr::core::detail {

struct Config {
    // 0 = no key
    int vk_toggle = 0, vk_repeat = 0, vk_set_start = 0, vk_set_end = 0;
    int vk_jump_start = 0, vk_prev = 0, vk_next = 0, vk_clear = 0;
    int vk_speed_up = 0, vk_speed_down = 0;
    int vk_pause = 0, vk_scale_windows = 0, vk_fixed_scroll = 0, vk_no_camera = 0;
    int vk_overlay = VK_OEM_3;
    int enabled_on_start = 0;
    int preroll_measures = 1;
    double speed_mod_step = TrainerConfig{}.speed_mod_step;
    double speed_mod_min = TrainerConfig{}.speed_mod_min;
    double speed_mod_max = TrainerConfig{}.speed_mod_max;
};

struct VoiceObs {
    void* voice;
    float t;
    double ms;
    double rate;  // voice ms advanced per timer ms over the last interval
    int good;  // consecutive intervals with rate ~1
};

struct WinEntry {
    float* p;
    float orig;
};

struct WinTable {
    std::vector<WinEntry> e;
    void* notes_begin = nullptr;
    size_t notes = 0;
    void* first_note = nullptr;
    double factor = 1.0;
    bool built = false, refused = false;
};

struct PlayerSnap {
    uint8_t* player;
    float gauge;
    uint8_t cleared, finished;
};

struct DrawClock {
    void* timer = nullptr;
    int search_wait = 0;
    bool known = false;
    int64_t prev = 0;
    bool run_prev = false;
    uint32_t stats_prev[2] = {};
    int64_t jump_ns = 0;  // timer time apply_speed added for the dropped stretch delay; the clock must follow
};

struct Session {
    // A world can be reallocated at the same address, so the session is keyed on its sub-objects too.
    void* world = nullptr;
    void* timer = nullptr;
    void* audio = nullptr;
    int refuse_cooldown = 0;
    bool waiting_logged = false;
    bool ready_logged = false;
    int frames_unready = 0;
    int bgm_searches = 0;
    std::vector<void*> excluded;  // voices already playing when the world appeared (menu music, preview)
    std::vector<VoiceObs> obs;
    int drift_frames = 0;
    VoxTimeline tl;
    bool tl_ok = false;
    void* bgm = nullptr;
    int bgm_rescan = 0;
    double offset = 0.0;  // timer time - chart time, smoothed
    bool offset_ok = false;
    int settle = 0;
    bool keys_prev[14] = {};
    bool referee_searched = false;
    void* objmgr = nullptr;
    void* referee = nullptr;
    int64_t last_ns = 0;
    bool sync_lock = false;  // speed was used: the timer follows the heard music, offset frozen
    bool speed_refused = false;
    double shift_speed = 1.0;
    DrawClock draw;
    int pause_req = 0;
    bool paused_eff = false;
    bool pause_pos_ok = false;
    double pause_pos = 0;
    double scale = 1.0;
    void* fumen_seen = nullptr;
    WinTable win;
    std::vector<PlayerSnap> snaps;
    std::vector<PlayerSnap> gauge0;
    bool forced_snap = false;
    int crash_undone = 0, restarts = 0;
    bool crash_logged = false, chord_logged = false;
    bool chord_down = false, chord_jump = false;
    int chord_blocked = 0;
    void* input = nullptr;
    int input_wait = 0;
    bool in_retry = false;
    bool restart_logged = false;
    double resume_at = -1;
    void* engine = nullptr;
    void* gworld = nullptr;
    bool camera_logged = false;
};

inline constexpr float kEndMargin = 0.2f;  // hold this long before the chart end time
inline constexpr double kMixRate = 44100.0;  // CSoundVoltexAudioGraph mixes at 44.1 kHz
using FnWorldFinished = uint8_t (*)(void*);

extern Module g_mod;
extern RttiIndex g_rtti;
extern GameApi g_api;
extern Config g_cfg;
extern Session g_s;
extern Trainer g_trainer;
extern FnMasterRender g_orig_master_render;
extern FnFeederRead g_orig_stretch_read;
extern FnSetRate g_stretch_set_rate;
extern std::atomic<bool> g_req_reset;
extern std::atomic<bool> g_stretching;
extern std::atomic<float> g_stretch_lag;
extern int64_t (*g_clock)();
extern void* g_lane_range;
extern float g_lane_v;
extern FnVoid g_orig_input_update;
extern bool g_prev_enabled;
extern FnVoid g_orig_crash_update;
extern FnWorldFinished g_orig_world_finished;
extern FnRotate g_orig_rotate;
extern FnSetF g_orig_cam_dist;
extern FnSetF g_orig_cam_pitch;
extern uint64_t g_last_frame_ms;
extern FnWorldInit g_orig_world_init;
extern FnVoid g_orig_morph;

void* field(void* obj, uint32_t off);
bool is_class(void* obj, const char* mangled);
void* fumen_of(void* world);
void* find_world_obj(void* world, const char* cls_name);

void note_active_voice(void* obj);
void master_rendered(void* master);
int hk_master_render(void* self, void* ctx, int frames);
std::vector<void*> active_voices();
bool voice_pos(void* v, double& ms);
int voice_bus(void* v);
bool excluded(void* v);
void* find_bgm(void* world, bool verbose, float t_now);
std::vector<void*> song_voices(void* world, void* bgm);
void clear_active_voices();

uint64_t hk_stretch_read(void* self, void* buf, int bytes);
void apply_speed(void* timer, double speed, float t_now);
void draw_follow(void* world, bool running);
void draw_remember();

int skip_notes(void* world, double from, double to);
int skip_and_hide_notes(void* world, double from, double to);
void show_notes();
void apply_windows(void* world, double factor);
void restore_lane();
void apply_lane();
void restore_offsets();
void apply_offsets();
void hk_input_update(void* self);

bool guard_active();
void hk_crash_update(void* self);
uint8_t hk_world_finished(void* self);
void undo_crash(void* world);
void snapshot_guard(void* world);
void remember_gauge(void* world, float t_now);
void refill_gauge(void* world);
float chart_end_time(void* world);
void handle_pause(void* world, float t_now, bool key);
void process_restart(void* world, double chart_now);
void refresh_chart_caches(void* world);

void hk_rotate(void* self, const float* center, float angle);
void hk_cam_dist(void* self, float v);
void hk_cam_pitch(void* self, float v);
void hk_morph(void* self);

float effects_time(void* fumen);
void rebuild_after_seek(void* fumen, float before);
void apply_camera();
void hk_world_init(void* world, void* arg);
bool set_fixed_scroll(void* world, bool on);

}  // namespace tr::core::detail
