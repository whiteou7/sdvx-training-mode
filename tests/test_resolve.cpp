#include <windows.h>

#include <cstdio>

#include "game.hpp"
#include "mem.hpp"
#include "rtti.hpp"

using namespace tr;

static int g_fail = 0, g_checks = 0;
#define EXPECT_EQ(a, b)                                                                                  \
    do {                                                                                                 \
        g_checks++;                                                                                      \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);                 \
        if (va_ != vb_) {                                                                                \
            g_fail++;                                                                                    \
            std::printf("FAIL %s:%d: %s = 0x%llx, expected 0x%llx\n", __FILE__, __LINE__, #a, va_, vb_); \
        }                                                                                                \
    } while (0)

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: test_resolve <soundvoltex.dll>\n");
        return 2;
    }
    HMODULE h = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!h) {
        std::printf("LoadLibraryEx failed: %lu\n", GetLastError());
        return 2;
    }
    Module m;
    if (!m.init(h)) {
        std::printf("bad PE\n");
        return 2;
    }
    const ULONGLONG t0 = GetTickCount64();
    RttiIndex rtti;
    rtti.build(m);
    std::printf("RTTI: %zu classes in %llu ms (base %p)\n", rtti.size(), (unsigned long long)(GetTickCount64() - t0),
                m.base);
    struct Expected {
        uint32_t stamp, classes;
        uint32_t world_update, off_tutorial, fumen_rebuild, tutorial_rebuild;
        uint32_t timer_get, timer_seek, timer_update;
        uint32_t voice_get, voice_set, voice_conns, voice_update, master_render;
        uint32_t referee_update, referee_timeout, stretch_read, draw_recompute;
        uint32_t state_playing, state_end, state_retry, audio_stop, counter_get, sp_release;
        uint32_t off_finished, crash_update, input_update, range_update, off_vox_path;
        uint32_t cold_part, cold_owner, mid_playing;
        uint32_t world_init, rotate, cam_dist, cam_pitch, cam_dist_value, cam_pitch_value;
    };
    static const Expected kBuilds[] = {
        {0x69ccbc0e, 2019, 0x47abe0, 0x178, 0x470100, 0x33d7f0, 0x1361d0, 0x135df0, 0x136070, 0x6c9e10, 0x6c9ed0,
         0x6caa10, 0x6ca0d0, 0x6cb5b0, 0x4577f0, 0x457c90, 0x64a720, 0x134d30, 0x47e1b0, 0x47eb30, 0x47ce40,
         0x4654e0, 0x476ce0, 0xb1ea0, 0x278, 0x34f5a0, 0x421f70, 0x1347e0, 0x430, 0x47ece5, 0x47eb30, 0x47e780,
         0x478550, 0x485cb0, 0x488cc0, 0x488d40, 0xabfed0, 0xabfed8},
        {0x6a8fe2c4, 2235, 0x4ba970, 0x158, 0x4afaa0, 0x373070, 0x14fc90, 0x14f8b0, 0x14fb30, 0x7184c0, 0x718580,
         0x7190c0, 0x718780, 0x719bd0, 0x497a20, 0x497ec0, 0x697230, 0x14e820, 0x4be000, 0x4be990, 0x4bcc40,
         0x4a5800, 0x4b6680, 0xb8720, 0x258, 0x3859c0, 0x45fd80, 0x14e2d0, 0x410, 0, 0, 0,
         0x4b7eb0, 0x4c6050, 0x4c9190, 0x4c9210, 0xb3c470, 0xb3c478},
    };
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    const Expected* x = nullptr;
    for (auto& b : kBuilds)
        if (b.stamp == nt->FileHeader.TimeDateStamp) x = &b;
    if (!x) {
        std::printf("unknown build (link time 0x%lx): only checking that resolution succeeds\n",
                    (unsigned long)nt->FileHeader.TimeDateStamp);
        x = &kBuilds[0];
    }
    const bool known = x->stamp == nt->FileHeader.TimeDateStamp;
    EXPECT_EQ(rtti.size(), known ? x->classes : rtti.size());

    GameApi api;
    bool ok = resolve_game(m, rtti, api);
    if (!ok) std::printf("resolve error: %s\n", api.error.c_str());
    EXPECT_EQ(ok, 1);
    if (!ok) return 1;
    EXPECT_EQ(api.vft_stretch != nullptr, 1);
    EXPECT_EQ(api.vft_draw != nullptr, 1);
    EXPECT_EQ(api.state_retry != nullptr, 1);
    EXPECT_EQ(api.vft_crash != nullptr, 1);
    EXPECT_EQ(api.off_players_map != 0, 1);
    EXPECT_EQ(api.off_chart_end != 0, 1);
    EXPECT_EQ(api.vft_input != nullptr, 1);
    EXPECT_EQ(api.vft_judge != nullptr, 1);
    EXPECT_EQ(api.vft_range != nullptr, 1);
    EXPECT_EQ(api.off_const_scroll != 0, 1);
    EXPECT_EQ(api.off_spc_list != 0, 1);
    EXPECT_EQ(api.off_morph_values, 0x18);
    EXPECT_EQ(api.vft_event != nullptr, 1);
    EXPECT_EQ(api.off_events, 0x80);
    EXPECT_EQ(api.off_event_time, 8);
    EXPECT_EQ(api.off_event_fn, 0x48);
    EXPECT_EQ(api.vft_engine != nullptr, 1);
    void* fake[1] = {api.vft_timer};
    EXPECT_EQ(rtti.class_of(fake) == cls::Timer, 1);
    void* bogus[1] = {reinterpret_cast<void*>(0x1234)};
    EXPECT_EQ(rtti.class_of(bogus).empty(), 1);

    const uintptr_t top = 0xFFFFFFFFFFFFFFF8ull;
    void* zero_vptr[1] = {nullptr};
    void* small_vptr[1] = {reinterpret_cast<void*>(8)};
    void* wrap_vptr[1] = {reinterpret_cast<void*>(top)};
    void* base_vptr[1] = {m.base};
    void* end_vptr[1] = {m.base + m.image_size - 4};
    void* code_vptr[1] = {static_cast<void*>(api.vft_world[0])};
    for (void** obj : {zero_vptr, small_vptr, wrap_vptr, base_vptr, end_vptr, code_vptr})
        EXPECT_EQ(rtti.class_of(obj).empty(), 1);
    EXPECT_EQ(rtti.class_of(nullptr).empty(), 1);
    EXPECT_EQ(rtti.class_of(reinterpret_cast<void*>(top)).empty(), 1);
    EXPECT_EQ(m.contains(reinterpret_cast<void*>(top), 8), 0);
    EXPECT_EQ(m.contains(m.base + m.image_size - 4, 8), 0);
    EXPECT_EQ(m.contains(m.base + m.image_size - 8, 8), 1);
    EXPECT_EQ(readable(reinterpret_cast<void*>(top), 8), 0);
    EXPECT_EQ(readable(nullptr, 1), 0);
    EXPECT_EQ(readable(reinterpret_cast<void*>(0x1000), 8), 0);
    EXPECT_EQ(readable(zero_vptr, sizeof(zero_vptr)), 1);

    if (!api.vft_stretch || !api.vft_crash || !api.vft_input || !api.vft_judge || !api.vft_range || !api.state_retry) {
        std::printf("%d checks, %d failed\n", g_checks, g_fail);
        return 1;
    }
    if (!known) {
        std::printf("%d checks, %d failed\n", g_checks, g_fail);
        return g_fail ? 1 : 0;
    }

    auto rva = [&](void* p) { return m.rva(p); };
    EXPECT_EQ(rva(api.vft_world[slot::WorldUpdate]), x->world_update);
    EXPECT_EQ(api.off_timer, 0xd8);
    EXPECT_EQ(api.off_tumami_l, 0x118);
    EXPECT_EQ(api.off_tumami_r, 0x128);
    EXPECT_EQ(api.off_fumen, 0xc8);
    EXPECT_EQ(api.off_tutorial, x->off_tutorial);
    EXPECT_EQ(rva(reinterpret_cast<void*>(api.fumen_rebuild)), x->fumen_rebuild);
    EXPECT_EQ(rva(reinterpret_cast<void*>(api.tutorial_rebuild)), x->tutorial_rebuild);
    EXPECT_EQ(rva(api.vft_timer[slot::TimerGetTime]), x->timer_get);
    EXPECT_EQ(rva(api.vft_timer[slot::TimerSeek]), x->timer_seek);
    EXPECT_EQ(rva(api.vft_timer[slot::TimerUpdate]), x->timer_update);
    EXPECT_EQ(rva(api.vft_voice[slot::VoiceGetPos]), x->voice_get);
    EXPECT_EQ(rva(api.vft_voice[slot::VoiceSetPos]), x->voice_set);
    EXPECT_EQ(rva(api.vft_voice[slot::VoiceConnCount]), x->voice_conns);
    EXPECT_EQ(rva(api.vft_voice_update[slot::VoiceUpdate]), x->voice_update);
    EXPECT_EQ(rva(api.vft_master_render[slot::MasterRender]), x->master_render);
    EXPECT_EQ(api.off_active_begin, 0x478);
    EXPECT_EQ(api.off_active_end, 0x480);
    EXPECT_EQ(api.vft_tumami != nullptr, 1);
    EXPECT_EQ(api.vft_fumen != nullptr, 1);
    EXPECT_EQ(api.vft_audio != nullptr, 1);
    EXPECT_EQ(api.off_timer_running, 0x10);
    EXPECT_EQ(api.off_timer_acc, 0x18);
    EXPECT_EQ(rva(api.vft_referee[slot::RefereeUpdate]), x->referee_update);
    EXPECT_EQ(rva(api.vft_referee[slot::RefereeTimeout]), x->referee_timeout);
    EXPECT_EQ(api.off_referee_now, 0x118);
    EXPECT_EQ(rva(api.vft_stretch[slot::FeederRead]), x->stretch_read);
    EXPECT_EQ(api.off_stretch_source, 0x8010);
    EXPECT_EQ(api.off_draw_clock, 0x38);
    EXPECT_EQ(api.off_draw_stats, 0x70);
    EXPECT_EQ(api.off_draw_table, 0x58);
    EXPECT_EQ(rva(reinterpret_cast<void*>(api.draw_recompute)), x->draw_recompute);
    EXPECT_EQ(api.off_vox_path, x->off_vox_path);
    EXPECT_EQ(rva(api.vft_world[slot::WorldInit]), x->world_init);
    EXPECT_EQ(api.off_world_vox, 0x88);
    EXPECT_EQ(api.off_vox_data, 8);
    EXPECT_EQ(api.off_const_scroll, 0x20);
    EXPECT_EQ(api.fd_rep != nullptr, 1);
    EXPECT_EQ(api.off_world_fdata, 0x98);
    EXPECT_EQ(api.off_fd_table, 0x50);
    EXPECT_EQ(api.off_fd_const, 0x120);
    EXPECT_EQ(api.fd_stride, 0x14);
    EXPECT_EQ(api.fd_bpm, 4);
    EXPECT_EQ(api.fd_mult, 0xc);
    EXPECT_EQ(api.fd_stop, 8);
    EXPECT_EQ(api.off_spc_list, 0xb0);
    EXPECT_EQ(api.spc_lane_y, 0x12);
    EXPECT_EQ(api.spc_morph0, 0x15);
    EXPECT_EQ(rva(api.vft_engine[slot::EngineRotate]), x->rotate);
    EXPECT_EQ(rva(api.vft_gworld[slot::CamDistance]), x->cam_dist);
    EXPECT_EQ(rva(api.vft_gworld[slot::CamPitch]), x->cam_pitch);
    EXPECT_EQ(rva(const_cast<float*>(api.cam_distance)), x->cam_dist_value);
    EXPECT_EQ(rva(const_cast<float*>(api.cam_pitch)), x->cam_pitch_value);
    EXPECT_EQ(int(*api.cam_distance * 100 + 0.5f), 6012);
    EXPECT_EQ(int(*api.cam_pitch * 100 + 0.5f), 4128);

    EXPECT_EQ(rva(const_cast<void*>(api.state_playing)), x->state_playing);
    EXPECT_EQ(rva(const_cast<void*>(api.state_end)), x->state_end);
    EXPECT_EQ(rva(const_cast<void*>(api.state_retry)), x->state_retry);
    EXPECT_EQ(rva(reinterpret_cast<void*>(api.audio_stop)), x->audio_stop);
    EXPECT_EQ(rva(reinterpret_cast<void*>(api.counter_get)), x->counter_get);
    EXPECT_EQ(rva(reinterpret_cast<void*>(api.sp_release)), x->sp_release);
    EXPECT_EQ(api.off_state_fn, 0x18);
    EXPECT_EQ(api.off_pending, 0x48);
    EXPECT_EQ(api.off_subreq, 0x64);
    EXPECT_EQ(api.off_context, 0xa8);
    EXPECT_EQ(api.off_finished, x->off_finished);
    EXPECT_EQ(rva(api.vft_crash[slot::CrashUpdate]), x->crash_update);
    EXPECT_EQ(api.off_crash_held, 0x10);
    EXPECT_EQ(api.off_crash_time, 0x14);
    EXPECT_EQ(api.off_players_map, 0x60);
    EXPECT_EQ(api.off_players_mapsize, 0x68);
    EXPECT_EQ(api.off_players_first, 0x70);
    EXPECT_EQ(api.off_players_count, 0x78);
    EXPECT_EQ(api.off_forced_crash, 0x99);
    EXPECT_EQ(api.off_chart_end, 0x170);
    EXPECT_EQ(api.off_chart_over, 0x174);
    EXPECT_EQ(rva(api.vft_input[slot::InputUpdate]), x->input_update);
    EXPECT_EQ(rva(api.vft_judge[slot::JudgeGetTime]) != 0, 1);
    EXPECT_EQ(rva(api.vft_range[1]), x->range_update);
    EXPECT_EQ(api.off_draw_range, 0x48);
    EXPECT_EQ(api.off_range_speed, 0x10);
    EXPECT_EQ(api.off_draw_offs, 0x18);
    EXPECT_EQ(api.off_judge_offs, 0x18);
    EXPECT_EQ(api.judge_offs_at, 0xc);
    if (x->cold_part) {
        EXPECT_EQ(rva(const_cast<uint8_t*>(m.function_start(m.base + x->cold_part))), x->cold_owner);
        EXPECT_EQ(rva(const_cast<uint8_t*>(m.function_start(m.base + x->mid_playing))), x->state_playing);
    }
    EXPECT_EQ(m.function_start(m.base + 0x10) == nullptr, 1);

    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
