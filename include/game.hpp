#pragma once

#include <cstdint>
#include <string>

#include "rtti.hpp"

namespace tr {

namespace cls {
// RTTI names of the game classes that are hooked or read.
inline constexpr const char* World = ".?AVGameWorldKFC@Game@@";
inline constexpr const char* Timer = ".?AVGameTimer@Game@@";
inline constexpr const char* Voice = ".?AVVoiceImpl@BMSoundLib2017@@";
inline constexpr const char* Audio = ".?AVGameAudio@Game@@";
inline constexpr const char* Fumen = ".?AVGameFumenPlayer@Game@@";
inline constexpr const char* Tumami = ".?AVGameInputTumamiCursor@Game@@";
inline constexpr const char* Tutorial = ".?AVTutorialEventPlayer@Game@@";
inline constexpr const char* VoiceMaster = ".?AVVoiceMasterImpl@BMSoundLib2017@@";
inline constexpr const char* Referee = ".?AVNoteReferee@Game@@";
inline constexpr const char* Draw = ".?AVDrawTimer@Game@@";
inline constexpr const char* DrawRange = ".?AVDrawRangeEx@Game@@";
inline constexpr const char* Judge = ".?AVJudgeTimer@Game@@";
inline constexpr const char* Input = ".?AVGameInput@Game@@";
inline constexpr const char* ForceCrash = ".?AVGameInputCheckRequestForceTrackCrash@Game@@";
inline constexpr const char* Stretch = ".?AV?$TimeStretchAudioFeeder@UF32Stereo@BMSoundLib2017@@M$01@BMSoundLib2017@@";
inline constexpr const char* Stretch2 = ".?AV?$TimeStretchAudioFeeder@M$01@BMSoundLib2017@@";  // name used by builds from 2026-08-27 on
inline constexpr const char* Engine = ".?AVPseudo3dEngine@Graphics@@";
inline constexpr const char* GWorld = ".?AVPseudo3dGameWorld@Graphics@@";
inline constexpr const char* Morph = ".?AVLaneMorphingUpdater@Game@@";
inline constexpr const char* Event = ".?AVEventObject@Game@@";
inline constexpr const char* Chip = ".?AVButtonNote@Game@@";
inline constexpr const char* Hold = ".?AVLongButtonNote@Game@@";
inline constexpr const char* Laser = ".?AVTumamiNote@Game@@";
}  // namespace cls

namespace slot {
inline constexpr int WorldInit = 2;
inline constexpr int WorldUpdate = 3;
inline constexpr int TimerUpdate = 7;  // called first thing in WorldUpdate (call [rax+0x38])
inline constexpr int TimerGetTime = 2;  // returns the base time plus any pending seek
inline constexpr int TimerSeek = 6;  // Seek(const float* delta), also notifies the seek listeners
inline constexpr int TumamiReset = 3;
inline constexpr int VoiceConnCount = 23;  // number of mixer connections. GetPosition reads the first one
inline constexpr int VoiceGetPos = 14;
inline constexpr int VoiceSetPos = 15;  // the audio thread applies the new position later
inline constexpr int VoiceUpdate = 1;
inline constexpr int MasterRender = 1;  // audio thread mix callback. It calls each voice's update directly, so voices are watched here
inline constexpr int RefereeUpdate = 1;
inline constexpr int RefereeTimeout = 3;  // marks the note's first expired tick as a miss
inline constexpr int FeederRead = 1;
inline constexpr int StretchSetRate = 2;  // takes effect on the next read. A rate of 1 turns stretching off and 0 mutes
inline constexpr int DrawGetTime = 3;  // clock * 1e-9 + display offset
inline constexpr int DrawStats = 4;
inline constexpr int DrawSetTable = 5;  // stores the scroll table and recalculates the scroll position
inline constexpr int WorldFinished = 6;  // the scene reads the result once this returns true
inline constexpr int InputUpdate = 1;
inline constexpr int CrashUpdate = 1;
inline constexpr int TimerStop = 4;  // the game's quick retry calls this first
inline constexpr int JudgeGetTime = 2;
inline constexpr int EngineRotate = 6;
inline constexpr int EngineCenter = 7;
inline constexpr int CamDistance = 5;
inline constexpr int CamPitch = 7;
inline constexpr int MorphUpdate = 1;
inline constexpr int EventFire = 1;
inline constexpr int EventTime = 2;  // in draw timer seconds
inline constexpr int NoteDraw = 5;  // the same slot in ButtonNote, LongButtonNote and TumamiNote
}  // namespace slot

using FnVoid = void (*)(void*);
using FnGetF = float (*)(void*);
using FnSeek = void (*)(void*, const float*);
using FnGetD = double (*)(void*);
using FnSetD = int (*)(void*, double);
using FnSizeT = uint64_t (*)(void*);
using FnMasterRender = int (*)(void* self, void* ctx, int frames);
using FnFeederRead = uint64_t (*)(void* self, void* buf, int bytes);
using FnSetRate = void (*)(void* self, float rate);
using FnRefereeTimeout = void* (*)(void* self, void* out, void* note_sp, float t);
using FnDrawSetTable = void (*)(void* self, void* table_sp);
using FnWorldInit = void (*)(void* world, void* arg);
using FnRotate = void (*)(void* self, const float* center, float angle);
using FnSetF = void (*)(void* self, float v);
// Retry counter: get(ctx, out) fills out with {px, ctrl}. The caller increments *px and then releases out.
using FnCounterGet = void** (*)(void* ctx, void** out);

struct GameApi {
    void** vft_world = nullptr;
    void** vft_timer = nullptr;
    void** vft_voice = nullptr;
    void** vft_voice_update = nullptr;
    void** vft_master_render = nullptr;
    // vector<shared_ptr<IVoiceUpdate>> on VoiceMasterImpl+16. Only the audio thread changes it, inside MasterRender
    uint32_t off_active_begin = 0;
    uint32_t off_active_end = 0;
    void** vft_audio = nullptr;
    void** vft_fumen = nullptr;
    void** vft_tumami = nullptr;
    void** vft_referee = nullptr;
    uint32_t off_referee_now = 0;

    // While running, GameTimer time = (acc + now_ns - start_ns) * 1e-9. Playback speed adjusts acc every frame.
    uint32_t off_timer_running = 0;
    uint32_t off_timer_acc = 0;

    // Everything below is optional. If a pattern isn't found, only the feature that needs it is turned off.
    void** vft_stretch = nullptr;
    uint32_t off_stretch_source = 0;

    void** vft_draw = nullptr;
    uint32_t off_draw_clock = 0;  // int64 ns. The previous frame's value is 8 bytes before it
    uint32_t off_draw_table = 0;
    uint32_t off_draw_stats = 0;
    FnDrawSetTable draw_recompute = nullptr;

    uint32_t off_timer = 0;
    uint32_t off_tumami_l = 0;
    uint32_t off_tumami_r = 0;
    uint32_t off_fumen = 0;
    uint32_t off_tutorial = 0;
    uint32_t off_audio = 0xb8;  // checked against the class at runtime
    uint32_t off_vox_path = 0x430;  // found from the "VoxFile Target:%s" log call. Later builds use 0x410

    FnVoid fumen_rebuild = nullptr;  // also runs NoteReferee::RewindJudgementsAfter(now)
    FnVoid tutorial_rebuild = nullptr;

    // State machine: the current and pending {fn, adj} (pending runs on the next Update), plus a sub-state request {int, flag}.
    uint32_t off_state_fn = 0, off_pending = 0, off_subreq = 0;
    uint32_t off_context = 0;
    const void* state_playing = nullptr;
    const void* state_end = nullptr;
    const void* state_retry = nullptr;
    FnVoid audio_stop = nullptr;
    FnCounterGet counter_get = nullptr;
    FnVoid sp_release = nullptr;

    uint32_t off_finished = 0;
    void** vft_crash = nullptr;
    uint32_t off_crash_held = 0, off_crash_time = 0;
    uint32_t off_players_map = 0, off_players_mapsize = 0, off_players_first = 0, off_players_count = 0;  // deque of players. Each player starts with {begin, end} of 12-byte gauge records
    uint32_t off_forced_crash = 0;
    uint32_t off_chart_end = 0, off_chart_over = 0;

    void** vft_input = nullptr;
    void** vft_judge = nullptr;
    VfTable chip{}, hold{}, laser{};
    uint32_t off_draw_offs = 0, off_judge_offs = 0, judge_offs_at = 0;  // both timers read the timing offsets through a pointer to the same float block
    void** vft_range = nullptr;
    uint32_t off_draw_range = 0;
    uint32_t off_range_speed = 0;

    uint32_t off_world_vox = 0, off_vox_data = 0, off_const_scroll = 0;  // world -> CVoxFile -> parsed data. Setting ConstantScroll before world init makes the scroll constant
    uint32_t off_world_fdata = 0, off_fd_table = 0, off_fd_const = 0, fd_stride = 0, fd_bpm = 0, fd_mult = 0, fd_stop = 0;
    FnGetF fd_rep = nullptr;

    uint32_t off_spc_list = 0;  // std::list of #SPCONTROLER records in the parsed vox. The ids are the game's keyword ids
    int spc_lane_y = 0, spc_morph0 = 0;

    void** vft_engine = nullptr;
    void** vft_gworld = nullptr;
    const float* cam_distance = nullptr;
    const float* cam_pitch = nullptr;
    void** vft_morph = nullptr;
    uint32_t off_morph_values = 0;  // two morph floats that GameLaneView uses to shape the lane

    void** vft_event = nullptr;
    uint32_t off_events = 0, off_event_time = 0, off_event_fn = 0;  // the object manager's vector<shared_ptr<EventObject>>. fn is the std::function target

    std::string error;
    bool ok() const { return error.empty(); }
};

bool resolve_game(const Module& m, const RttiIndex& rtti, GameApi& api);

}  // namespace tr
