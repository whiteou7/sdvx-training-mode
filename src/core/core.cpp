#include "core/internal.hpp"
#include "hud.hpp"
#include "core/util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tr::core::detail {

static std::wstring g_dir;

Module g_mod;
RttiIndex g_rtti;
GameApi g_api;

Config g_cfg;
Session g_s;
Trainer g_trainer;

static void load_config() {
    const std::wstring ini = g_dir + L"\\sdvx_training.ini";
    auto get = [&](const wchar_t* section, const wchar_t* key, int fallback) {
        wchar_t text[128];
        if (!GetPrivateProfileStringW(section, key, L"", text, 128, ini.c_str()) || !text[0]) return fallback;
        wchar_t* end = nullptr;
        const long value = wcstol(text, &end, 0);
        return end == text ? fallback : int(value);
    };
    struct Entry {
        const wchar_t* section;
        const wchar_t* key;
        int Config::*field;
    };
    static const Entry entries[] = {
        {L"keys", L"toggle", &Config::vk_toggle},
        {L"keys", L"repeat", &Config::vk_repeat},
        {L"keys", L"set_start", &Config::vk_set_start},
        {L"keys", L"set_end", &Config::vk_set_end},
        {L"keys", L"jump_start", &Config::vk_jump_start},
        {L"keys", L"prev", &Config::vk_prev},
        {L"keys", L"next", &Config::vk_next},
        {L"keys", L"clear", &Config::vk_clear},
        {L"keys", L"speed_up", &Config::vk_speed_up},
        {L"keys", L"speed_down", &Config::vk_speed_down},
        {L"keys", L"overlay", &Config::vk_overlay},
        {L"keys", L"pause", &Config::vk_pause},
        {L"keys", L"scale_windows", &Config::vk_scale_windows},
        {L"keys", L"fixed_scroll", &Config::vk_fixed_scroll},
        {L"keys", L"no_camera", &Config::vk_no_camera},
        {L"training", L"enabled_on_start", &Config::enabled_on_start},
        {L"training", L"preroll_measures", &Config::preroll_measures},
    };
    for (const Entry& e : entries) g_cfg.*e.field = get(e.section, e.key, g_cfg.*e.field);

    auto get_double = [&](const wchar_t* key, double fallback) {
        wchar_t text[128];
        if (!GetPrivateProfileStringW(L"training", key, L"", text, 128, ini.c_str()) || !text[0]) return fallback;
        wchar_t* end = nullptr;
        const double value = wcstod(text, &end);
        return end == text ? fallback : value;
    };
    const Config defaults;
    g_cfg.speed_mod_step = get_double(L"speed_mod_step", g_cfg.speed_mod_step);
    g_cfg.speed_mod_min = get_double(L"speed_mod_min", g_cfg.speed_mod_min);
    g_cfg.speed_mod_max = get_double(L"speed_mod_max", g_cfg.speed_mod_max);
    if (!(g_cfg.speed_mod_step > 0 && g_cfg.speed_mod_min > 0 && g_cfg.speed_mod_min <= g_cfg.speed_mod_max)) {
        logf("ini: invalid speed_mod_step/min/max (%g, %g, %g), using %g, %g, %g", g_cfg.speed_mod_step,
             g_cfg.speed_mod_min, g_cfg.speed_mod_max, defaults.speed_mod_step, defaults.speed_mod_min,
             defaults.speed_mod_max);
        g_cfg.speed_mod_step = defaults.speed_mod_step;
        g_cfg.speed_mod_min = defaults.speed_mod_min;
        g_cfg.speed_mod_max = defaults.speed_mod_max;
    }
}

void* field(void* obj, uint32_t off) {
    void* p = nullptr;
    read(static_cast<uint8_t*>(obj) + off, p);
    return p;
}

static std::string short_name(const std::string& mangled) {
    return mangled.size() > 4 && mangled.compare(0, 3, ".?A") == 0 && (mangled[3] == 'V' || mangled[3] == 'U') ? mangled.substr(4) : mangled;
}

bool is_class(void* obj, const char* mangled) { return obj && g_rtti.class_of(obj) == mangled; }

void* fumen_of(void* world) {
    void* fumen = field(world, g_api.off_fumen);
    return is_class(fumen, cls::Fumen) ? fumen : nullptr;
}

void* find_world_obj(void* world, const char* cls_name) {
    for (uint32_t off = 0x20; off < 0x700; off += 8) {
        void* q = field(world, off);
        if (q && is_class(q, cls_name)) return q;
    }
    return nullptr;
}

static FnVoid g_orig_update = nullptr;

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), int(s.size()), w.data(), n);
    return w;
}

static bool read_msvc_string(void* p, std::string& out) {
    if (!readable(p, 32)) return false;
    uint64_t size = at<uint64_t>(p, 16), cap = at<uint64_t>(p, 24);
    if (size > 1024 || cap < size || cap < 15) return false;
    const char* data = cap > 15 ? at<const char*>(p, 0) : static_cast<const char*>(p);
    if (!readable(data, size)) return false;
    out.assign(data, size);
    return true;
}

static void start_session(void* world) {
    restore_lane();
    restore_offsets();
    g_s = Session{};
    g_prev_enabled = g_trainer.enabled();
    g_s.world = world;
    g_s.timer = field(world, g_api.off_timer);
    g_s.excluded = active_voices();
    g_s.audio = field(world, g_api.off_audio);
    logf("new GameWorldKFC %p (%zu voices already playing, ignored for BGM detection)", world, g_s.excluded.size());

    void* timer = g_s.timer;
    logf("  timer +0x%x %p %s", g_api.off_timer, timer, short_name(g_rtti.class_of(timer)).c_str());
    logf("  audio +0x%x %s", g_api.off_audio, short_name(g_rtti.class_of(field(world, g_api.off_audio))).c_str());
    logf("  fumen +0x%x %s", g_api.off_fumen, short_name(g_rtti.class_of(field(world, g_api.off_fumen))).c_str());
    logf("  tumami +0x%x %s / +0x%x %s", g_api.off_tumami_l,
         short_name(g_rtti.class_of(field(world, g_api.off_tumami_l))).c_str(), g_api.off_tumami_r,
         short_name(g_rtti.class_of(field(world, g_api.off_tumami_r))).c_str());
    logf("  tutorial +0x%x %p", g_api.off_tutorial, field(world, g_api.off_tutorial));

    std::string vox;
    if (read_msvc_string(static_cast<uint8_t*>(world) + g_api.off_vox_path, vox)) {
        logf("  vox path \"%s\"", vox.c_str());
        {
            std::string name = vox.substr(vox.find_last_of("/\\") + 1);
            remote::set_song(name.substr(0, name.rfind('.')));
        }
        std::string rel = vox;
        while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
        for (auto& ch : rel)
            if (ch == '/') ch = '\\';
        wchar_t modpath[MAX_PATH];
        GetModuleFileNameW(reinterpret_cast<HMODULE>(g_mod.base), modpath, MAX_PATH);
        std::wstring root(modpath);
        root = root.substr(0, root.find_last_of(L'\\'));
        root = root.substr(0, root.find_last_of(L'\\'));
        wchar_t cwd[MAX_PATH];
        GetCurrentDirectoryW(MAX_PATH, cwd);
        for (const std::wstring& base : {root, std::wstring(cwd)}) {
            std::wstring path = base + L"\\" + widen(rel);
            std::string err;
            if (g_s.tl.load(path, &err)) {
                g_s.tl_ok = true;
                logf("  measure grid: %d measures, end measure %d, res %d, measure 2 at %.3f s", g_s.tl.count(),
                     g_s.tl.end_measure + 1, g_s.tl.res, g_s.tl.start_of(1));
                break;
            }
            logf("  vox load failed (%s) for base %ls", err.c_str(), base.c_str());
        }
    } else {
        logf("  vox path string at +0x%x not readable", g_api.off_vox_path);
        remote::set_song("");
    }
    g_trainer.reset_session(g_s.tl_ok ? &g_s.tl : nullptr);
}

static bool do_seek(void* world, double target, double chart_now) {
    void* timer = field(world, g_api.off_timer);
    void** tvft = nullptr;
    if (!read(timer, tvft) || tvft != g_api.vft_timer) {
        logf("seek aborted: timer not a GameTimer");
        return false;
    }
    if (!g_s.bgm) {
        logf("seek refused: BGM voice not identified; candidates:");
        find_bgm(world, true, reinterpret_cast<FnGetF>(tvft[slot::TimerGetTime])(timer));
        g_s.refuse_cooldown = 120;
        return false;
    }
    const float t_now = reinterpret_cast<FnGetF>(tvft[slot::TimerGetTime])(timer);
    if (target + g_s.offset < 0) target = -g_s.offset;
    if (guard_active()) {
        const float end_t = chart_end_time(world);
        const double limit = double(end_t) - kEndMargin - 0.3 - g_s.offset;  // the seek stops short of the end so training never reaches the result screen
        if (end_t > 0 && limit > 0 && target > limit) {
            logf("seek stopped short of the chart end: %.3f -> %.3f", target, limit);
            target = limit;
        }
    }
    const auto voices = song_voices(world, g_s.bgm);  // collected before anything moves
    show_notes();
    void* fumen = fumen_of(world);
    const float fx_before = fumen ? effects_time(fumen) : -1.0f;
    const float delta = float(target + g_s.offset - t_now);
    reinterpret_cast<FnSeek>(tvft[slot::TimerSeek])(timer, &delta);
    for (void* v : voices) {
        void** vvft = nullptr;
        if (read(v, vvft) && vvft == g_api.vft_voice)
            reinterpret_cast<FnSetD>(vvft[slot::VoiceSetPos])(v, target * 1000.0);
    }
    for (uint32_t off : {g_api.off_tumami_l, g_api.off_tumami_r}) {
        void* t = field(world, off);
        void** vft = nullptr;
        if (read(t, vft) && vft == g_api.vft_tumami) reinterpret_cast<FnVoid>(vft[slot::TumamiReset])(t);
    }
    g_req_reset.store(true);  // the time-stretch node still holds audio from before the seek
    if (fumen) rebuild_after_seek(fumen, fx_before);
    if (void* t = field(world, g_api.off_tutorial); t && is_class(t, cls::Tutorial)) g_api.tutorial_rebuild(t);
    const float t_after = reinterpret_cast<FnGetF>(tvft[slot::TimerGetTime])(timer);
    logf("seek chart %.3f -> %.3f s (timer %.3f -> %.3f, offset %.4f, bgm %p, %zu song voice%s moved)", chart_now,
         target, t_now, t_after, g_s.offset, g_s.bgm, voices.size(), voices.size() == 1 ? "" : "s");
    if (t_after > t_now + 0.05f) {
        const int n = skip_notes(world, t_now, t_after);
        logf("forward seek: %d skipped note%s recorded as missed without judgement", n, n == 1 ? "" : "s");
    }
    g_s.settle = 15;
    g_s.pause_pos = target;
    return true;
}

static const Keys* g_key_override = nullptr;

const struct {
    const char* name;
    bool Keys::*key;
} kCommands[] = {
    {"toggle", &Keys::toggle},
    {"repeat", &Keys::repeat},
    {"set_start", &Keys::set_start},
    {"set_end", &Keys::set_end},
    {"jump_start", &Keys::jump_start},
    {"prev", &Keys::prev},
    {"next", &Keys::next},
    {"clear", &Keys::clear},
    {"speed_up", &Keys::speed_up},
    {"speed_down", &Keys::speed_down},
    {"pause", &Keys::pause},
    {"scale_windows", &Keys::scale_windows},
    {"fixed_scroll", &Keys::fixed_scroll},
    {"no_camera", &Keys::no_camera},
};

static void add_remote_command(Keys& keys, double chart_now) {
    const auto command = remote::pop_command(GetTickCount64());
    if (!command) return;
    logf("remote: %s", command->c_str());
    if (*command == "status") {
        logf("status | %s bgm=%p offset=%.4f", g_trainer.status(chart_now).c_str(), g_s.bgm, g_s.offset);
        return;
    }
    for (const auto& c : kCommands) {
        if (*command == c.name) {
            keys.*c.key = true;
            return;
        }
    }
    logf("remote: unknown command \"%s\"", command->c_str());
}

const struct {
    int Config::*vk;
    bool Keys::*key;
    bool taps;
} kBindings[] = {
    {&Config::vk_toggle, &Keys::toggle, false},
    {&Config::vk_repeat, &Keys::repeat, false},
    {&Config::vk_set_start, &Keys::set_start, false},
    {&Config::vk_set_end, &Keys::set_end, false},
    {&Config::vk_jump_start, &Keys::jump_start, false},
    {&Config::vk_prev, &Keys::prev, false},
    {&Config::vk_next, &Keys::next, false},
    {&Config::vk_clear, &Keys::clear, false},
    {&Config::vk_speed_up, &Keys::speed_up, false},
    {&Config::vk_speed_down, &Keys::speed_down, false},
    {&Config::vk_pause, &Keys::pause, true},
    {&Config::vk_scale_windows, &Keys::scale_windows, false},
    {&Config::vk_fixed_scroll, &Keys::fixed_scroll, false},
    {&Config::vk_no_camera, &Keys::no_camera, false},
};

// Pause/Break sends its press and release together, so a poll rarely sees it held. For these keys the "pressed since last call" bit counts too.
static bool key_edge(bool& prev, bool focus, SHORT st, bool taps) {
    const bool down = focus && (st & 0x8000);
    const bool tapped = taps && focus && (st & 1);
    const bool edge = (down && !prev) || (tapped && !down);
    prev = down;
    return edge;
}

static Keys poll_keys() {
    if (g_key_override) return *g_key_override;
    DWORD pid = 0;
    if (HWND foreground = GetForegroundWindow()) GetWindowThreadProcessId(foreground, &pid);
    const bool focus = pid == GetCurrentProcessId();
    Keys keys;
    for (size_t i = 0; i < std::size(kBindings); i++) {
        const int vk = g_cfg.*kBindings[i].vk;
        if (vk) keys.*kBindings[i].key = key_edge(g_s.keys_prev[i], focus, GetAsyncKeyState(vk), kBindings[i].taps);
    }
    return keys;
}

static void log_world_ready(void* world, float t_now) {
    if (g_s.ready_logged) return;
    void* fumen = field(world, g_api.off_fumen);
    const bool ready = is_class(fumen, cls::Fumen);
    if (!ready && ++g_s.frames_unready < 600) return;
    g_s.ready_logged = true;
    if (ready) logf("world ready at timer %.3f: fumen +0x%x %p", t_now, g_api.off_fumen, fumen);
    else logf("warning: no GameFumenPlayer at +0x%x after 600 frames (found %p %s)", g_api.off_fumen, fumen,
              short_name(g_rtti.class_of(fumen)).c_str());
}

static void search_bgm(void* world, float t_now) {
    if (g_s.bgm || --g_s.bgm_rescan > 0) return;
    const bool diag = t_now > 0.5f && ++g_s.bgm_searches % 20 == 0 && g_s.bgm_searches <= 120;
    if (diag) logf("BGM not found yet at timer %.3f; %zu voices playing:", t_now, active_voices().size());
    g_s.bgm = find_bgm(world, diag, t_now);
    g_s.bgm_rescan = 15;  // about 0.1 s. The music is usually found about 0.5 s after it starts
    if (!g_s.bgm) return;
    double pos = 0;
    voice_pos(g_s.bgm, pos);
    logf("BGM voice %p at %.1f ms (timer %.3f), bus %d", g_s.bgm, pos, t_now, voice_bus(g_s.bgm));
    g_s.offset_ok = false;
    g_s.drift_frames = 0;
}

static void track_offset(float t_now) {
    double ms;
    if (g_s.bgm && !voice_pos(g_s.bgm, ms)) {
        logf("BGM voice %p lost", g_s.bgm);
        g_s.bgm = nullptr;
    }
    if (g_s.bgm && g_s.settle <= 0 && !g_s.sync_lock && voice_pos(g_s.bgm, ms)) {
        const double off = t_now - ms / 1000.0;
        if (!g_s.offset_ok) {
            g_s.offset = off;
            g_s.offset_ok = true;
            logf("timer-chart offset %.4f s", off);
        } else if (std::fabs(off - g_s.offset) <= 0.25) {
            g_s.drift_frames = 0;
            g_s.offset += 0.05 * (off - g_s.offset);
        } else if (++g_s.drift_frames >= 60) {
            logf("BGM voice %p drifted %.3f s from the timer; searching again", g_s.bgm, off - g_s.offset);
            g_s.bgm = nullptr;
            g_s.offset_ok = false;
            g_s.drift_frames = 0;
            g_s.bgm_rescan = 0;
        }
    }
    if (g_s.settle > 0) g_s.settle--;
}

static void drop_seek_keys(Keys& keys) {
    const bool any = keys.set_start || keys.set_end || keys.jump_start || keys.prev || keys.next || keys.clear;
    if (any && !g_s.waiting_logged) {
        logf("key ignored: waiting for the BGM to be identified");
        g_s.waiting_logged = true;
    }
    keys.set_start = keys.set_end = keys.jump_start = keys.prev = keys.next = keys.clear = false;
}

static void run_trainer(void* world, double chart_now, const Keys& keys) {
    std::string msg;
    const auto target = g_trainer.update(chart_now, keys, &msg);
    if (target && g_s.refuse_cooldown > 0) return;  // after a refused seek, neither retry nor log every frame
    if (!msg.empty()) logf("%s", msg.c_str());
    if (!target || !do_seek(world, *target, chart_now) || !g_trainer.to_start()) return;
    refill_gauge(world);
    if (const auto start = g_trainer.preroll_end()) {
        const int n = skip_and_hide_notes(world, *target + g_s.offset, *start + g_s.offset);
        logf("preroll: %d note%s cleared before the loop start", n, n == 1 ? "" : "s");
    }
}

static void publish_status(double chart_now, float t_now) {
    remote::Status st;
    st.training = g_trainer.enabled();
    st.repeat = g_trainer.repeat();
    st.start = g_trainer.start() >= 0 ? g_trainer.start() + 1 : 0;
    st.end = g_trainer.end() >= 0 ? g_trainer.end() : 0;
    st.measure = g_s.tl_ok ? g_s.tl.measure_at(chart_now) + 1 : 0;
    st.measures = g_s.tl_ok ? g_s.tl.end_measure + 1 : 0;
    st.bgm = g_s.bgm != nullptr && g_s.offset_ok;
    st.time = chart_now;
    st.started = t_now > 0.05f;  // the timer stays at 0 during the intro
    st.speed = g_trainer.speed_setting();
    st.speed_ok = !g_s.speed_refused;
    st.paused = g_s.paused_eff ? g_s.pause_req : 0;
    st.scale_windows = g_trainer.scale_windows();
    st.fixed_scroll = g_trainer.fixed_scroll();
    st.no_camera = g_trainer.no_camera();
    remote::publish(st);
}

static void on_frame_body(void* world) {
    void* timer = field(world, g_api.off_timer);
    if (world != g_s.world || timer != g_s.timer || field(world, g_api.off_audio) != g_s.audio) start_session(world);

    void** tvft = nullptr;
    if (!read(timer, tvft) || tvft != g_api.vft_timer) return;
    const float t_now = reinterpret_cast<FnGetF>(tvft[slot::TimerGetTime])(timer);
    draw_follow(world, at<uint8_t>(timer, g_api.off_timer_running) != 0);
    refresh_chart_caches(world);
    if (guard_active()) undo_crash(world);
    remember_gauge(world, t_now);
    if (!g_s.input && g_api.vft_input && g_s.input_wait-- <= 0) {
        g_s.input_wait = 30;  // the world builds its components over the first frames
        g_s.input = find_world_obj(world, cls::Input);
    }
    log_world_ready(world, t_now);

    search_bgm(world, t_now);
    track_offset(t_now);
    if (g_s.refuse_cooldown > 0) g_s.refuse_cooldown--;

    double chart_now = t_now - g_s.offset;
    if (!g_s.paused_eff) g_s.pause_pos_ok = false;
    else if (g_s.pause_pos_ok) chart_now = g_s.pause_pos;
    else g_s.pause_pos = chart_now, g_s.pause_pos_ok = true;
    Keys keys = poll_keys();
    add_remote_command(keys, chart_now);
    if (g_s.chord_jump) keys.jump_start = true;
    g_s.chord_jump = false;
    if (!g_s.offset_ok) drop_seek_keys(keys);
    if (g_s.resume_at >= 0 && g_s.offset_ok && !g_s.in_retry && t_now > 0.05f) {
        const double to = g_s.resume_at;
        g_s.resume_at = -1;
        do_seek(world, to, chart_now);
    }
    run_trainer(world, chart_now, keys);
    handle_pause(world, t_now, keys.pause);
    process_restart(world, chart_now);
    apply_speed(timer, g_trainer.speed(), t_now);
    apply_windows(world, g_trainer.scale_windows() ? g_s.scale : 1.0);
    apply_lane();
    apply_offsets();
    apply_camera();
    publish_status(chart_now, t_now);
}

static void on_frame(void* world) {
    g_last_frame_ms = GetTickCount64();
    on_frame_body(world);
    draw_remember();
    snapshot_guard(world);
}

static void hk_update(void* world) {
    g_orig_update(world);
    on_frame(world);
}

}  // namespace tr::core::detail

namespace tr::core {

using namespace detail;

void set_dir(const std::wstring& dir) { g_dir = dir; }

void load_config() { detail::load_config(); }

bool init(HMODULE game) {
    if (!g_mod.init(game)) {
        logf("soundvoltex.dll: bad PE header");
        return false;
    }
    const ULONGLONG t0 = GetTickCount64();
    g_rtti.build(g_mod);
    logf("RTTI: %zu classes indexed in %llu ms", g_rtti.size(), (unsigned long long)(GetTickCount64() - t0));
    if (!resolve_game(g_mod, g_rtti, g_api)) {
        logf("unsupported soundvoltex.dll build: %s", g_api.error.c_str());
        return false;
    }
    logf("resolved: update=+0x%x timer@+0x%x tumami@+0x%x/+0x%x fumen@+0x%x tutorial@+0x%x "
         "fumen_rebuild=+0x%x tutorial_rebuild=+0x%x referee_now@+0x%x timer_acc@+0x%x stretch=%s draw=%s",
         g_mod.rva(g_api.vft_world[slot::WorldUpdate]), g_api.off_timer, g_api.off_tumami_l, g_api.off_tumami_r,
         g_api.off_fumen, g_api.off_tutorial, g_mod.rva(reinterpret_cast<void*>(g_api.fumen_rebuild)),
         g_mod.rva(reinterpret_cast<void*>(g_api.tutorial_rebuild)), g_api.off_referee_now, g_api.off_timer_acc,
         g_api.vft_stretch ? "yes" : "no", g_api.vft_draw ? "yes" : "no");
    TrainerConfig tc;
    tc.preroll_measures = g_cfg.preroll_measures;
    tc.speed_mod_step = g_cfg.speed_mod_step;
    tc.speed_mod_min = g_cfg.speed_mod_min;
    tc.speed_mod_max = g_cfg.speed_mod_max;
    g_trainer = Trainer(tc);
    g_trainer.set_enabled(g_cfg.enabled_on_start != 0);
    g_prev_enabled = g_trainer.enabled();
    g_s = Session{};
    remote::init(g_dir);
    remote::set_speed_config(tc);
    remote::seed(g_trainer.enabled(), g_trainer.repeat(), g_trainer.speed_setting(), g_trainer.scale_windows(),
                 g_trainer.fixed_scroll(), g_trainer.no_camera());
    return true;
}

bool install_hook() {
    if (!hook_slot(g_api.vft_world, slot::WorldUpdate, hk_update, g_orig_update, "world update")) return false;
    hook_slot(g_api.vft_master_render, slot::MasterRender, hk_master_render, g_orig_master_render, "voice mixer");

    if (g_api.vft_stretch) {
        g_stretch_set_rate = reinterpret_cast<FnSetRate>(g_api.vft_stretch[slot::StretchSetRate]);
        hook_slot(g_api.vft_stretch, slot::FeederRead, hk_stretch_read, g_orig_stretch_read, "time-stretch");
    } else {
        logf("no time-stretch node in this build; playback speed unavailable");
    }

    if (g_api.off_world_vox) hook_slot(g_api.vft_world, slot::WorldInit, hk_world_init, g_orig_world_init, "chart init");
    if (!g_api.fd_rep) logf("fixed scroll unavailable in this build");
    if (!g_api.off_spc_list) logf("lane events cannot be dropped in this build");

    if (g_api.vft_engine) {
        const bool camera = hook_slot(g_api.vft_engine, slot::EngineRotate, hk_rotate, g_orig_rotate, "lane rotation") &&
                            hook_slot(g_api.vft_gworld, slot::CamDistance, hk_cam_dist, g_orig_cam_dist, "camera distance") &&
                            hook_slot(g_api.vft_gworld, slot::CamPitch, hk_cam_pitch, g_orig_cam_pitch, "camera pitch");
        if (camera) logf("camera defaults: distance %.2f, pitch %.2f", *g_api.cam_distance, *g_api.cam_pitch);
    } else {
        logf("no-camera switch unavailable in this build");
    }

    if (g_api.vft_morph) hook_slot(g_api.vft_morph, slot::MorphUpdate, hk_morph, g_orig_morph, "lane split");
    else logf("lane split cannot be held in this build");

    if (g_api.vft_input) hook_slot(g_api.vft_input, slot::InputUpdate, hk_input_update, g_orig_input_update, "input");

    if (g_api.vft_crash) hook_slot(g_api.vft_crash, slot::CrashUpdate, hk_crash_update, g_orig_crash_update, "chord guard");
    else logf("chord guard unavailable in this build: the chord can still crash a training track");
    if (g_api.off_finished)
        hook_slot(g_api.vft_world, slot::WorldFinished, hk_world_finished, g_orig_world_finished, "result guard");
    logf("guard: restart=%s crash undo=%s chart end=%s", g_api.state_retry ? "yes" : "no",
         g_api.off_players_map ? "yes" : "no", g_api.off_chart_end ? "yes" : "no");
    logf("hook installed. keys: toggle=0x%02x repeat=0x%02x set_start=0x%02x set_end=0x%02x jump_start=0x%02x prev=0x%02x next=0x%02x "
         "clear=0x%02x speed+=0x%02x speed-=0x%02x panel=0x%02x pause=0x%02x; preroll=%d enabled_on_start=%d",
         g_cfg.vk_toggle, g_cfg.vk_repeat, g_cfg.vk_set_start, g_cfg.vk_set_end, g_cfg.vk_jump_start, g_cfg.vk_prev,
         g_cfg.vk_next, g_cfg.vk_clear, g_cfg.vk_speed_up, g_cfg.vk_speed_down, g_cfg.vk_overlay, g_cfg.vk_pause,
         g_cfg.preroll_measures, g_cfg.enabled_on_start);
    hud::start({g_cfg.vk_overlay});
    remote::start_thread();
    logf("remote control on: sdvx_training.cmd / sdvx_training.status in %ls", g_dir.c_str());
    return true;
}

void frame(void* world) { on_frame(world); }

void set_key_override(const Keys* keys) { g_key_override = keys; }
bool key_edge(bool& prev, bool focus, short st, bool tap_counts) { return detail::key_edge(prev, focus, st, tap_counts); }

void voice_playing(void* voice) { note_active_voice(voice); }
void forget_voices() { detail::clear_active_voices(); }
void mixer_rendered(void* master) { master_rendered(master); }
void set_clock(int64_t (*ns)()) { g_clock = ns; }
uint64_t stretch_read(void* self, void* buf, int bytes) { return hk_stretch_read(self, buf, bytes); }
void set_stretch_original(FnFeederRead read, FnSetRate set_rate) { g_orig_stretch_read = read, g_stretch_set_rate = set_rate; }
double stretch_lag() { return g_stretching.load() ? g_stretch_lag.load() / kMixRate : 0.0; }
// Tests use these to replace originals that read game objects a test doesn't have.
void set_audio_stop(FnVoid fn) { g_api.audio_stop = fn; }
void set_fumen_rebuild(FnVoid fn) { g_api.fumen_rebuild = fn; }
void forget_session() {
    restore_lane();
    restore_offsets();
    g_s = Session{};
}
void input_update(void* self, FnVoid orig) {
    g_orig_input_update = orig;
    hk_input_update(self);
}
void crash_update(void* self, FnVoid orig) {
    g_orig_crash_update = orig;
    hk_crash_update(self);
}
uint8_t world_finished(void* self, uint8_t (*orig)(void*)) {
    g_orig_world_finished = orig;
    return hk_world_finished(self);
}
void effects_rebuild(void* fumen, float before) { rebuild_after_seek(fumen, before); }
void world_init(void* world, void* arg, FnWorldInit orig) {
    g_orig_world_init = orig;
    hk_world_init(world, arg);
}
bool fixed_scroll(void* world, bool on) { return set_fixed_scroll(world, on); }
void set_config(const char* key, int value) {
    const std::string k = key;
    if (k == "vk_pause") g_cfg.vk_pause = value;
}
Guards guards() {
    return {g_s.pause_req, g_s.paused_eff, g_s.scale,        g_s.win.factor,      g_s.win.built ? g_s.win.e.size() : 0,
            g_lane_range != nullptr, g_s.crash_undone, g_s.chord_blocked, g_s.restarts, g_lane_v};
}

const Trainer& trainer() { return g_trainer; }
const GameApi& api() { return g_api; }
const RttiIndex& rtti() { return g_rtti; }
State state() { return {g_s.bgm, g_s.offset, g_s.offset_ok, g_s.tl_ok ? &g_s.tl : nullptr, g_s.sync_lock}; }

}  // namespace tr::core
