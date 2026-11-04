#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core.hpp"
#include "log.hpp"
#include "mem.hpp"
#include "remote.hpp"

#include "check.hpp"

using namespace tr;

template <size_t N>
struct alignas(16) Obj {
    uint8_t b[N] = {};
    template <typename T>
    T& f(size_t off) { return *reinterpret_cast<T*>(b + off); }
    void* p() { return b; }
};

struct BuildAddrs {
    uint32_t stamp, upd_vft, seek_vft, real_ns, frame_ns, classify;
};
static const BuildAddrs kBuildAddrs[] = {
    {0x69ccbc0e, 0x880a90, 0x880b00, 0xb27ff0, 0xb27ff8, 0x458d70},
    {0x6aa256e6, 0x8d7d68, 0x8d7d30, 0xbb4140, 0xbb4148, 0x49be40},
};
static const BuildAddrs* g_addrs = nullptr;

constexpr double kRate = 48000.0;
constexpr double kDt = 1.0 / 120.0;
constexpr double kLeadIn = 2.0;
constexpr double kMix = 44100.0;

static double g_T = 0;
static int64_t test_clock() { return int64_t(g_T * 1e9); }
static int64_t g_pulled = 0;
static uint64_t bus_read(void*, void* buf, int bytes) {
    std::memset(buf, 0, size_t(bytes));
    g_pulled += bytes / 8;
    return uint64_t(bytes);
}
static void* g_bus_vft[2] = {nullptr, reinterpret_cast<void*>(&bus_read)};
static float g_node_queued = 1.0f, g_node_rate = 1.0f;
static double g_node_due = 0;
static int g_node_ons = 0;
static void node_set_rate(void*, float rate) { g_node_queued = rate; }
static uint64_t node_read(void* self, void* buf, int bytes) {
    void* src = *reinterpret_cast<void**>(static_cast<uint8_t*>(self) + 0x8010);
    auto rd = reinterpret_cast<FnFeederRead>((*static_cast<void***>(src))[1]);
    if (g_node_rate == 1.0f && g_node_queued != 1.0f) g_node_ons++, g_node_due = 1024;
    g_node_rate = g_node_queued;
    if (g_node_rate == 1.0f) return rd(src, buf, bytes);
    g_node_due += bytes / 8 * double(g_node_rate);
    static float in[4096 * 2];
    for (int k; (k = std::min(int(g_node_due), 4096)) > 0; g_node_due -= k) rd(src, in, k * 8);
    std::memset(buf, 0, size_t(bytes));
    return uint64_t(bytes);
}

struct Rig {
    Obj<0x800> world;
    Obj<0x100> timer;
    Obj<0x180> audio;
    Obj<0x100> voice;
    Obj<0x60> wave;
    WAVEFORMATEX fmt{};
    void* conns[1] = {reinterpret_cast<void*>(0x1)};
    void* elems[2] = {};
    std::string path = "data/music/0001_albida_muryoku/0001_albida_muryoku_4i.vox";
    double t = 0.0;
    bool bgm_playing = true;
    Obj<0x40> zeroed;
    Obj<0x40> wraps;
    Obj<0x40> tiny;
    bool via_audio_thread = false;
    Obj<0x100> fx;
    Obj<0x100> chip;
    Obj<0x100> menu;
    Obj<0x100> stuck;
    Obj<0x500> mixer;
    void* mixer_list[12] = {};
    int frame_no = 0;
    Obj<0x40> conn;
    bool bus_mode = false;
    double T = 0;
    std::unique_ptr<Obj<0x8100>> stretch;
    Obj<0x10> bus;
    double mix_frac = 0, song_frac = 0;

    ~Rig() { core::forget_voices(); }

    void build(const GameApi& api, void** vft_wave) {
        conns[0] = conn.p();
        conn.f<uint32_t>(0x18) = 1;
        timer.f<void*>(0) = api.vft_timer;
        voice.f<void*>(0) = api.vft_voice;
        voice.f<void*>(0x38) = &conns[0];
        voice.f<void*>(0x40) = &conns[1];
        voice.f<void*>(0x50) = wave.p();
        voice.f<int>(0x90) = 0;
        wave.f<void*>(0) = vft_wave;
        wave.f<void*>(0x28) = &fmt;
        fmt.wFormatTag = 3;
        fmt.nChannels = 2;
        fmt.nSamplesPerSec = DWORD(kRate);
        fmt.nBlockAlign = 8;
        audio.f<void*>(0) = api.vft_audio;
        elems[0] = voice.p();
        audio.f<void*>(0x128) = &elems[0];
        audio.f<void*>(0x130) = &elems[2];
        audio.f<void*>(0x138) = &elems[2];
        world.f<void*>(0) = api.vft_world;
        world.f<void*>(api.off_timer) = timer.p();
        world.f<void*>(api.off_audio) = audio.p();
        world.f<const char*>(api.off_vox_path) = path.c_str();
        world.f<uint64_t>(api.off_vox_path + 16) = path.size();
        world.f<uint64_t>(api.off_vox_path + 24) = path.size();
        wraps.f<uint64_t>(0) = 0xFFFFFFFFFFFFFFF8ull;
        tiny.f<uint64_t>(0) = 8;
        world.f<void*>(0x1d8) = zeroed.p();
        world.f<void*>(0x1e0) = wraps.p();
        world.f<void*>(0x1e8) = tiny.p();
        world.f<uint64_t>(0x1f0) = 0xFFFFFFFFFFFFFFF8ull;
        audio.f<void*>(0x150) = zeroed.p();
        audio.f<void*>(0x158) = wraps.p();
        audio.f<uint64_t>(0x160) = 0xFFFFFFFFFFFFFFF0ull;
    }

    void use_audio_thread() {
        via_audio_thread = true;
        audio.f<void*>(0x130) = &elems[0];
        for (Obj<0x100>* v : {&fx, &chip, &menu, &stuck}) {
            *v = voice;
            v->f<int>(0x90) = 0;
        }
        menu.f<int>(0x90) = int(2.0 * kRate);
        stuck.f<int>(0x90) = int(5.0 * kRate);
    }

    void use_bus() {
        bus_mode = true;
        timer.f<uint8_t>(core::api().off_timer_running) = 1;
        stretch = std::make_unique<Obj<0x8100>>();
        stretch->f<void*>(0) = core::api().vft_stretch;
        stretch->f<void*>(core::api().off_stretch_source) = bus.p();
        bus.f<void*>(0) = g_bus_vft;
    }

    bool draw_mode = false;
    Obj<0x100> draw;
    Obj<0x40> weak_ctrl, range, range_ctrl, table, table_ctrl, upd_impl, seek_impl;
    float draw_off = 0;
    float draw_entry[5] = {0, 1.0f, 0, 1.0f, 0};
    uint8_t seek_list[0x48] = {};
    uint8_t* game_base = nullptr;
    int64_t frame_ns = 0;
    int64_t real_ns = 0;

    void use_draw(HMODULE game) {
        draw_mode = true;
        game_base = reinterpret_cast<uint8_t*>(game);
        frame_ns = int64_t(std::llround(kDt * 1e9));
        real_ns = frame_ns;
        *reinterpret_cast<int64_t*>(game_base + g_addrs->frame_ns) = frame_ns;
        draw.f<void*>(0) = core::api().vft_draw;
        weak_ctrl.f<int>(8) = 100;
        draw.f<void*>(0x08) = timer.p();
        draw.f<void*>(0x10) = weak_ctrl.p();
        draw.f<void*>(0x18) = &draw_off;
        draw.f<uint8_t>(0x28) = 1;
        draw.f<void*>(0x48) = range.p();
        draw.f<void*>(0x50) = range_ctrl.p();
        range_ctrl.f<int>(8) = 100;
        table.f<void*>(0) = draw_entry;
        table.f<void*>(8) = draw_entry + 5;
        table.f<uint8_t>(0x18) = 0;
        table_ctrl.f<int>(8) = 100;
        draw.f<void*>(0x58) = table.p();
        draw.f<void*>(0x60) = table_ctrl.p();
        upd_impl.f<void*>(0) = game_base + g_addrs->upd_vft;
        seek_impl.f<void*>(0) = game_base + g_addrs->seek_vft;
        void* owner = draw.p();
        std::memcpy(seek_list, &owner, 8);
        void* impl = seek_impl.p();
        std::memcpy(seek_list + 0x40, &impl, 8);
        timer.f<void*>(0x78) = seek_list;
        timer.f<void*>(0x80) = seek_list + 0x48;
        world.f<void*>(0xf8) = draw.p();
    }
    void draw_update() {
        *reinterpret_cast<int64_t*>(game_base + g_addrs->real_ns) = real_ns;
        void* owner = draw.p();
        const float tt = timer.f<float>(0x20);
        auto call = reinterpret_cast<void (*)(void*, void**, const float*)>((*reinterpret_cast<void***>(upd_impl.p()))[2]);
        call(upd_impl.p(), &owner, &tt);
    }
    double draw_clock() { return double(draw.f<int64_t>(0x38)) * 1e-9; }
    double draw_phase() { return draw_clock() - game_time(); }
    float draw_pos() { return range.f<float>(8); }

    Obj<0x200> fumen;
    Obj<0x400> objmgr;
    Obj<0x20> referee_obj;
    void* player_map[2] = {};
    Obj<0x10> block0, block1;
    Obj<0x40> player, player2;
    uint8_t recs[12 * 4] = {}, recs2[12 * 2] = {};
    struct TickMap {
        Obj<0x28> head, node[4];
    };
    Obj<0x100> note[2];
    uint8_t ticks[2][0x30] = {};
    TickMap maps[2], ref;
    uint8_t ref_tick[0x30] = {};
    void* note_list[4] = {};
    float hispeed = 2.0f;
    Obj<0x200> input;
    Obj<0x40> judge, judge_params, judge_ctrl;
    float presses[6][8] = {};
    Obj<0x100> ctx;
    Obj<0x20> ctx_ctrl;
    int counter = 5;
    static constexpr float kBounds[4] = {-0.10f, -0.05f, 0.05f, 0.10f};

    static void link(Obj<0x28>& n, void* left, void* parent, void* right, int kind, float bound) {
        n.f<void*>(0) = left;
        n.f<void*>(8) = parent;
        n.f<void*>(0x10) = right;
        n.f<int>(0x1c) = kind;
        n.f<float>(0x20) = bound;
    }
    static void build_map(TickMap& m, uint8_t* tick) {
        void* nil = m.head.p();
        m.head.f<uint8_t>(0x18) = 1;
        m.head.f<uint8_t>(0x19) = 1;
        link(m.node[0], nil, m.node[1].p(), nil, 1, kBounds[0]);
        link(m.node[1], m.node[0].p(), nil, m.node[2].p(), 2, kBounds[1]);
        link(m.node[2], nil, m.node[1].p(), m.node[3].p(), 3, kBounds[2]);
        link(m.node[3], nil, m.node[2].p(), nil, 4, kBounds[3]);
        m.head.f<void*>(0) = m.node[0].p();
        m.head.f<void*>(8) = m.node[1].p();
        m.head.f<void*>(0x10) = m.node[3].p();
        void* head = m.head.p();
        std::memcpy(tick + 8, &head, 8);
        const uint64_t size = 4;
        std::memcpy(tick + 16, &size, 8);
    }
    void use_fumen(const GameApi& api, float end_time) {
        fumen.f<void*>(0) = api.vft_fumen;
        fumen.f<void*>(0x18) = objmgr.p();
        block0.f<void*>(0) = player.p();
        player_map[0] = block0.p();
        player_map[1] = block1.p();
        fumen.f<void*>(api.off_players_map) = player_map;
        fumen.f<uint64_t>(api.off_players_mapsize) = 2;
        fumen.f<uint64_t>(api.off_players_first) = 0;
        fumen.f<uint64_t>(api.off_players_count) = 1;
        recs[8] = 1;
        recs[12 + 8] = 1;
        const float g0 = 100.0f, g1 = 60.0f;
        std::memcpy(recs + 4, &g0, 4);
        std::memcpy(recs + 12 + 4, &g1, 4);
        uint8_t* b = recs;
        uint8_t* e = recs + 12 * 2;
        uint8_t* cap = recs + 12 * 4;
        std::memcpy(player.b, &b, 8);
        std::memcpy(player.b + 8, &e, 8);
        std::memcpy(player.b + 16, &cap, 8);
        block1.f<void*>(0) = player2.p();
        uint8_t* b2 = recs2;
        uint8_t* e2 = recs2 + 12;
        std::memcpy(player2.b, &b2, 8);
        std::memcpy(player2.b + 8, &e2, 8);
        std::memcpy(player2.b + 16, &e2, 8);
        referee_obj.f<void*>(0) = api.vft_referee;
        objmgr.f<void*>(0x160) = referee_obj.p();
        objmgr.f<float>(api.off_chart_end) = end_time;
        const auto* bn = core::rtti().find(".?AVButtonNote@Game@@");
        void** v0 = nullptr;
        void** v8 = nullptr;
        for (auto& t : *bn) {
            if (t.offset == 0) v0 = t.vft;
            if (t.offset == 8) v8 = t.vft;
        }
        for (int i = 0; i < 2; i++) {
            note[i].f<void*>(0) = v0;
            note[i].f<void*>(8) = v8;
            uint8_t* tb = ticks[i];
            uint8_t* te = ticks[i] + 0x30;
            std::memcpy(note[i].b + 0x60, &tb, 8);
            std::memcpy(note[i].b + 0x68, &te, 8);
            std::memcpy(note[i].b + 0x70, &te, 8);
            build_map(maps[i], ticks[i]);
            note_list[2 * i] = note[i].p();
        }
        build_map(ref, ref_tick);
        void* nb = note_list;
        void* ne = note_list + 4;
        std::memcpy(objmgr.b + 0x50, &nb, 8);
        std::memcpy(objmgr.b + 0x58, &ne, 8);
        world.f<void*>(api.off_fumen) = fumen.p();
    }
    std::vector<float> bounds(int i) {
        std::vector<float> out;
        for (auto& n : maps[i].node) out.push_back(n.f<float>(0x20));
        return out;
    }
    void use_lane(const GameApi& api) {
        range.f<void*>(0) = api.vft_range;
        float* hs = &hispeed;
        std::memcpy(range.b + api.off_range_speed, &hs, 8);
    }
    float* lane_ptr(const GameApi& api) { return range.f<float*>(api.off_range_speed); }
    void use_input(const GameApi& api) {
        input.f<void*>(0) = api.vft_input;
        input.f<void*>(8) = judge.p();
        judge.f<void*>(0) = api.vft_judge;
        judge.f<void*>(8) = timer.p();
        judge.f<void*>(0x10) = judge_ctrl.p();
        judge_ctrl.f<int>(8) = 100;
        judge.f<void*>(0x18) = judge_params.p();
        for (int lane = 1; lane <= 6; lane++) {
            float* b = presses[lane - 1];
            float* cap = b + 8;
            uint8_t* v = input.b + 0x28 + lane * 0x18;
            std::memcpy(v, &b, 8);
            std::memcpy(v + 8, &b, 8);
            std::memcpy(v + 16, &cap, 8);
        }
        world.f<void*>(0x108) = input.p();
    }
    void use_restart(const GameApi& api) {
        ctx.f<void*>(0x68) = &counter;
        ctx.f<void*>(0x70) = ctx_ctrl.p();
        ctx_ctrl.f<int>(8) = 1;
        ctx_ctrl.f<int>(0xc) = 1;
        world.f<void*>(api.off_context) = ctx.p();
        world.f<const void*>(api.off_state_fn) = api.state_playing;
    }

    float game_time() { return reinterpret_cast<FnGetF>(core::api().vft_timer[slot::TimerGetTime])(timer.p()); }
    double voice_ms() { return voice.f<int>(0x90) * 1000.0 / kRate; }
    double chart() { return voice_ms() / 1000.0; }

    void step(const Keys& k = {}) {
        int song_adv = int(std::lround(kDt * kRate));
        if (bus_mode) {
            T += kDt;
            g_T = T;
            auto& acc = timer.f<int64_t>(core::api().off_timer_acc);
            if (timer.f<uint8_t>(0x28)) {
                acc += std::max(int64_t(timer.f<float>(0x24) * 1e9), -(int64_t(T * 1e9) + acc));
                timer.f<float>(0x24) = 0;
                timer.f<uint8_t>(0x28) = 0;
            }
            t = double(acc + int64_t(T * 1e9)) * 1e-9;
            timer.f<float>(0x20) = float(t);
            mix_frac += kDt * kMix;
            const int n = int(mix_frac);
            mix_frac -= n;
            std::vector<float> buf(size_t(n) * 2);
            const int64_t before = g_pulled;
            core::stretch_read(stretch->p(), buf.data(), n * 8);
            song_frac += double(g_pulled - before) * kRate / kMix;
            song_adv = int(song_frac);
            song_frac -= song_adv;
        } else {
            if (timer.f<uint8_t>(0x28)) {
                t += timer.f<float>(0x24);
                timer.f<float>(0x24) = 0;
                timer.f<uint8_t>(0x28) = 0;
            }
            t += kDt;
            timer.f<float>(0x20) = float(t);
        }
        if (bgm_playing && t >= kLeadIn) voice.f<int>(0x90) += song_adv;
        if (via_audio_thread) {
            if (bgm_playing && t >= kLeadIn) fx.f<int>(0x90) += song_adv;
            chip.f<int>(0x90) = (frame_no++ % 36) * int(std::lround(kDt * kRate));
            int n = 0;
            if (t < 1.0) {
                menu.f<int>(0x90) += int(std::lround(kDt * kRate));
                mixer_list[2 * n++] = menu.b + 8;
            }
            if (t >= 1.0) mixer_list[2 * n++] = stuck.b + 8;
            if (bgm_playing && t >= kLeadIn) {
                mixer_list[2 * n++] = voice.b + 8;
                mixer_list[2 * n++] = fx.b + 8;
                mixer_list[2 * n++] = chip.b + 8;
            }
            mixer.f<void*>(core::api().off_active_begin) = &mixer_list[0];
            mixer.f<void*>(core::api().off_active_end) = &mixer_list[2 * n];
            core::mixer_rendered(mixer.p());
        }
        if (draw_mode) draw_update();
        core::set_key_override(&k);
        core::frame(world.p());
        core::set_key_override(nullptr);
    }
    void run_until_chart(double c) {
        for (int i = 0; i < 200000 && chart() < c; i++) step();
    }
};

static bool queue_cmds(const char* cmds) {
    FILE* f = _wfopen(L"build\\remote_test\\sdvx_training.cmd", L"ab");
    if (!f) return false;
    std::string text = cmds;
    for (char& c : text)
        if (c == ' ') c = '\n';
    text += '\n';
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    return std::fclose(f) == 0 && ok;
}

static std::map<std::string, std::string> read_kv(const wchar_t* path) {
    std::map<std::string, std::string> kv;
    FILE* f = _wfopen(path, L"rb");
    if (!f) return kv;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (auto eq = s.find('='); eq != std::string::npos) kv[s.substr(0, eq)] = s.substr(eq + 1);
    }
    fclose(f);
    return kv;
}

static double timer_chart(Rig& r) { return r.game_time() - core::state().offset; }

static Keys key(bool Keys::*k) {
    Keys out;
    out.*k = true;
    return out;
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    std::printf("CRASH code=%08lx at %p\n", er->ExceptionCode, er->ExceptionAddress);
    if (er->NumberParameters >= 2)
        std::printf("  access kind %llu at %p\n", (unsigned long long)er->ExceptionInformation[0],
                    reinterpret_cast<void*>(er->ExceptionInformation[1]));
    void* bt[40];
    const USHORT n = CaptureStackBackTrace(0, 40, bt, nullptr);
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    for (USHORT i = 0; i < n; i++) std::printf("  frame %2u  image+0x%llx\n", i, (unsigned long long)(reinterpret_cast<uintptr_t>(bt[i]) - base));
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    SetUnhandledExceptionFilter(crash_filter);
    HMODULE game = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!game) return 2;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const uint8_t*>(game) +
                                                                  reinterpret_cast<const IMAGE_DOS_HEADER*>(game)->e_lfanew);
    for (const BuildAddrs& b : kBuildAddrs)
        if (b.stamp == nt->FileHeader.TimeDateStamp) g_addrs = &b;
    if (!g_addrs)
        std::printf("SKIP: the note-scroll and training-guard sections need the addresses of build 0x%lx (kBuildAddrs)\n",
                    (unsigned long)nt->FileHeader.TimeDateStamp);
    CreateDirectoryW(L"build", nullptr);
    DeleteFileW(L"build\\test_frame.log");
    log_init(L"build\\test_frame.log");
    core::set_dir(L"build\\no_such_dir");
    core::load_config();
    CHECK(core::init(game));
    void** vft_wave = core::rtti().primary(".?AVWaveDataImpl@BMSoundLib2017@@");
    CHECK(vft_wave != nullptr);

    auto rig = std::make_unique<Rig>();
    Rig& r = *rig;
    r.build(core::api(), vft_wave);

    const double M = 4 * 60.0 / 156.0;
    r.step();
    auto st = core::state();
    CHECK(st.timeline != nullptr);
    if (!st.timeline) return 1;
    CHECK_NEAR(st.timeline->start_of(1), M, 1e-9);
    CHECK(st.timeline->end_measure == 68);

    r.step(key(&Keys::toggle));
    CHECK(core::trainer().enabled());
    r.step(key(&Keys::set_end));
    CHECK(core::trainer().end() == st.timeline->count() - 1);
    CHECK(core::state().bgm == nullptr);

    while (r.t < kLeadIn + 3.0) r.step();
    st = core::state();
    CHECK(st.bgm == r.voice.p());
    CHECK(st.offset_ok);
    CHECK_NEAR(st.offset, kLeadIn, 2 * kDt);

    r.run_until_chart(2 * M + 0.1);
    r.step(key(&Keys::set_start));
    CHECK(core::trainer().start() == 2);
    r.run_until_chart(3 * M + 0.1);
    r.step(key(&Keys::set_end));
    CHECK(core::trainer().end() == 4);

    r.run_until_chart(4 * M - 0.05);
    bool looped = false;
    for (int i = 0; i < 100 && !looped; i++) {
        r.step();
        looped = r.chart() < 2 * M;
    }
    CHECK(looped);
    CHECK_NEAR(r.chart(), 1 * M, 2.0 / kRate);
    CHECK_NEAR(timer_chart(r), 1 * M, 1e-3);
    r.step();
    CHECK_NEAR(timer_chart(r) - r.chart(), 0.0, 1e-3);

    int loops = 0;
    double last = r.chart();
    for (int i = 0; i < int(10 * M / kDt); i++) {
        r.step();
        if (r.chart() < last - 0.5) loops++;
        last = r.chart();
    }
    CHECK(loops >= 3);

    r.step(key(&Keys::repeat));
    CHECK(!core::trainer().repeat());
    r.run_until_chart(6 * M);
    CHECK(r.chart() >= 6 * M);

    r.step(key(&Keys::jump_start));
    CHECK_NEAR(r.chart(), 1 * M, 2.0 / kRate);
    CHECK_NEAR(timer_chart(r), 1 * M, 1e-3);
    r.run_until_chart(1 * M + 0.6);
    r.step(key(&Keys::next));
    CHECK_NEAR(r.chart(), 2 * M, 2.0 / kRate);
    r.step();
    r.step(key(&Keys::prev));
    CHECK_NEAR(r.chart(), 1 * M, 2.0 / kRate);
    CHECK_NEAR(timer_chart(r), 1 * M, 1e-3);
    CHECK_NEAR(timer_chart(r) - r.chart(), 0.0, 1e-3);

    {
        CreateDirectoryW(L"build\\remote_test", nullptr);
        DeleteFileW(L"build\\remote_test\\sdvx_training.cmd");
        remote::init(L"build\\remote_test");
        CHECK(queue_cmds("clear set_start"));
        remote::poll_once();
        for (int i = 0; i < 3; i++) r.step();
        CHECK(core::trainer().start() == core::state().timeline->measure_at(r.chart()));
        CHECK(GetFileAttributesW(L"build\\remote_test\\sdvx_training.cmd") == INVALID_FILE_ATTRIBUTES);
        r.run_until_chart(r.chart() + M);
        CHECK(queue_cmds("set_end"));
        remote::poll_once();
        r.step();
        const int start1 = core::trainer().start(), end1 = core::trainer().end();
        CHECK(end1 == start1 + 2);

        remote::write_status_once(true);
        auto st = read_kv(L"build\\remote_test\\sdvx_training.status");
        CHECK(st["in_song"] == "1");
        CHECK(st["song"] == "0001_albida_muryoku_4i");
        CHECK(st["training"] == "1" && st["repeat"] == "0" && st["bgm"] == "1");
        CHECK(st["started"] == "1");
        CHECK(std::atoi(st["start"].c_str()) == start1 + 1 && std::atoi(st["end"].c_str()) == end1);
        CHECK(std::atoi(st["measures"].c_str()) == 69);
        CHECK(std::atoi(st["measure"].c_str()) == core::state().timeline->measure_at(r.chart()) + 1);

        CHECK(queue_cmds("jump_start"));
        remote::poll_once();
        r.step();
        CHECK_NEAR(r.chart(), core::state().timeline->start_of(start1 - 1), 2.0 / kRate);

        CHECK(queue_cmds("next repeat"));
        remote::poll_once();
        Sleep(1600);
        const double before_stale = r.chart();
        r.step();
        r.step();
        CHECK(r.chart() < before_stale + 0.1);
        CHECK(core::trainer().repeat());
        r.step(key(&Keys::repeat));
        CHECK(!core::trainer().repeat());

        Sleep(600);
        remote::write_status_once(true);
        st = read_kv(L"build\\remote_test\\sdvx_training.status");
        CHECK(st["in_song"] == "0" && st["training"] == "1");
        CHECK(queue_cmds("toggle"));
        remote::poll_once();
        remote::write_status_once(true);
        st = read_kv(L"build\\remote_test\\sdvx_training.status");
        CHECK(st["training"] == "0");
        r.step();
        CHECK(!core::trainer().enabled());
        r.step(key(&Keys::toggle));
        CHECK(core::trainer().enabled());
    }

    {
        bool prev = false;
        CHECK(core::key_edge(prev, true, short(0x8000), false));
        CHECK(!core::key_edge(prev, true, short(0x8000), false));
        CHECK(!core::key_edge(prev, true, 0, false));
        CHECK(core::key_edge(prev, true, short(0x8000), false));
        prev = false;
        CHECK(core::key_edge(prev, true, 1, true));
        CHECK(!core::key_edge(prev, true, 1, false));
        CHECK(!core::key_edge(prev, false, short(0x8001), true));
        CHECK(!prev);
        prev = true;
        core::key_edge(prev, false, short(0x8000), false);
        CHECK(core::key_edge(prev, true, short(0x8000), false));
    }

    r.step(key(&Keys::toggle));
    CHECK(!core::trainer().enabled());
    double before = r.chart();
    r.step(key(&Keys::jump_start));
    CHECK(r.chart() > before);

    r.run_until_chart(3.0);
    Obj<0x100> timer2;
    timer2.f<void*>(0) = core::api().vft_timer;
    r.world.f<void*>(core::api().off_timer) = timer2.p();
    core::frame(r.world.p());
    CHECK(core::trainer().start() == 0 && core::trainer().end() == core::state().timeline->count() - 1);
    CHECK(core::state().bgm == nullptr && !core::state().offset_ok);

    r.world.f<void*>(core::api().off_timer) = r.timer.p();
    r.bgm_playing = false;
    r.voice.f<int>(0x90) = 0;
    r.step();
    r.step(key(&Keys::toggle));
    for (int i = 0; i < 300; i++) r.step(i % 7 == 0 ? key(&Keys::jump_start) : Keys{});
    CHECK(core::state().bgm == nullptr);
    CHECK(core::trainer().start() == 0);

    {
        auto rig2 = std::make_unique<Rig>();
        Rig& q = *rig2;
        q.build(core::api(), vft_wave);
        q.use_audio_thread();
        q.step();
        if (!core::trainer().enabled()) q.step(key(&Keys::toggle));
        if (!core::trainer().repeat()) q.step(key(&Keys::repeat));
        CHECK(core::trainer().enabled() && core::trainer().repeat());
        while (q.t < kLeadIn + 3.0) q.step();
        void* bgm = core::state().bgm;
        CHECK(bgm == q.voice.p() || bgm == q.fx.p());
        CHECK_NEAR(core::state().offset, kLeadIn, 2 * kDt);
        q.run_until_chart(2 * M + 0.1);
        q.step(key(&Keys::set_start));
        q.run_until_chart(3 * M + 0.1);
        q.step(key(&Keys::set_end));
        bool looped = false;
        for (int i = 0; i < 2000 && !looped; i++) {
            q.step();
            looped = q.chart() < 2 * M;
        }
        CHECK(looped);
        const double fx_chart = q.fx.f<int>(0x90) / kRate;
        CHECK_NEAR(q.chart(), 1 * M, 2.0 / kRate);
        CHECK_NEAR(fx_chart, 1 * M, 2.0 / kRate);
        CHECK(q.chip.f<int>(0x90) / kRate < 0.4);
        CHECK_NEAR(timer_chart(q), 1 * M, 1e-3);
    }

    {
        core::set_clock(test_clock);
        core::set_stretch_original(node_read, node_set_rate);
        auto rig3 = std::make_unique<Rig>();
        Rig& z = *rig3;
        z.build(core::api(), vft_wave);
        z.use_audio_thread();
        z.use_bus();
        z.step();
        if (!core::trainer().enabled()) z.step(key(&Keys::toggle));
        if (core::trainer().repeat()) z.step(key(&Keys::repeat));
        while (z.t < kLeadIn + 3.0) z.step();
        CHECK(core::state().bgm != nullptr && core::state().offset_ok);
        auto sync_err = [&] {
            return (z.game_time() - core::state().offset) - (z.voice_ms() / 1000.0 - core::stretch_lag());
        };
        CHECK(std::fabs(sync_err()) < 0.003);

        for (int i = 0; i < 5; i++) z.step(key(&Keys::speed_down));
        CHECK(std::fabs(core::trainer().speed() - 0.75) < 1e-9);
        for (int i = 0; i < 120; i++) z.step();
        CHECK(core::state().sync_lock);
        CHECK(g_node_rate == 0.75f && g_node_ons == 1);
        double T0 = z.T, g0 = z.game_time(), v0 = z.voice_ms();
        auto mean_err = [&](int frames) {
            double sum = 0;
            for (int i = 0; i < frames; i++) z.step(), sum += sync_err();
            return sum / frames;
        };
        const double e0 = mean_err(120);
        for (int i = 0; i < int(8 / kDt); i++) z.step();
        const double e1 = mean_err(120);
        CHECK(std::fabs((z.game_time() - g0) / (z.T - T0) - 0.75) < 0.002);
        CHECK(std::fabs((z.voice_ms() - v0) / 1000 / (z.T - T0) - 0.75) < 0.002);
        CHECK(std::fabs(e1 - e0) < 0.001);
        CHECK(std::fabs(e0) < 0.004 && std::fabs(e1) < 0.004);
        if (std::fabs(e1 - e0) >= 0.001 || std::fabs(e1) >= 0.004)
            std::printf("  speed 0.75: mean sync error %.4f -> %.4f s\n", e0, e1);

        z.step(key(&Keys::prev));
        for (int i = 0; i < 30; i++) z.step();
        const double e2 = mean_err(120);
        CHECK(std::fabs(e2) < 0.004);
        CHECK(g_node_rate == 0.75f && g_node_ons == 2);
        if (std::fabs(e2) >= 0.004) std::printf("  speed 0.75 after a seek: mean sync error %.4f s\n", e2);

        for (int i = 0; i < 5; i++) z.step(key(&Keys::speed_up));
        CHECK(core::trainer().speed() == 1.0);
        for (int i = 0; i < int(3 / kDt); i++) z.step();
        CHECK(!core::state().sync_lock);
        T0 = z.T, g0 = z.game_time();
        for (int i = 0; i < int(2 / kDt); i++) z.step();
        CHECK(std::fabs((z.game_time() - g0) / (z.T - T0) - 1.0) < 0.005);
        CHECK(std::fabs(sync_err()) < 0.003);
        CHECK(g_node_rate == 1.0f);

        auto rig4 = std::make_unique<Rig>();
        Rig& w = *rig4;
        w.build(core::api(), vft_wave);
        w.use_audio_thread();
        w.use_bus();
        w.step();
        for (int i = 0; i < 5; i++) w.step(key(&Keys::speed_down));
        CHECK(std::fabs(core::trainer().speed() - 0.75) < 1e-9);
        T0 = w.T, g0 = w.game_time();
        while (w.t < kLeadIn - 0.2) w.step();
        CHECK(std::fabs((w.game_time() - g0) / (w.T - T0) - 1.0) < 0.001);
        while (!core::state().offset_ok && w.T < 20) w.step();
        CHECK(core::state().offset_ok);
        for (int i = 0; i < 120; i++) w.step();
        T0 = w.T, g0 = w.game_time();
        for (int i = 0; i < 600; i++) w.step();
        CHECK(std::fabs((w.game_time() - g0) / (w.T - T0) - 0.75) < 0.002);
        double sum = 0;
        for (int i = 0; i < 120; i++)
            w.step(), sum += (w.game_time() - core::state().offset) - (w.voice_ms() / 1000.0 - core::stretch_lag());
        CHECK(std::fabs(sum / 120) < 0.004);
        for (int i = 0; i < 5; i++) w.step(key(&Keys::speed_up));
        core::set_clock(nullptr);
    }

    if (g_addrs) {
        Sleep(700);
        core::set_clock(test_clock);
        core::set_stretch_original(node_read, node_set_rate);
        auto rig5 = std::make_unique<Rig>();
        Rig& y = *rig5;
        y.build(core::api(), vft_wave);
        y.use_audio_thread();
        y.use_bus();
        y.use_draw(game);
        y.step();
        if (!core::trainer().enabled()) y.step(key(&Keys::toggle));
        if (core::trainer().repeat()) y.step(key(&Keys::repeat));
        while (y.t < kLeadIn + 3.0) y.step();
        CHECK(core::state().bgm != nullptr && core::state().offset_ok);
        auto cs = [&](const char* what, double a, double b, double eps) {
            if (std::fabs(a - b) > eps) std::printf("  %s: %.5f, wanted %.5f\n", what, a, b);
            CHECK(std::fabs(a - b) <= eps);
        };

        cs("position at normal speed", y.draw_pos(), 8.0 * (y.draw_clock() + y.draw_off), 1e-3);

        for (int i = 0; i < 5; i++) y.step(key(&Keys::speed_down));
        CHECK(std::fabs(core::trainer().speed() - 0.75) < 1e-9);
        for (int i = 0; i < 120; i++) y.step();
        double T0 = y.T, c0 = y.draw_clock(), g0 = y.game_time(), p0 = y.draw_pos();
        const double ph0 = y.draw_phase();
        for (int i = 0; i < int(8 / kDt); i++) y.step();
        const double dT = y.T - T0;
        cs("timer rate", (y.game_time() - g0) / dT, 0.75, 0.002);
        cs("note clock rate", (y.draw_clock() - c0) / dT, 0.75, 0.002);
        cs("note position rate", (y.draw_pos() - p0) / dT, 8.0 * 0.75, 0.016);
        cs("position follows the clock", y.draw_pos(), 8.0 * (y.draw_clock() + y.draw_off), 1e-3);
        cs("clock stays on the timer", y.draw_phase() - ph0, 0.0, 0.002);

        y.real_ns = 3 * y.frame_ns;
        y.step();
        y.real_ns = y.frame_ns;
        CHECK(y.draw_phase() > 0 && y.draw_phase() <= 1.5 * kDt);
        const double ph_rs = y.draw_phase();
        T0 = y.T, c0 = y.draw_clock();
        for (int i = 0; i < int(4 / kDt); i++) y.step();
        cs("note clock rate after a resync", (y.draw_clock() - c0) / (y.T - T0), 0.75, 0.002);
        cs("clock stays on the timer after a resync", y.draw_phase() - ph_rs, 0.0, 0.002);

        y.step(key(&Keys::prev));
        for (int i = 0; i < 30; i++) y.step();
        const double ph_seek = y.draw_phase();
        CHECK(ph_seek > 0 && ph_seek <= 1.5 * kDt);
        T0 = y.T, c0 = y.draw_clock();
        for (int i = 0; i < int(4 / kDt); i++) y.step();
        cs("note clock rate after a seek", (y.draw_clock() - c0) / (y.T - T0), 0.75, 0.002);
        cs("clock stays on the timer after a seek", y.draw_phase() - ph_seek, 0.0, 0.002);

        const double ph_before = y.draw_phase();
        for (int i = 0; i < 5; i++) y.step(key(&Keys::speed_up));
        CHECK(core::trainer().speed() == 1.0);
        for (int i = 0; i < int(3 / kDt); i++) y.step();
        CHECK(!core::state().sync_lock);
        cs("clock stays on the timer after returning to 1", y.draw_phase() - ph_before, 0.0, 0.003);
        T0 = y.T, c0 = y.draw_clock();
        for (int i = 0; i < int(2 / kDt); i++) y.step();
        cs("note clock rate at 1", (y.draw_clock() - c0) / (y.T - T0), 1.0, 0.002);
        while (core::trainer().speed() < 0.999) y.step(key(&Keys::speed_up));
    }
    core::set_clock(nullptr);

    if (g_addrs) {
        Sleep(700);
        core::set_clock(test_clock);
        core::set_stretch_original(node_read, node_set_rate);
        const GameApi& api = core::api();
        const FnVoid real_rebuild = api.fumen_rebuild;
        core::set_fumen_rebuild([](void*) {});
        core::forget_session();
        auto rigG = std::make_unique<Rig>();
        Rig& g = *rigG;
        g.build(api, vft_wave);
        g.use_audio_thread();
        g.use_bus();
        g.use_draw(game);
        g.use_lane(api);
        g.use_input(api);
        g.use_fumen(api, 1.0e5f);
        g.step();
        if (!core::trainer().enabled()) g.step(key(&Keys::toggle));
        if (core::trainer().repeat()) g.step(key(&Keys::repeat));
        if (!core::trainer().scale_windows()) g.step(key(&Keys::scale_windows));
        while (!core::state().offset_ok && g.T < 30) g.step();
        for (int i = 0; i < 60; i++) g.step();
        CHECK(core::state().offset_ok);
        CHECK(core::guards().window_bounds == 0 && !core::guards().lane_held);
        const float orig_bounds[4] = {Rig::kBounds[0], Rig::kBounds[1], Rig::kBounds[2], Rig::kBounds[3]};
        auto classify = [&](float off, uint8_t* tick) {
            return reinterpret_cast<int (*)(float, void*)>(reinterpret_cast<uint8_t*>(game) + g_addrs->classify)(off, tick + 8);
        };
        auto range_width = [&] {
            float out[3] = {};
            reinterpret_cast<void (*)(void*, float*)>(api.vft_range[1])(g.range.p(), out);
            return double(out[2]) - double(out[0]);
        };
        const double width1 = range_width();
        CHECK_NEAR(width1, 3640.0 / g.hispeed, 1e-3);

        g.draw_off = 0.008f;
        g.judge_params.f<float>(api.judge_offs_at) = 0.008f;
        auto offsets = [&] {
            const double d = reinterpret_cast<FnGetF>(api.vft_draw[slot::DrawGetTime])(g.draw.p()) - g.draw_clock();
            const double j = reinterpret_cast<FnGetF>(api.vft_judge[slot::JudgeGetTime])(g.judge.p()) - g.game_time();
            return std::pair<double, double>(d, j);
        };
        g.step();
        CHECK_NEAR(offsets().first, 0.008, 1e-4);
        CHECK_NEAR(offsets().second, 0.008, 1e-4);

        for (int i = 0; i < 10; i++) g.step(key(&Keys::speed_down));
        CHECK(std::fabs(core::trainer().speed() - 0.5) < 1e-9);
        for (int i = 0; i < 60; i++) g.step();
        auto gs = core::guards();
        CHECK_NEAR(gs.scale, 0.5, 1e-9);
        CHECK_NEAR(gs.window_factor, 0.5, 1e-9);
        CHECK(gs.window_bounds == 8);
        for (int n = 0; n < 2; n++) {
            const auto b = g.bounds(n);
            for (int k = 0; k < 4; k++) CHECK(b[size_t(k)] == orig_bounds[k] * 0.5f);
        }
        for (float off : {-0.12f, -0.07f, -0.03f, 0.0f, 0.03f, 0.07f, 0.12f}) {
            CHECK(classify(off * 0.5f, g.ticks[0]) == classify(off, g.ref_tick));
            CHECK(classify(off * 0.5f, g.ticks[1]) == classify(off, g.ref_tick));
        }
        CHECK(classify(-0.035f, g.ticks[0]) != classify(-0.035f, g.ref_tick));

        CHECK(gs.lane_held);
        CHECK(g.lane_ptr(api) != &g.hispeed);
        CHECK_NEAR(range_width() / width1, 0.5, 1e-4);
        CHECK(g.hispeed == 2.0f);

        CHECK_NEAR(offsets().first, 0.004, 1e-4);
        CHECK_NEAR(offsets().second, 0.004, 1e-4);
        CHECK(g.draw_off == 0.008f && g.judge_params.f<float>(api.judge_offs_at) == 0.008f);

        g.step(key(&Keys::scale_windows));
        for (int i = 0; i < 5; i++) g.step();
        gs = core::guards();
        CHECK(!core::trainer().scale_windows());
        CHECK_NEAR(gs.window_factor, 1.0, 1e-9);
        for (int n = 0; n < 2; n++) {
            const auto b = g.bounds(n);
            for (int k = 0; k < 4; k++) CHECK(b[size_t(k)] == orig_bounds[k]);
        }
        CHECK(gs.lane_held);
        g.step(key(&Keys::scale_windows));
        for (int i = 0; i < 5; i++) g.step();
        CHECK_NEAR(core::guards().window_factor, 0.5, 1e-9);
        CHECK(g.bounds(0)[0] == orig_bounds[0] * 0.5f);
        g.maps[1].node[2].f<float>(0x20) = 0.5f;
        g.step(key(&Keys::scale_windows));
        for (int i = 0; i < 5; i++) g.step();
        CHECK(g.maps[1].node[2].f<float>(0x20) == 0.5f);
        CHECK(core::guards().window_bounds == 0);
        for (int n = 0; n < 2; n++)
            for (int k = 0; k < 4; k++) g.maps[n].node[k].f<float>(0x20) = orig_bounds[k];
        g.step(key(&Keys::scale_windows));
        for (int i = 0; i < 5; i++) g.step();
        CHECK_NEAR(core::guards().window_factor, 0.5, 1e-9);
        CHECK(core::guards().window_bounds == 8);
        for (int n = 0; n < 2; n++) {
            const auto b = g.bounds(n);
            for (int k = 0; k < 4; k++) CHECK(b[size_t(k)] == orig_bounds[k] * 0.5f);
        }

        auto presses_stub = [](void* self) {
            auto* in = static_cast<uint8_t*>(self);
            void* judge = *reinterpret_cast<void**>(in + 8);
            void** jv = *reinterpret_cast<void***>(judge);
            const float J = reinterpret_cast<FnGetF>(jv[2])(judge);
            for (int lane : {1, 4}) {
                uint8_t* v = in + 0x28 + lane * 0x18;
                float* b = *reinterpret_cast<float**>(v);
                b[0] = J - 0.020f;
                float* e = b + 1;
                std::memcpy(v + 8, &e, 8);
            }
        };
        core::input_update(g.input.p(), presses_stub);
        {
            const float J = reinterpret_cast<FnGetF>(core::api().vft_judge[2])(g.judge.p());
            CHECK_NEAR(g.presses[0][0], J - 0.010f, 1e-5);
            CHECK_NEAR(g.presses[3][0], J - 0.010f, 1e-5);
        }

        g.step(key(&Keys::pause));
        CHECK(core::guards().pause_req == 1 && core::guards().paused);
        g.step();
        g.step();
        const double g0 = g.game_time(), c0 = g.draw_clock(), v0 = g.voice_ms();
        const int64_t pulled0 = g_pulled;
        for (int i = 0; i < int(5 / kDt); i++) g.step();
        CHECK_NEAR(g.game_time() - g0, 0.0, 0.002);
        CHECK_NEAR(g.draw_clock() - c0, 0.0, 0.002);
        CHECK_NEAR(g.voice_ms() - v0, 0.0, 1.0);
        CHECK(g_pulled == pulled0);
        core::input_update(g.input.p(), presses_stub);
        CHECK(g.input.f<float*>(0x28 + 1 * 0x18 + 8) == g.presses[0]);
        g.step(key(&Keys::prev));
        for (int i = 0; i < 30; i++) g.step();
        CHECK(core::guards().paused);
        const double g1 = g.game_time();
        for (int i = 0; i < 60; i++) g.step();
        CHECK_NEAR(g.game_time() - g1, 0.0, 0.002);
        {
            const auto& tl = *core::state().timeline;
            auto at = [&] { return tl.measure_at(g.chart() + 0.001); };
            auto wander = [&](int i) { g.timer.f<int64_t>(core::api().off_timer_acc) += (i % 2 ? 5000000 : -5000000); };
            const int m0 = at();
            for (int i = 0; i < 6; i++) wander(i), g.step(key(&Keys::next));
            CHECK(at() == m0 + 6);
            for (int i = 0; i < 6; i++) wander(i), g.step(key(&Keys::prev));
            CHECK(at() == m0);
            for (int i = 0; i < 60; i++) g.step();
        }
        g.step(key(&Keys::pause));
        CHECK(core::guards().pause_req == 0);
        for (int i = 0; i < 120; i++) g.step();
        const double T1 = g.T, t1 = g.game_time(), vv1 = g.voice_ms();
        for (int i = 0; i < int(4 / kDt); i++) g.step();
        CHECK_NEAR((g.game_time() - t1) / (g.T - T1), 0.5, 0.003);
        CHECK_NEAR((g.voice_ms() - vv1) / 1000 / (g.T - T1), 0.5, 0.003);
        double sum = 0;
        for (int i = 0; i < 120; i++)
            g.step(), sum += (g.game_time() - core::state().offset) - (g.voice_ms() / 1000.0 - core::stretch_lag());
        CHECK(std::fabs(sum / 120) < 0.004);

        for (int i = 0; i < 10; i++) g.step(key(&Keys::speed_up));
        for (int i = 0; i < int(3 / kDt); i++) g.step();
        CHECK(core::trainer().speed() == 1.0 && !core::state().sync_lock);
        CHECK(!core::guards().lane_held);
        CHECK(g.lane_ptr(api) == &g.hispeed);
        CHECK_NEAR(range_width() / width1, 1.0, 1e-6);
        CHECK(g.draw.f<float*>(api.off_draw_offs) == &g.draw_off);
        CHECK(g.judge.f<void*>(api.off_judge_offs) == g.judge_params.p());
        CHECK_NEAR(core::guards().window_factor, 1.0, 1e-9);
        for (int n = 0; n < 2; n++) {
            const auto b = g.bounds(n);
            for (int k = 0; k < 4; k++) CHECK(b[size_t(k)] == orig_bounds[k]);
        }
        g.step(key(&Keys::pause));
        g.step();
        g.step();
        const double g2 = g.game_time();
        const int64_t pulled1 = g_pulled;
        for (int i = 0; i < int(3 / kDt); i++) g.step();
        CHECK_NEAR(g.game_time() - g2, 0.0, 0.002);
        CHECK(g_pulled == pulled1);
        g.step(key(&Keys::pause));
        for (int i = 0; i < int(2 / kDt); i++) g.step();
        const double e1 = (g.game_time() - core::state().offset) - (g.voice_ms() / 1000.0 - core::stretch_lag());
        CHECK(std::fabs(e1) < 0.004);
        CHECK(g_pulled > pulled1);
        const double T2 = g.T, t2 = g.game_time();
        for (int i = 0; i < int(2 / kDt); i++) g.step();
        CHECK_NEAR((g.game_time() - t2) / (g.T - T2), 1.0, 0.003);

        g.objmgr.f<float>(api.off_chart_end) = float(g.game_time() + 3.0);
        for (int i = 0; i < int(4 / kDt) && core::guards().pause_req != 2; i++) g.step();
        CHECK(core::guards().pause_req == 2 && core::guards().paused);
        CHECK(g.game_time() < g.objmgr.f<float>(api.off_chart_end) - 0.1);
        g.step();
        g.step();
        const double g3 = g.game_time();
        for (int i = 0; i < 120; i++) g.step();
        CHECK_NEAR(g.game_time() - g3, 0.0, 0.003);
        g.step(key(&Keys::pause));
        CHECK(core::guards().pause_req == 2);
        g.step(key(&Keys::prev));
        for (int i = 0; i < 5; i++) g.step();
        CHECK(core::guards().pause_req == 1);
        g.step(key(&Keys::pause));
        CHECK(core::guards().pause_req == 0 && !core::guards().paused);
        g.objmgr.f<float>(api.off_chart_end) = 1.0e5f;
        for (int i = 0; i < 3; i++) g.step();
        g.objmgr.f<float>(api.off_chart_end) = float(g.game_time() + 1.0);
        g.step(key(&Keys::next));
        for (int i = 0; i < 10; i++) g.step();
        CHECK(g.game_time() <= g.objmgr.f<float>(api.off_chart_end) - 0.40);
        g.objmgr.f<float>(api.off_chart_end) = 1.0e5f;
        g.step();
        if (core::guards().pause_req) g.step(key(&Keys::pause));

        for (int i = 0; i < 3; i++) g.step();
        auto mark_crash = [&] {
            const float zero = 0.0f;
            std::memcpy(g.recs + 12 + 4, &zero, 4);
            g.recs[12 + 8] = 0;
            g.recs[12 + 9] = 1;
            g.fumen.f<uint8_t>(api.off_forced_crash) = 1;
        };
        mark_crash();
        g.step();
        CHECK(g.recs[12 + 9] == 0 && g.recs[12 + 8] == 1);
        float gauge_after = -1;
        std::memcpy(&gauge_after, g.recs + 12 + 4, 4);
        CHECK_NEAR(gauge_after, 0.0f, 1e-6);
        CHECK(g.fumen.f<uint8_t>(api.off_forced_crash) == 0);
        CHECK(core::guards().crash_undone >= 2);
        g.step(key(&Keys::jump_start));
        std::memcpy(&gauge_after, g.recs + 12 + 4, 4);
        CHECK_NEAR(gauge_after, 60.0f, 1e-6);
        CHECK(g.recs[12 + 9] == 0 && g.recs[12 + 8] == 1);
        g.step();
        g.step();
        {
            g.fumen.f<uint64_t>(api.off_players_count) = 2;
            g.step();
            const int undone = core::guards().crash_undone;
            const float zero = 0.0f;
            std::memcpy(g.recs + 12 + 4, &zero, 4);
            g.recs[12 + 9] = 1;
            for (int i = 0; i < 3; i++) {
                g.step();
                CHECK(g.recs[12 + 9] == 1);
            }
            CHECK(core::guards().crash_undone == undone);
            g.step(key(&Keys::jump_start));
            CHECK(g.recs[12 + 9] == 0);
            g.fumen.f<uint64_t>(api.off_players_count) = 1;
            g.step();
        }

        {
            static auto chord_stub = [](void* self) {
                auto* c = static_cast<uint8_t*>(self);
                c[0x10] = 1;
                const float t = 0.5f;
                std::memcpy(c + 0x14, &t, 4);
            };
            auto gauge = [&] {
                float v = -1;
                std::memcpy(&v, g.recs + 12 + 4, 4);
                return v;
            };
            static auto released_stub = [](void*) {};
            Obj<0x40> chord, idle;
            mark_crash();
            g.step();
            CHECK_NEAR(gauge(), 0.0f, 1e-6);
            core::crash_update(chord.p(), chord_stub);
            CHECK(chord.f<uint8_t>(0x10) == 0 && chord.f<float>(0x14) == 0.0f);
            CHECK(core::guards().chord_blocked >= 1);
            g.step();
            CHECK_NEAR(gauge(), 60.0f, 1e-6);
            mark_crash();
            g.step();
            core::crash_update(chord.p(), chord_stub);
            g.step();
            CHECK_NEAR(gauge(), 0.0f, 1e-6);
            core::crash_update(idle.p(), released_stub);
            g.step();
        }

        {
            static int audio_stops = 0;
            static auto audio_stop_stub = [](void*) { audio_stops++; };
            core::set_audio_stop(audio_stop_stub);
            g.use_restart(api);
            g.timer.f<uint8_t>(api.off_timer_running) = 0;
            auto world_state = [&] { return g.world.f<const void*>(api.off_state_fn); };
            auto pending = [&] { return g.world.f<const void*>(api.off_pending); };
            CHECK(core::trainer().enabled() && world_state() == api.state_playing);
            for (int i = 0; i < 3; i++) g.step();
            CHECK(core::guards().restarts == 0 && audio_stops == 0 && pending() == nullptr);
            const int counter0 = g.counter;
            g.step(key(&Keys::toggle));
            CHECK(!core::trainer().enabled());
            CHECK(core::guards().restarts == 1 && audio_stops == 1);
            CHECK(pending() == api.state_retry);
            CHECK(g.world.f<uint32_t>(api.off_subreq) == 0 && g.world.f<uint32_t>(api.off_subreq + 4) == 1);
            CHECK(g.counter == counter0 + 1 && g.ctx_ctrl.f<int>(8) == 1);
            CHECK(core::state().bgm == nullptr && !core::state().offset_ok);
            g.world.f<const void*>(api.off_state_fn) = api.state_retry;
            g.world.f<const void*>(api.off_pending) = nullptr;
            for (int i = 0; i < 5; i++) g.step();
            CHECK(core::guards().restarts == 1 && audio_stops == 1);
            g.timer.f<int64_t>(api.off_timer_acc) = -int64_t(g.T * 1e9);
            g.voice.f<int>(0x90) = 0;
            g.fx.f<int>(0x90) = 0;
            for (int i = 0; i < int(8 / kDt) && !core::state().offset_ok; i++) g.step();
            CHECK(core::state().bgm != nullptr && core::state().offset_ok);
            CHECK_NEAR(core::state().offset, kLeadIn, 3 * kDt);
            g.world.f<const void*>(api.off_state_fn) = api.state_playing;
            g.step(key(&Keys::toggle));
            CHECK(core::trainer().enabled());
            for (int i = 0; i < 3; i++) g.step();
            CHECK(core::guards().restarts == 1);
            g.world.f<const void*>(api.off_state_fn) = api.state_end;
            g.step();
            CHECK(core::guards().restarts == 2 && audio_stops == 2 && pending() == api.state_retry);
            g.world.f<const void*>(api.off_state_fn) = api.state_playing;
            g.world.f<const void*>(api.off_pending) = nullptr;
            g.step();
            static auto finished_stub = [](void*) -> uint8_t { return 1; };
            g.world.f<uint8_t>(api.off_finished) = 1;
            CHECK(core::world_finished(g.world.p(), finished_stub) == 0);
            CHECK(g.world.f<uint8_t>(api.off_finished) == 0);
            g.step();
            CHECK(core::guards().restarts == 3 && pending() == api.state_retry);
            g.world.f<const void*>(api.off_state_fn) = api.state_playing;
            g.world.f<const void*>(api.off_pending) = nullptr;
            g.step();
            g.world.f<const void*>(api.off_pending) = api.state_retry;
            g.step();
            CHECK(core::guards().restarts == 4 && audio_stops == 3);
            g.world.f<const void*>(api.off_pending) = nullptr;
            core::set_audio_stop(nullptr);
        }

        if (api.off_spc_list) {
            CHECK(core::trainer().enabled());
            g.step(key(&Keys::no_camera));
            CHECK(core::trainer().no_camera());
            Obj<0x40> head;
            Obj<0x48> nodes[5];
            const int types[5][2] = {{api.spc_lane_y, 0}, {api.spc_morph0 + 2, 0}, {0xe, 0x1b}, {0xe, 0x1c}, {3, 0}};
            for (int i = 0; i < 5; i++) {
                nodes[i].f<void*>(0) = i < 4 ? nodes[i + 1].p() : head.p();
                nodes[i].f<void*>(8) = i > 0 ? nodes[i - 1].p() : head.p();
                nodes[i].f<int>(0x28) = types[i][0];
                nodes[i].f<int>(0x2c) = types[i][1];
            }
            head.f<void*>(0) = nodes[0].p();
            head.f<void*>(8) = nodes[4].p();
            Obj<0x200> vox;
            vox.f<void*>(api.off_vox_data + api.off_spc_list) = head.p();
            vox.f<size_t>(api.off_vox_data + api.off_spc_list + 8) = 5;
            Obj<0x100> world;
            world.f<void*>(api.off_world_vox) = vox.p();
            static bool init_called = false;
            core::world_init(world.p(), nullptr, [](void*, void*) { init_called = true; });
            CHECK(init_called && vox.f<size_t>(api.off_vox_data + api.off_spc_list + 8) == 3);
            CHECK(head.f<void*>(0) == nodes[2].p() && nodes[2].f<void*>(8) == head.p());
            CHECK(nodes[3].f<void*>(0) == nodes[4].p() && head.f<void*>(8) == nodes[4].p());
            g.step(key(&Keys::no_camera));
        }

        if (api.fd_rep) {
            Obj<0x200> fd;
            Obj<0x100> world;
            uint8_t tbl[3 * 0x20] = {};
            const float bpm[3] = {100, 200, 150}, mult[3] = {1, 1, 2};
            for (int i = 0; i < 3; i++) {
                std::memcpy(tbl + i * api.fd_stride + api.fd_bpm, &bpm[i], 4);
                std::memcpy(tbl + i * api.fd_stride + api.fd_mult, &mult[i], 4);
            }
            tbl[api.fd_stride + api.fd_stop] = 1;
            auto m = [&](int i) { return at<float>(tbl + i * api.fd_stride, api.fd_mult); };
            auto stop = [&](int i) { return tbl[i * api.fd_stride + api.fd_stop]; };
            fd.f<uint8_t*>(api.off_fd_table) = tbl;
            fd.f<uint8_t*>(api.off_fd_table + 8) = tbl + 3 * api.fd_stride;
            fd.f<float>(0x118) = 150;
            fd.f<uint8_t>(0x11c) = 1;
            world.f<void*>(api.off_world_fdata) = fd.p();
            CHECK(core::fixed_scroll(world.p(), true) && fd.f<uint8_t>(api.off_fd_const) == 1);
            CHECK(m(0) == 1.5f && m(1) == 0.75f && m(2) == 2.0f && stop(1) == 0);
            CHECK(!core::fixed_scroll(world.p(), true));
            CHECK(core::fixed_scroll(world.p(), false) && fd.f<uint8_t>(api.off_fd_const) == 0);
            CHECK(m(0) == 1 && m(1) == 1 && m(2) == 2 && stop(1) == 1);
            fd.f<uint8_t>(api.off_fd_const) = 1;
            CHECK(core::fixed_scroll(world.p(), true) && m(0) == 1 && stop(1) == 0);
            CHECK(core::fixed_scroll(world.p(), false) && fd.f<uint8_t>(api.off_fd_const) == 1 && stop(1) == 1);
            tbl[api.fd_stride + api.fd_stop] = 0;
            CHECK(!core::fixed_scroll(world.p(), true) && !core::fixed_scroll(world.p(), false));
        }

        if (api.vft_event) {
            struct Target {
                void** vft;
                int id;
            };
            static int fired[4];
            static void* target_vft[6] = {};
            target_vft[2] = reinterpret_cast<void*>(+[](Target* t) { fired[t->id]++; });
            static Obj<0x100>* draw = &g.draw;
            Target targets[4];
            Obj<0x50> fx[4];
            void* list[8] = {};
            for (int i = 0; i < 4; i++) {
                targets[i] = {target_vft, i};
                fx[i].f<void*>(0) = api.vft_event;
                fx[i].f<float>(api.off_event_time) = float(i + 1);
                fx[i].f<void*>(api.off_event_fn) = &targets[i];
                list[i * 2] = fx[i].p();
            }
            Obj<0x200> om;
            om.f<void*>(0x10) = g.draw.p();
            om.f<void**>(api.off_events) = list;
            om.f<void**>(api.off_events + 8) = list + 8;
            Obj<0x40> fu;
            fu.f<void*>(0x18) = om.p();
            core::set_fumen_rebuild([](void* f) {
                void* m = static_cast<Obj<0x40>*>(f)->f<void*>(0x18);
                const float now = reinterpret_cast<FnGetF>(core::api().vft_draw[slot::DrawGetTime])(draw->p());
                auto** l = static_cast<Obj<0x200>*>(m)->f<void**>(core::api().off_events);
                for (int i = 0; i < 4; i++) {
                    auto* e = static_cast<uint8_t*>(l[i * 2]);
                    if (*reinterpret_cast<float*>(e + core::api().off_event_time) <= now) {
                        auto* t = *reinterpret_cast<Target**>(e + core::api().off_event_fn);
                        reinterpret_cast<void (*)(Target*)>(t->vft[2])(t);
                    }
                }
            });
            auto at_time = [&](double s) { g.draw.f<int64_t>(0x38) = int64_t((s - g.draw_off) * 1e9); };
            auto run = [&](float before, double after) {
                std::memset(fired, 0, sizeof(fired));
                at_time(after);
                core::effects_rebuild(fu.p(), before);
                return fired[0] * 1000 + fired[1] * 100 + fired[2] * 10 + fired[3];
            };
            CHECK(run(2.5f, 3.5) == 10);
            CHECK(run(3.6f, 3.5) == 0);
            CHECK(run(4.2f, 3.5) == 1110);
            bool restored = true;
            for (int i = 0; i < 4; i++) restored &= fx[i].f<void*>(api.off_event_fn) == &targets[i];
            CHECK(restored);
        }
        core::set_fumen_rebuild(real_rebuild);
        core::set_clock(nullptr);
    }

    {
        FILE* f = _wfopen(L"build\\test_frame.log", L"rb");
        std::string logtext;
        char buf[4096];
        size_t n;
        while (f && (n = fread(buf, 1, sizeof(buf), f)) > 0) logtext.append(buf, n);
        if (f) fclose(f);
        CHECK(logtext.find("warning: no GameFumenPlayer at +0xc8 after 600 frames") != std::string::npos);
    }

    std::printf("%d checks, %d failed (log: build\\test_frame.log)\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
