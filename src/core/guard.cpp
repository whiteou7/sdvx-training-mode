#include "core/internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tr::core::detail {

bool g_prev_enabled = false;
// block exiting to result screen during training
static std::atomic<bool> g_restart_req{false};

bool guard_active() { return g_trainer.enabled(); }

FnVoid g_orig_crash_update = nullptr;

void hk_crash_update(void* self) {
    g_orig_crash_update(self);
    if (!guard_active() || !g_api.vft_crash) return;
    auto* c = static_cast<uint8_t*>(self);
    if (!c[g_api.off_crash_held]) { 
        g_s.chord_down = false;
        return;
    }
    c[g_api.off_crash_held] = 0;
    const float zero = 0.0f;
    std::memcpy(c + g_api.off_crash_time, &zero, sizeof(zero));
    g_s.chord_blocked++;
    if (!g_s.chord_down) g_s.chord_jump = g_s.chord_down = true;
    if (!g_s.chord_logged) {
        g_s.chord_logged = true;
        logf("training: the FX-L + FX-R + START chord jumps to the start instead of ending the track");
    }
}

FnWorldFinished g_orig_world_finished = nullptr;

uint8_t hk_world_finished(void* self) {
    const uint8_t r = g_orig_world_finished(self);
    if (!r || !guard_active() || self != g_s.world) return r;
    static_cast<uint8_t*>(self)[g_api.off_finished] = 0;
    g_restart_req.store(true);
    return 0;
}

struct Players {
    uint8_t* player[8];
    int count = 0;
};

// A deque of shared_ptr with block size 1, so player i is at map[(first + i) & (mapsize - 1)].
static Players players_of(void* fumen) {
    Players out;
    if (!g_api.off_players_map) return out;
    auto* f = static_cast<uint8_t*>(fumen);
    uint8_t* map = nullptr;
    uint64_t mapsize = 0, first = 0, count = 0;
    if (!read(f + g_api.off_players_map, map) || !read(f + g_api.off_players_mapsize, mapsize) ||
        !read(f + g_api.off_players_first, first) || !read(f + g_api.off_players_count, count))
        return out;
    if (!map || mapsize == 0 || mapsize > 64 || (mapsize & (mapsize - 1)) || count == 0 || count > std::size(out.player) ||
        !readable(map, size_t(mapsize) * 8))
        return out;
    for (uint64_t i = 0; i < count; i++) {
        uint8_t* block = at<uint8_t*>(map, size_t((first + i) & (mapsize - 1)) * 8);
        uint8_t* player = nullptr;
        if (block && read(block, player) && readable(player, 16)) out.player[out.count++] = player;
    }
    return out;
}

// Gauge record (12 bytes): u32, float gauge, u8 cleared, u8 finished, pad.
static bool last_record(uint8_t* player, uint8_t*& rec) {
    uint8_t* b = at<uint8_t*>(player, 0);
    uint8_t* e = at<uint8_t*>(player, 8);
    if (!b || e <= b || (e - b) % 12 || (e - b) > 12 * (1 << 22) || !readable(e - 12, 12)) return false;
    rec = e - 12;
    return true;
}

// This runs right after the frame, before the game checks these flags at the start of the next one.
void undo_crash(void* world) {
    void* fumen = fumen_of(world);
    if (!g_api.off_players_map || !fumen) return;
    const Players players = players_of(fumen);
    bool alive = false;
    for (int i = 0; i < players.count; i++) {
        uint8_t* rec = nullptr;
        if (last_record(players.player[i], rec) && !rec[9]) alive = true;
    }
    int fixed = 0;
    for (int i = 0; i < players.count && !alive; i++) {
        uint8_t* rec = nullptr;
        if (!last_record(players.player[i], rec) || !rec[9]) continue;
        const bool seen = i < int(g_s.snaps.size()) && g_s.snaps[size_t(i)].player == players.player[i];
        const PlayerSnap* before = seen ? &g_s.snaps[size_t(i)] : nullptr;
        if (before && before->finished) continue;
        rec[9] = 0;
        if (before) rec[8] = before->cleared;
        fixed++;
    }
    auto* forced = static_cast<uint8_t*>(fumen) + g_api.off_forced_crash;
    if (g_api.off_forced_crash && readable(forced, 1) && *forced && !g_s.forced_snap) {
        *forced = 0;
        fixed++;
    }
    if (!fixed) return;
    g_s.crash_undone += fixed;
    if (!g_s.crash_logged) {
        g_s.crash_logged = true;
        logf("training: a track crash was prevented (gauge ran out or a forced crash)");
    }
}

void snapshot_guard(void* world) {
    g_s.snaps.clear();
    g_s.forced_snap = false;
    void* fumen = world && guard_active() && g_api.off_players_map ? fumen_of(world) : nullptr;
    if (!fumen) return;
    const Players players = players_of(fumen);
    for (int i = 0; i < players.count; i++) {
        uint8_t* player = players.player[i];
        uint8_t* rec = nullptr;
        if (last_record(player, rec)) g_s.snaps.push_back({player, at<float>(rec, 4), rec[8], rec[9]});
        else g_s.snaps.push_back({player, 0.0f, 0, 0});
    }
    auto* forced = static_cast<uint8_t*>(fumen) + g_api.off_forced_crash;
    g_s.forced_snap = g_api.off_forced_crash && readable(forced, 1) && *forced;
}

static PlayerSnap* gauge_before_chart(uint8_t* player) {
    const auto it = std::find_if(g_s.gauge0.begin(), g_s.gauge0.end(), [&](const PlayerSnap& s) { return s.player == player; });
    return it == g_s.gauge0.end() ? nullptr : &*it;
}

void remember_gauge(void* world, float t_now) {
    void* fumen = g_api.off_players_map && t_now <= 0.05f ? fumen_of(world) : nullptr;
    if (!fumen) return;
    const Players players = players_of(fumen);
    for (int i = 0; i < players.count; i++) {
        uint8_t* rec = nullptr;
        if (!last_record(players.player[i], rec) || rec[9] || gauge_before_chart(players.player[i])) continue;
        g_s.gauge0.push_back({players.player[i], at<float>(rec, 4), rec[8], 0});
    }
}

void refill_gauge(void* world) {
    void* fumen = g_api.off_players_map ? fumen_of(world) : nullptr;
    if (!fumen) return;
    const Players players = players_of(fumen);
    for (int i = 0; i < players.count; i++) {
        uint8_t* rec = nullptr;
        const PlayerSnap* before = gauge_before_chart(players.player[i]);
        if (!before || !last_record(players.player[i], rec)) continue;
        std::memcpy(rec + 4, &before->gauge, sizeof(float));
        rec[8] = before->cleared;
        rec[9] = 0;
        logf("gauge refilled to %.1f", before->gauge);
    }
}

float chart_end_time(void* world) {
    void* fumen = g_api.off_chart_end ? fumen_of(world) : nullptr;
    if (!fumen) return -1.0f;
    void* om = g_s.objmgr ? g_s.objmgr : field(fumen, 0x18);
    float t = 0;
    if (!om || !read(static_cast<uint8_t*>(om) + g_api.off_chart_end, t) || !std::isfinite(t) || t < 1.0f || t > 7200.0f)
        return -1.0f;
    return t;
}

void handle_pause(void* world, float t_now, bool key) {
    if (!g_trainer.enabled()) {
        g_s.pause_req = 0;
        return;
    }
    const float end_t = guard_active() ? chart_end_time(world) : -1.0f;
    const bool running = g_s.timer && at<uint8_t>(g_s.timer, g_api.off_timer_running) != 0;
    if (key) {
        if (g_s.pause_req == 0) {
            if (g_s.offset_ok && running && !g_s.speed_refused && g_api.vft_stretch) {
                g_s.pause_req = 1;
                logf("paused at timer %.3f", t_now);
            } else {
                logf("pause ignored: the chart is not running yet, or the music is not on the time-stretch bus");
            }
        } else if (g_s.pause_req == 2 && end_t > 0 && t_now >= end_t - kEndMargin - 0.05f) {
            logf("resume refused: the chart is at its end; seek back first");
        } else {
            g_s.pause_req = 0;
            logf("resumed at timer %.3f", t_now);
        }
    }
    if (end_t > 0 && g_s.offset_ok && !g_s.speed_refused && g_api.vft_stretch) {
        if (g_s.pause_req == 0 && t_now >= end_t - kEndMargin) {
            g_s.pause_req = 2;
            logf("training: the chart reached its end (%.3f): paused", double(end_t));
        } else if (g_s.pause_req == 2 && t_now < end_t - kEndMargin - 0.05f) {
            g_s.pause_req = 1;
        }
    }
}

static void forget_chart() {
    g_s.win = WinTable{};
    g_s.snaps.clear();
    g_s.forced_snap = false;
    g_s.referee_searched = false;
    g_s.referee = nullptr;
    g_s.objmgr = nullptr;
}

static void on_restart_reset() {
    g_s.restarts++;
    logf("the chart is being (re)built: dropping what pointed into the old one");
    g_s.pause_req = 0;
    g_s.paused_eff = false;
    restore_lane();
    restore_offsets();
    forget_chart();
    g_s.fumen_seen = nullptr;
    g_s.bgm = nullptr;
    g_s.offset_ok = false;
    g_s.obs.clear();
    g_s.drift_frames = 0;
    g_s.bgm_rescan = 0;
    g_s.settle = 0;
    g_s.sync_lock = false;
    g_s.draw.known = false;
    g_req_reset.store(true);
}

static bool do_restart(void* world, const char* why) {
    if (!g_api.state_retry || !g_api.audio_stop || !g_api.counter_get || !g_api.sp_release) {
        if (!g_s.restart_logged) logf("restart (%s) unavailable: this build was not recognised", why);
        g_s.restart_logged = true;
        return false;
    }
    auto* w = static_cast<uint8_t*>(world);
    void* cur = nullptr;
    void* pend = nullptr;
    if (!read(w + g_api.off_state_fn, cur) || !read(w + g_api.off_pending, pend)) return false;
    if (cur == g_api.state_retry || pend == g_api.state_retry) return false;
    if (cur != g_api.state_playing && cur != g_api.state_end) return false;
    void* timer = field(world, g_api.off_timer);
    void** tvft = nullptr;
    if (!read(timer, tvft) || tvft != g_api.vft_timer) return false;
    logf("restart (%s)", why);
    reinterpret_cast<FnVoid>(tvft[slot::TimerStop])(timer);
    g_api.audio_stop(field(world, g_api.off_audio));
    if (void* ctx = field(world, g_api.off_context)) {
        void* sp[2] = {};
        void** r = g_api.counter_get(ctx, sp);
        int* counter = r ? static_cast<int*>(*r) : nullptr;
        if (counter && readable(counter, 4)) ++*counter;
        g_api.sp_release(sp);
    }
    const void* fn = g_api.state_retry;
    std::memcpy(w + g_api.off_pending, &fn, sizeof(fn));
    const uint64_t zero = 0;
    std::memcpy(w + g_api.off_pending + 8, &zero, sizeof(zero));
    const uint32_t req[2] = {0, 1};  // sub-state 0, request flag set
    std::memcpy(w + g_api.off_subreq, req, sizeof(req));
    on_restart_reset();
    g_s.in_retry = true;
    return true;
}

void process_restart(void* world, double chart_now) {
    const bool en = g_trainer.enabled();
    const bool left_training = g_prev_enabled && !en;
    g_prev_enabled = en;
    if (!en) g_s.pause_req = 0;
    if (!g_api.state_retry) return;
    auto* w = static_cast<uint8_t*>(world);
    void* cur = nullptr;
    void* pend = nullptr;
    if (!read(w + g_api.off_state_fn, cur) || !read(w + g_api.off_pending, pend)) return;
    if (cur == g_api.state_retry || pend == g_api.state_retry) {
        if (!g_s.in_retry) {
            g_s.in_retry = true;
            on_restart_reset();
        }
        g_restart_req.store(false);
        return;
    }
    g_s.in_retry = false;
    const bool scroll_changed = set_fixed_scroll(world, en && g_trainer.fixed_scroll());
    const char* why = nullptr;
    if (left_training) why = "training left";
    else if (scroll_changed) why = "fixed scroll changed";
    else if (g_restart_req.exchange(false)) why = "result blocked";
    else if (en && (cur == g_api.state_end || pend == g_api.state_end)) why = "the track ended";
    if (why && do_restart(world, why) && scroll_changed && en) g_s.resume_at = chart_now;
}

void refresh_chart_caches(void* world) {
    void* fumen = field(world, g_api.off_fumen);
    if (fumen == g_s.fumen_seen) return;
    g_s.fumen_seen = fumen;
    forget_chart();
}

}  // namespace tr::core::detail
