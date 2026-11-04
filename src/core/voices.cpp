#include "core/internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tr::core::detail {

constexpr double kBgmMinMs = 500.0;  // FX and chip samples are shorter than this. The song passes it about 0.5 s in

static void* as_voice(void* p) {
    uint32_t sub = 0;
    if (!p || g_rtti.class_of(p, &sub) != cls::Voice) return nullptr;
    return static_cast<uint8_t*>(p) - sub;
}

static void add_voice(void* p, std::vector<void*>& out) {
    if (void* v = as_voice(p))
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
}

static void scan_for_voices(void* obj, size_t span, std::vector<void*>& out) {
    if (!readable(obj, span)) return;
    for (size_t off = 0; off + 8 <= span; off += 8) {
        void* q = at<void*>(obj, off);
        if (!q) continue;
        add_voice(q, out);
        if (off + 16 > span) continue;
        auto* end = at<uint8_t*>(obj, off + 8);
        auto* beg = static_cast<uint8_t*>(q);
        if (end <= beg || end - beg > 16 * 512 || !readable(beg, end - beg)) continue;
        for (int stride : {16, 8}) {
            if ((end - beg) % stride) continue;
            for (uint8_t* e = beg; e < end; e += stride) add_voice(at<void*>(e, 0), out);
        }
    }
}

struct ActiveVoice {
    std::atomic<void*> obj{nullptr};
    std::atomic<uint64_t> seen{0};
};
constexpr int kActiveSlots = 64;
constexpr uint64_t kActiveMs = 500;
static ActiveVoice g_active[kActiveSlots];
FnMasterRender g_orig_master_render = nullptr;

void note_active_voice(void* obj) {
    const uint64_t now = GetTickCount64();
    int victim = 0;
    uint64_t oldest = UINT64_MAX;
    for (int i = 0; i < kActiveSlots; i++) {
        if (g_active[i].obj.load(std::memory_order_relaxed) == obj) {
            g_active[i].seen.store(now, std::memory_order_relaxed);
            return;
        }
        const uint64_t s = g_active[i].seen.load(std::memory_order_relaxed);
        if (s < oldest) oldest = s, victim = i;
    }
    g_active[victim].seen.store(0, std::memory_order_relaxed);
    g_active[victim].obj.store(obj, std::memory_order_relaxed);
    g_active[victim].seen.store(now, std::memory_order_release);
}

// Runs on the audio thread after the mix callback. The voice list only changes inside that callback, so reading it here is safe.
void master_rendered(void* master) {
    auto* m = static_cast<uint8_t*>(master);
    auto* beg = at<uint8_t*>(m, g_api.off_active_begin);
    auto* end = at<uint8_t*>(m, g_api.off_active_end);
    if (!beg || end < beg || end - beg > 16 * 256) return;
    for (uint8_t* e = beg; e < end; e += 16)  // shared_ptr<IVoiceUpdate>, pointer is VoiceImpl+8
        if (auto* upd = at<uint8_t*>(e, 0)) note_active_voice(upd - 8);
}

int hk_master_render(void* self, void* ctx, int frames) {
    const int r = g_orig_master_render(self, ctx, frames);
    master_rendered(self);
    return r;
}

std::vector<void*> active_voices() {
    std::vector<void*> out;
    const uint64_t now = GetTickCount64();
    for (auto& a : g_active) {
        const uint64_t s = a.seen.load(std::memory_order_acquire);
        void* v = a.obj.load(std::memory_order_relaxed);
        if (v && s && now - s <= kActiveMs && std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    }
    return out;
}

static std::vector<void*> voice_candidates(void* world) {
    std::vector<void*> out;
    void* audio = field(world, g_api.off_audio);
    if (is_class(audio, cls::Audio)) {
        scan_for_voices(audio, 0x170, out);
        for (size_t off = 0; off + 8 <= 0x170; off += 8) {
            void* q = at<void*>(audio, off);
            if (q && q != audio && !g_rtti.class_of(q).empty()) scan_for_voices(q, 0x100, out);
        }
    }
    for (void* v : active_voices())
        if (std::find(out.begin(), out.end(), v) == out.end()) out.push_back(v);
    return out;
}

bool voice_pos(void* v, double& ms) {
    void** vft = nullptr;
    if (!read(v, vft) || vft != g_api.vft_voice) return false;
    auto count = reinterpret_cast<FnSizeT>(vft[slot::VoiceConnCount])(v);
    if (count == 0 || count > 64) return false;  // GetPosition dereferences the first connection
    ms = reinterpret_cast<FnGetD>(vft[slot::VoiceGetPos])(v);
    return std::isfinite(ms) && ms >= 0.0 && ms < 3.6e6;
}

// VoiceImpl+0x38 holds the mixer connections. Each one has its bus at +0x18 (0 = master, 1 = music through time-stretch).
int voice_bus(void* v) {
    void** conns = nullptr;
    void* conn = nullptr;
    uint32_t bus = 0;
    if (!read(static_cast<uint8_t*>(v) + 0x38, conns) || !read(conns, conn) ||
        !read(static_cast<uint8_t*>(conn) + 0x18, bus))
        return -1;
    return bus < 2 ? int(bus) : -1;
}

bool excluded(void* v) { return std::find(g_s.excluded.begin(), g_s.excluded.end(), v) != g_s.excluded.end(); }

// The BGM is the voice that wasn't playing before the world loaded, keeps pace with the game timer, and is past kBgmMinMs.
void* find_bgm(void* world, bool verbose, float t_now) {
    auto cands = voice_candidates(world);
    void* best = nullptr;
    double best_ms = -1;
    for (void* v : cands) {
        double ms;
        const bool ok = voice_pos(v, ms);
        const bool excl = excluded(v);
        auto it = std::find_if(g_s.obs.begin(), g_s.obs.end(), [&](const VoiceObs& o) { return o.voice == v; });
        if (it == g_s.obs.end()) it = g_s.obs.insert(g_s.obs.end(), VoiceObs{v, t_now, ok ? ms : -1, 0, 0});
        if (ok && t_now < it->t) {  // the timer went backwards (the chart restarted), so the old observations are dropped
            it->t = t_now, it->ms = ms, it->rate = 0, it->good = 0;
        }
        if (ok && t_now - it->t >= 0.2f) {
            it->rate = it->ms >= 0 ? (ms - it->ms) / 1000.0 / (t_now - it->t) : 0;
            it->good = (it->rate > 0.9 && it->rate < 1.1) ? it->good + 1 : 0;
            it->t = t_now, it->ms = ms;
        }
        if (verbose)
            logf("  voice %p pos=%s%.1f ms rate=%.3f good=%d bus=%d%s", v, ok ? "" : "n/a ", ok ? ms : 0.0, it->rate,
                 it->good, voice_bus(v), excl ? " (was playing before the chart: ignored)" : "");
        if (ok && !excl && it->good >= 2 && ms > kBgmMinMs && ms > best_ms) best = v, best_ms = ms;
    }
    if (verbose) logf("  %zu voice candidates, BGM guess %p (%.1f ms)", cands.size(), best, best_ms);
    return best;
}

constexpr double kCompanionMs = 300.0;  // the song can play as several voices at once (a dry track and its _fx version)

std::vector<void*> song_voices(void* world, void* bgm) {
    std::vector<void*> out;
    double bgm_ms;
    if (!bgm || !voice_pos(bgm, bgm_ms)) return out;
    out.push_back(bgm);
    for (void* v : voice_candidates(world)) {
        double ms;
        if (v != bgm && !excluded(v) && voice_pos(v, ms) && ms > kBgmMinMs && std::fabs(ms - bgm_ms) <= kCompanionMs)
            out.push_back(v);
    }
    return out;
}

void clear_active_voices() {
    for (auto& a : g_active) a.obj.store(nullptr), a.seen.store(0);
}

}  // namespace tr::core::detail
