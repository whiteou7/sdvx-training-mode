#include "remote.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <utility>

#include "log.hpp"

namespace tr::remote {

namespace {

CRITICAL_SECTION g_lock;
bool g_lock_init = false;
std::wstring g_cmd_path, g_status_path, g_status_tmp;

std::deque<std::pair<std::string, uint64_t>> g_queue;
Status g_status;
std::string g_song;
uint64_t g_last_publish = 0;
TrainerConfig g_speed_cfg;

std::string g_last_written;
uint64_t g_last_write = 0;

constexpr uint64_t kInSongMs = 500;
constexpr uint64_t kStaleCmdMs = 1500;

struct Lock {
    Lock() { EnterCriticalSection(&g_lock); }
    ~Lock() { LeaveCriticalSection(&g_lock); }
};

// Outside a chart the game thread does nothing, so switches are written to the status right away.
bool is_switch(const std::string& c) {
    return c == "toggle" || c == "repeat" || c == "scale_windows" || c == "fixed_scroll" || c == "no_camera" || c == "speed_up" || c == "speed_down";
}

void enqueue_locked(const std::string& c, uint64_t now) {
    const bool in_song = g_last_publish && now - g_last_publish < kInSongMs;
    g_queue.emplace_back(c, now);
    if (in_song) return;
    if (c == "toggle") g_status.training = !g_status.training;
    if (c == "repeat") g_status.repeat = !g_status.repeat;
    if (c == "scale_windows") g_status.scale_windows = !g_status.scale_windows;
    if (c == "fixed_scroll") g_status.fixed_scroll = !g_status.fixed_scroll;
    if (c == "no_camera") g_status.no_camera = !g_status.no_camera;
    if (c == "speed_up" || c == "speed_down") {
        const TrainerConfig& cfg = g_speed_cfg;
        const double s = g_status.speed + (c == "speed_up" ? cfg.speed_mod_step : -cfg.speed_mod_step);
        g_status.speed = std::clamp(std::round(s / cfg.speed_mod_step) * cfg.speed_mod_step, cfg.speed_mod_min, cfg.speed_mod_max);
    }
}

DWORD WINAPI thread_main(void*) {
    for (;;) {
        poll_once();
        write_status_once();
        Sleep(50);
    }
}

}  // namespace

void init(const std::wstring& dir) {
    if (!g_lock_init) {
        InitializeCriticalSection(&g_lock);
        g_lock_init = true;
    }
    Lock l;
    g_cmd_path = dir + L"\\sdvx_training.cmd";
    g_status_path = dir + L"\\sdvx_training.status";
    g_status_tmp = dir + L"\\sdvx_training.status.tmp";
    g_queue.clear();
    g_last_written.clear();
    g_last_write = 0;
}

void start_thread() {
    if (HANDLE t = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr)) CloseHandle(t);
}

void poll_once() {
    // Exclusive + delete-on-close: a writer can't append while we read, and nothing is lost between
    // reading and deleting.
    HANDLE h = CreateFileW(g_cmd_path.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string text;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0 && text.size() < 65536) text.append(buf, n);
    CloseHandle(h);

    const uint64_t now = GetTickCount64();
    Lock l;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string c = text.substr(pos, end - pos);
        pos = end + 1;
        while (!c.empty() && (c.back() == '\r' || c.back() == ' ')) c.pop_back();
        if (c.empty()) continue;
        enqueue_locked(c, now);
    }
}

void post(const std::string& cmd) {
    if (!g_lock_init || cmd.empty()) return;
    Lock l;
    enqueue_locked(cmd, GetTickCount64());
}

Snapshot snapshot() {
    Snapshot out;
    if (!g_lock_init) return out;
    const uint64_t now = GetTickCount64();
    Lock l;
    out.s = g_status;
    out.in_song = g_last_publish && now - g_last_publish < kInSongMs;
    out.song = g_song;
    if (!out.in_song) out.s.measure = out.s.measures = 0, out.s.bgm = out.s.started = false, out.s.speed_ok = true, out.s.paused = 0;
    return out;
}

void write_status_once(bool force) {
    const uint64_t now = GetTickCount64();
    std::string body;
    {
        Lock l;
        const bool in_song = g_last_publish && now - g_last_publish < kInSongMs;
        char buf[768];
        std::snprintf(buf, sizeof(buf),
                      "in_song=%d\nsong=%s\ntraining=%d\nrepeat=%d\nstart=%d\nend=%d\nmeasure=%d\nmeasures=%d\nbgm=%d\n"
                      "time=%.3f\nstarted=%d\nspeed=%.2f\nspeed_ok=%d\npaused=%d\nscale_windows=%d\n"
                      "fixed_scroll=%d\nno_camera=%d\n",
                      in_song, in_song ? g_song.c_str() : "", g_status.training, g_status.repeat, g_status.start,
                      g_status.end, in_song ? g_status.measure : 0, in_song ? g_status.measures : 0,
                      in_song && g_status.bgm, in_song ? g_status.time : 0.0, in_song && g_status.started,
                      g_status.speed, !in_song || g_status.speed_ok, in_song ? g_status.paused : 0,
                      g_status.scale_windows, g_status.fixed_scroll, g_status.no_camera);
        body = buf;
    }
    // rewrite at least every 500 ms so readers can tell a live hook from a stale file
    if (!force && body == g_last_written && now - g_last_write < 500) return;
    const std::string text = "tick=" + std::to_string(now) + "\n" + body;
    HANDLE h = CreateFileW(g_status_tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(h, text.data(), DWORD(text.size()), &n, nullptr);
    CloseHandle(h);
    if (MoveFileExW(g_status_tmp.c_str(), g_status_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        g_last_written = body;
        g_last_write = now;
    }
}

void set_speed_config(const TrainerConfig& cfg) {
    Lock l;
    g_speed_cfg = cfg;
}

void seed(bool training, bool repeat, double speed, bool scale_windows, bool fixed_scroll, bool no_camera) {
    Lock l;
    g_status.training = training;
    g_status.repeat = repeat;
    g_status.speed = speed;
    g_status.scale_windows = scale_windows;
    g_status.fixed_scroll = fixed_scroll;
    g_status.no_camera = no_camera;
}

void publish(const Status& s) {
    Lock l;
    g_status = s;
    g_last_publish = GetTickCount64();
}

void set_song(const std::string& name) {
    Lock l;
    g_song = name;
}

std::optional<std::string> pop_command(uint64_t now_ms) {
    Lock l;
    while (!g_queue.empty()) {
        auto [c, t] = g_queue.front();
        g_queue.pop_front();
        if (is_switch(c) || now_ms - t <= kStaleCmdMs) return c;
        logf("remote: dropped stale command \"%s\" (%llu ms old)", c.c_str(), (unsigned long long)(now_ms - t));
    }
    return std::nullopt;
}

}  // namespace tr::remote
