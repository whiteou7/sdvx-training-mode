#include "core/internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tr::core::detail {

// Voice bus 1 (the music) goes through a TimeStretchAudioFeeder, but the game never calls its SetRate. Its read is hooked
// and the rate is set there, which changes the music speed without changing pitch. Sound effects on bus 0 are not affected.
static std::atomic<bool> g_req_paused{false};

FnFeederRead g_orig_stretch_read = nullptr;
FnSetRate g_stretch_set_rate = nullptr;
static std::atomic<float> g_req_speed{1.0f};
static std::atomic<uint64_t> g_req_tick{0};  // if no chart has asked for 300 ms, speed 1 is used
std::atomic<bool> g_req_reset{false};
static std::atomic<uint64_t> g_stretch_tick{0};
std::atomic<bool> g_stretching{false};
std::atomic<float> g_stretch_lag{0};
static std::atomic<float> g_dropped_lag{0};
// Only the audio thread uses the variables below.
static void* g_stretch_self = nullptr;
static float g_node_rate = 1.0f;
static int64_t g_node_pulled = 0;
static double g_node_heard = 0;

// Temporarily replaces the node's source during a read so the frames it pulls can be counted.
struct CountedSource {
    void** vft;
    void* src;
};
static uint64_t counted_read(void* self, void* buf, int bytes) {
    if (bytes <= 0) return 0;
    void* src = static_cast<CountedSource*>(self)->src;
    const uint64_t n = reinterpret_cast<FnFeederRead>((*static_cast<void***>(src))[slot::FeederRead])(src, buf, bytes);
    g_node_pulled += int64_t(n / 8);
    return n;
}
static void* g_counted_vft[2] = {nullptr, reinterpret_cast<void*>(&counted_read)};

uint64_t hk_stretch_read(void* self, void* buf, int bytes) {
    const uint64_t now = GetTickCount64();
    g_stretch_tick.store(now, std::memory_order_relaxed);
    float speed = g_req_speed.load(std::memory_order_relaxed);
    bool paused = g_req_paused.load(std::memory_order_relaxed);
    if (now - g_req_tick.load(std::memory_order_relaxed) > 300) speed = 1.0f, paused = false;
    if (paused && bytes > 0) {
        std::memset(buf, 0, size_t(bytes));
        return uint64_t(bytes);
    }
    auto* src_field = static_cast<uint8_t*>(self) + g_api.off_stretch_source;
    void* src = at<void*>(self, g_api.off_stretch_source);
    const bool reset = g_req_reset.exchange(false);  // always consumed: a stale request would drop audio later
    if (self != g_stretch_self) g_stretch_self = self, g_node_rate = 1.0f;  // the game made a new node, which starts with stretching off
    const float rate = bytes <= 0 || bytes % 8 || !src ? 1.0f : speed;
    if (rate == 1.0f) {
        if (g_node_rate != 1.0f) g_stretch_set_rate(self, 1.0f), g_node_rate = 1.0f;
        if (g_stretching.exchange(false, std::memory_order_relaxed) && !reset)
            g_dropped_lag.fetch_add(g_stretch_lag.load(std::memory_order_relaxed));
        g_stretch_lag.store(0, std::memory_order_relaxed);
        return g_orig_stretch_read(self, buf, bytes);
    }
    CountedSource counted{g_counted_vft, src};
    void* proxy = &counted;
    std::memcpy(src_field, &proxy, sizeof(proxy));  // safe because the node only reads its source during this call, on this thread
    // After a seek, an empty read at rate 1 turns the node off and clears its buffers.
    if (reset && g_node_rate != 1.0f) {
        g_stretch_set_rate(self, 1.0f);
        g_orig_stretch_read(self, buf, 0);
        g_node_rate = 1.0f;
    }
    if (rate != g_node_rate) g_stretch_set_rate(self, rate), g_node_rate = rate;
    if (!g_stretching.exchange(true, std::memory_order_relaxed) || reset) g_node_pulled = 0, g_node_heard = 0;
    uint64_t n = g_orig_stretch_read(self, buf, bytes);
    std::memcpy(src_field, &src, sizeof(src));
    if (n > uint64_t(bytes)) n = uint64_t(bytes);
    if (n < uint64_t(bytes)) std::memset(static_cast<uint8_t*>(buf) + n, 0, size_t(bytes) - size_t(n));
    g_node_heard += double(n / 8) * rate;
    g_stretch_lag.store(float(double(g_node_pulled) - g_node_heard), std::memory_order_relaxed);
    return uint64_t(bytes);
}

int64_t (*g_clock)() = nullptr;

static int64_t qpc_ns() {
    if (g_clock) return g_clock();
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return c.QuadPart / f.QuadPart * 1000000000 + c.QuadPart % f.QuadPart * 1000000000 / f.QuadPart;
}

static void refuse_if_unsupported(double speed) {
    const uint64_t last = g_stretch_tick.load();  // read before the clock: the audio thread may store a newer tick
    const int64_t age = last ? int64_t(GetTickCount64() - last) : -1;
    const int bus = g_s.bgm ? voice_bus(g_s.bgm) : -1;
    if (!g_api.vft_stretch || age < -100 || age > 1000 || (g_s.bgm && bus != 1)) {
        logf("speed %.2f refused: music not on the time-stretch bus (node %s, last read %lld ms ago, bgm bus %d)", speed,
             g_api.vft_stretch ? "present" : "missing", (long long)age, bus);
        g_s.speed_refused = true;
    } else if (!g_api.vft_draw) {
        logf("speed %.2f refused: this build note-scroll clock (DrawTimer) was not recognised", speed);
        g_s.speed_refused = true;
    }
}

// The GameTimer has no speed setting, so its accumulator is pulled back each frame by (1 - speed) of the frame time.
// While stretching, GetPosition reports too little (the bus is read in bursts), so offset tracking waits until speed is back to 1.
void apply_speed(void* timer, double speed, float t_now) {
    const int64_t now = qpc_ns();
    const int64_t dt = g_s.last_ns ? now - g_s.last_ns : 0;
    g_s.last_ns = now;
    const bool want_pause = g_s.pause_req != 0;

    if ((speed != 1.0 || want_pause) && !g_s.speed_refused) refuse_if_unsupported(speed);
    if (want_pause && g_s.speed_refused) {
        logf("pause cancelled: it needs the time-stretch bus");
        g_s.pause_req = 0;
    }
    auto* t = static_cast<uint8_t*>(timer);
    const bool running = at<uint8_t>(t, g_api.off_timer_running) != 0;
    bool paused = g_s.pause_req != 0;
    if (g_s.speed_refused || !running || !g_s.offset_ok) speed = 1.0, paused = false;
    g_req_speed.store(float(speed));
    g_req_paused.store(paused);
    g_req_tick.store(GetTickCount64());
    g_s.paused_eff = paused;
    g_s.scale = running ? speed : 1.0;
    const double eff = paused ? 0.0 : speed;

    // When the node goes back to pass-through it drops its buffered audio, so the timer is moved ahead by the same amount.
    const float dropped = g_dropped_lag.exchange(0);
    const int64_t jump = int64_t(double(dropped) / kMixRate * 1e9);
    int64_t shift = -jump;
    g_s.shift_speed = running ? eff : 1.0;
    if (!running) return;
    if (eff != 1.0) {
        const int64_t cap = paused ? 10000000000LL : 250000000LL;
        if (dt > 0 && dt < cap) shift += int64_t((1.0 - eff) * double(dt));
        if (!g_s.sync_lock) {
            g_s.sync_lock = true;
            if (paused) logf("paused: timer held at %.3f, music silent", t_now);
            else logf("speed %.2f: music time-stretched, offset %.4f kept", speed, g_s.offset);
        }
    } else if (g_s.sync_lock && !g_stretching.load()) {
        g_s.sync_lock = false;
        g_s.settle = 15;  // let GetPosition catch up before measuring the offset again
        double ms = 0;
        voice_pos(g_s.bgm, ms);
        logf("speed 1: timer moved %.1f ms for the dropped stretch delay; timer-BGM now %+.1f ms", dropped / kMixRate * 1000,
             ((t_now - g_s.offset) - ms / 1000.0) * 1000);
    }
    if (shift) {
        int64_t acc = at<int64_t>(t, g_api.off_timer_acc);
        acc -= shift;
        std::memcpy(t + g_api.off_timer_acc, &acc, sizeof(acc));
    }
    g_s.draw.jump_ns += jump;
}

// DrawTimer advances its clock one fixed frame per update and ignores the game timer, so at speed s,
// (1 - s) of the step is taken back. Both clock fields are changed together because the game measures frame error from their difference.
constexpr int64_t kDrawStepMin = 1000000, kDrawStepMax = 100000000;

void draw_follow(void* world, bool running) {
    if (!g_api.vft_draw) return;
    if (!g_s.draw.timer) {
        if (g_s.draw.search_wait-- > 0) return;
        g_s.draw.search_wait = 30;  // the world builds its components over the first frames
        g_s.draw.timer = find_world_obj(world, cls::Draw);
        g_s.draw.known = false;
        if (!g_s.draw.timer) return;
        logf("  draw timer %p", g_s.draw.timer);
    }
    auto* d = static_cast<uint8_t*>(g_s.draw.timer);
    void** vft = nullptr;
    if (!readable(d, 0x88) || !read(d, vft) || vft != g_api.vft_draw) {
        g_s.draw.timer = nullptr;
        g_s.draw.known = false;
        return;
    }
    const uint32_t oc = g_api.off_draw_clock;
    int64_t clock = at<int64_t>(d, oc), prev = at<int64_t>(d, oc - 8);
    const int64_t advanced = clock - g_s.draw.prev;
    const int64_t step = clock - prev;
    bool changed = false;

    const double s = g_s.shift_speed;
    if (s != 1.0 && running && g_s.draw.run_prev && g_s.draw.known) {
        const uint32_t st[2] = {at<uint32_t>(d, g_api.off_draw_stats), at<uint32_t>(d, g_api.off_draw_stats + 4)};
        if (st[0] == g_s.draw.stats_prev[0] && st[1] == g_s.draw.stats_prev[1] && step >= kDrawStepMin &&
            step <= kDrawStepMax && advanced > 0 && advanced <= 3 * step) {  // the game reset the clock itself (new stats or an unusual step), so it is left alone
            const int64_t cut = int64_t((1.0 - s) * double(advanced));
            clock -= cut;
            prev -= cut;
            changed = true;
        }
    }
    if (g_s.draw.jump_ns) {
        clock += g_s.draw.jump_ns;
        prev += g_s.draw.jump_ns;
        changed = true;
    }
    g_s.draw.jump_ns = 0;
    if (changed) {
        std::memcpy(d + oc, &clock, sizeof(clock));
        std::memcpy(d + oc - 8, &prev, sizeof(prev));
        g_api.draw_recompute(d, d + g_api.off_draw_table);
    }
}

void draw_remember() {
    if (!g_s.draw.timer) return;
    auto* d = static_cast<uint8_t*>(g_s.draw.timer);
    if (!readable(d, 0x88)) return;
    g_s.draw.prev = at<int64_t>(d, g_api.off_draw_clock);
    g_s.draw.stats_prev[0] = at<uint32_t>(d, g_api.off_draw_stats);
    g_s.draw.stats_prev[1] = at<uint32_t>(d, g_api.off_draw_stats + 4);
    g_s.draw.run_prev = g_s.timer && at<uint8_t>(g_s.timer, g_api.off_timer_running) != 0;
    g_s.draw.known = true;
}

}  // namespace tr::core::detail
