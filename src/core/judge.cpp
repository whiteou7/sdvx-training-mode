#include "core/internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tr::core::detail {

static float* g_lane_orig = nullptr;
void* g_lane_range = nullptr;
float g_lane_v = 1.0f;
constexpr uint32_t kNoteList = 0x50;  // object manager: vector<shared_ptr<INote>>

static bool tick_vector(void* note, uint8_t*& b, uint8_t*& e) {
    auto* ji = static_cast<uint8_t*>(note) + 8;
    void** vft = nullptr;
    void* fn = nullptr;
    if (!read(ji, vft) || !read(vft + 2, fn) || !g_mod.in_exec(fn)) return false;
    auto* vec = static_cast<uint8_t**>(reinterpret_cast<void* (*)(void*)>(fn)(ji));
    if (!readable(vec, 16)) return false;
    b = vec[0];
    e = vec[1];
    return b && e > b && (e - b) % 0x30 == 0 && e - b <= 0x30 * 4096 && readable(b, size_t(e - b));
}

static bool tick_span(void* note, float& first, float& last) {
    uint8_t *b = nullptr, *e = nullptr;
    if (!tick_vector(note, b, e)) return false;
    first = at<float>(b, 0);
    last = at<float>(e - 0x30, 0);
    return std::isfinite(first) && std::isfinite(last);
}

static bool note_range(uint8_t*& beg, uint8_t*& end) {
    beg = at<uint8_t*>(g_s.objmgr, kNoteList);
    end = at<uint8_t*>(g_s.objmgr, kNoteList + 8);
    return beg && end > beg && (end - beg) % 16 == 0 && end - beg <= 16 * 50000 && readable(beg, size_t(end - beg));
}

static bool is_game_note(void* note) {
    const std::string nc = note ? g_rtti.class_of(note) : std::string();
    return nc.size() >= 11 && !nc.compare(nc.size() - 11, 11, "Note@Game@@");
}

static bool find_referee(void* fumen) {
    if (g_s.referee_searched) return g_s.referee != nullptr;
    g_s.referee_searched = true;
    for (uint32_t off = 8; off <= 0x80; off += 8) {
        void* m = field(fumen, off);
        if (!m || !readable(m, 0x200)) continue;
        for (uint32_t r = 0; r + 8 <= 0x200; r += 8) {
            void* q = at<void*>(m, r);
            uint32_t sub = 1;
            if (q && g_rtti.class_of(q, &sub) == cls::Referee && sub == 0) {
                g_s.objmgr = m;
                g_s.referee = q;
                logf("NoteReferee %p at fumen+0x%x -> +0x%x", q, off, r);
                return true;
            }
        }
    }
    logf("NoteReferee not found: forward seeks will judge the skipped notes ERROR");
    return false;
}

// Without this, every skipped note would be judged ERROR. Running the referee's timeout check with its clock at the
// seek target marks them as misses instead. A later backward seek clears them (RewindJudgementsAfter).
Marked mark_skipped(void* world, double from, double to) {
    Marked r;
    if (!find_referee(field(world, g_api.off_fumen))) return r;
    auto* ref = static_cast<uint8_t*>(g_s.referee);
    void** rvft = nullptr;
    float saved = 0;
    if (!read(ref, rvft) || rvft != g_api.vft_referee || !read(ref + g_api.off_referee_now, saved)) return r;
    auto timeout = reinterpret_cast<FnRefereeTimeout>(rvft[slot::RefereeTimeout]);
    const float judge_new = saved + float(to - from);
    std::memcpy(ref + g_api.off_referee_now, &judge_new, sizeof(float));
    uint8_t *beg = nullptr, *end = nullptr;
    if (note_range(beg, end)) {
        for (uint8_t* e = beg; e < end; e += 16) {
            void* note = at<void*>(e, 0);
            float first = 0, last = 0;
            if (!is_game_note(note) || !tick_span(note, first, last)) continue;
            if (last < from - 1.0 || first > to) continue;
            int n = 0;
            alignas(16) uint8_t res[0x40];
            for (; n < 4096; n++) {
                std::memset(res, 0, sizeof(res));
                timeout(ref, res, e, judge_new);
                if (at<int>(res, 0) == 0) break;
            }
            if (n) r.notes++, r.ticks += n;
        }
    }
    std::memcpy(ref + g_api.off_referee_now, &saved, sizeof(float));
    return r;
}

constexpr size_t kMaxWindowEntries = 600000;

// A tick holds std::map<kind, bound> at +8. MSVC tree node: left, parent, right, color, isnil(+0x19),
// kind at +0x1c, bound at +0x20; the head keeps the root as its parent.
static bool walk_windows(uint8_t* tick, std::vector<WinEntry>& out) {
    uint8_t* head = nullptr;
    uint64_t size = 0;
    if (!read(tick + 8, head) || !read(tick + 16, size) || size > 16 || !readable(head, 0x28)) return false;
    if (size == 0) return true;
    if (at<uint8_t>(head, 0x19) != 1) return false;
    uint8_t* stack[40];
    int n = 0;
    uint8_t* root = at<uint8_t*>(head, 8);
    if (root) stack[n++] = root;
    uint64_t seen = 0;
    while (n) {
        uint8_t* node = stack[--n];
        if (!readable(node, 0x28) || at<uint8_t>(node, 0x19)) continue;
        if (++seen > size || n + 2 > 40) return false;
        const float v = at<float>(node, 0x20);
        if (!std::isfinite(v) || std::fabs(v) >= 1.0f) return false;
        out.push_back({reinterpret_cast<float*>(node + 0x20), v});
        stack[n++] = at<uint8_t*>(node, 0);
        stack[n++] = at<uint8_t*>(node, 0x10);
    }
    return seen == size;
}

static float scaled(float orig, double factor) { return float(double(orig) * factor); }

static bool build_windows(void* fumen) {
    WinTable& w = g_s.win;
    w = WinTable{};
    if (!find_referee(fumen)) {
        w.refused = true;
        return false;
    }
    uint8_t *beg = nullptr, *end = nullptr;
    if (!note_range(beg, end)) {
        w.refused = true;
        logf("judge windows: note list not readable; windows stay fixed");
        return false;
    }
    size_t ticks = 0, notes = 0;
    for (uint8_t* e = beg; e < end; e += 16) {
        void* note = at<void*>(e, 0);
        uint8_t *tb = nullptr, *te = nullptr;
        if (!is_game_note(note) || !tick_vector(note, tb, te)) continue;
        notes++;
        for (uint8_t* t = tb; t < te; t += 0x30, ticks++) {
            if (!walk_windows(t, w.e) || w.e.size() > kMaxWindowEntries) {
                w = WinTable{};
                w.refused = true;
                logf("judge windows: a tick map does not look like a std::map; windows stay fixed");
                return false;
            }
        }
    }
    w.notes_begin = beg;
    w.notes = size_t((end - beg) / 16);
    w.first_note = at<void*>(beg, 0);
    w.built = true;
    char sample[160] = {};
    for (size_t i = 0; i < w.e.size() && i < 8; i++) {
        char one[24];
        std::snprintf(one, sizeof(one), " %.4f", double(w.e[i].orig));
        std::strncat(sample, one, sizeof(sample) - std::strlen(sample) - 1);
    }
    logf("judge windows: %zu notes, %zu ticks, %zu bounds found; first bounds:%s", notes, ticks, w.e.size(), sample);
    return true;
}

// At speed s, a window of X ms lasts X/s ms in real time, so every bound is multiplied by s.
void apply_windows(void* world, double factor) {
    WinTable& w = g_s.win;
    if (w.refused || !(factor > 0.0)) return;
    void* fumen = fumen_of(world);
    if (!fumen) return;
    if (!w.built) {
        if (factor == 1.0 || !g_s.offset_ok) return;
        if (!build_windows(fumen)) return;
    }
    if (w.factor == factor) return;
    uint8_t *beg = nullptr, *end = nullptr;
    bool same = note_range(beg, end) && beg == w.notes_begin && size_t((end - beg) / 16) == w.notes &&
                at<void*>(beg, 0) == w.first_note;
    for (size_t i = 0; same && i < w.e.size(); i++) {
        float cur = 0;
        same = read(w.e[i].p, cur) && cur == scaled(w.e[i].orig, w.factor);
    }
    if (!same) {
        logf("judge windows: the chart changed under the table; rebuilding");
        w = WinTable{};
        return;
    }
    for (auto& e : w.e) *e.p = scaled(e.orig, factor);
    w.factor = factor;
    logf("judge windows x%.3f (%zu bounds)", factor, w.e.size());
}

void restore_lane() {
    if (!g_lane_range) return;
    auto* r = static_cast<uint8_t*>(g_lane_range);
    float* cur = nullptr;
    if (g_api.off_range_speed && is_class(g_lane_range, cls::DrawRange) && read(r + g_api.off_range_speed, cur) &&
        cur == &g_lane_v) {
        std::memcpy(r + g_api.off_range_speed, &g_lane_orig, sizeof(g_lane_orig));
        logf("lane speed: multiplier pointer put back");
    }
    g_lane_range = nullptr;
    g_lane_orig = nullptr;
}

// The note range divides by a hi-speed multiplier it reads through a pointer. The pointer is redirected to a copy divided by
// the speed, so the lane speed stays what the player chose.
void apply_lane() {
    const double s = g_s.scale;
    if (!g_api.vft_range || !g_s.draw.timer || s == 1.0 || !(s > 0.0)) {
        restore_lane();
        return;
    }
    void* range = nullptr;
    if (!read(static_cast<uint8_t*>(g_s.draw.timer) + g_api.off_draw_range, range) || !range || !is_class(range, cls::DrawRange))
        return;
    auto* slotp = static_cast<uint8_t*>(range) + g_api.off_range_speed;
    float* cur = nullptr;
    if (!read(slotp, cur) || !cur) return;
    if (g_lane_range != range) {
        restore_lane();
        if (cur == &g_lane_v || !readable(cur, 4)) return;
        g_lane_range = range;
        g_lane_orig = cur;
        logf("lane speed held: hi-speed multiplier %.3f, divided by the playback speed %.2f", double(*cur), s);
    }
    const float base = *g_lane_orig;
    if (!std::isfinite(base) || base <= 0.0f) return;
    g_lane_v = float(double(base) / s);
    float* mine = &g_lane_v;
    if (cur != mine) std::memcpy(slotp, &mine, sizeof(mine));
}

// Timing offsets are added to the timer in seconds, so a slowed timer would stretch them in real time.
// Each timer is pointed at a copy holding offset * speed, and the game's values are never written.
struct HeldOffsets {
    void* timer = nullptr;
    void* orig = nullptr;
    float copy[4] = {};
};
static HeldOffsets g_offs[2];

static void restore_offsets(HeldOffsets& h, void** vft, uint32_t field) {
    if (!h.timer) return;
    void** v = nullptr;
    float* cur = nullptr;
    auto* slotp = static_cast<uint8_t*>(h.timer) + field;
    if (read(h.timer, v) && v == vft && read(slotp, cur) && cur == h.copy) std::memcpy(slotp, &h.orig, sizeof(h.orig));
    h = HeldOffsets{};
}

void restore_offsets() {
    if (g_offs[0].timer || g_offs[1].timer) logf("timing offsets: timers point at the game's offsets again");
    restore_offsets(g_offs[0], g_api.vft_draw, g_api.off_draw_offs);
    restore_offsets(g_offs[1], g_api.vft_judge, g_api.off_judge_offs);
}

void apply_offsets() {
    const double s = g_s.scale;
    if (!g_api.off_draw_offs || s == 1.0 || !(s > 0.0)) {
        restore_offsets();
        return;
    }
    void* timers[2] = {g_s.draw.timer, g_s.input ? field(g_s.input, 8) : nullptr};
    void** vfts[2] = {g_api.vft_draw, g_api.vft_judge};
    const uint32_t fields[2] = {g_api.off_draw_offs, g_api.off_judge_offs};
    const uint32_t idx[2] = {0, g_api.judge_offs_at / 4};
    for (int i = 0; i < 2; i++) {
        HeldOffsets& h = g_offs[i];
        void** v = nullptr;
        if (h.timer != timers[i]) restore_offsets(h, vfts[i], fields[i]);
        if (!timers[i] || !read(timers[i], v) || v != vfts[i]) continue;
        auto* slotp = static_cast<uint8_t*>(timers[i]) + fields[i];
        float* cur = nullptr;
        if (!read(slotp, cur) || !cur) continue;
        if (cur != h.copy) {
            if (!readable(cur, (idx[i] + 1) * 4)) continue;
            if (!h.timer) logf("timing offsets: %s offset %.1f ms held in real time", i ? "judge" : "display", cur[idx[i]] * 1e3);
            h.timer = timers[i];
            h.orig = cur;
        }
        std::memcpy(h.copy, h.orig, (idx[i] + 1) * 4);
        h.copy[idx[i]] = float(double(h.copy[idx[i]]) * s);
        float* mine = h.copy;
        if (cur != mine) std::memcpy(slotp, &mine, sizeof(mine));
    }
}

FnVoid g_orig_input_update = nullptr;

// Presses are stored in six lane vectors (from +0x28, 0x18 bytes each) as judge time minus how long ago they happened.
// At speed s each press moves to J - s * (J - e). While paused, presses are dropped.
static void fix_presses(void* input) {
    const bool paused = g_s.paused_eff;
    const double s = g_s.scale;
    if (!paused && s == 1.0) return;
    void* judge = field(input, 8);
    void** jv = nullptr;
    if (!judge || !read(judge, jv) || jv != g_api.vft_judge) return;
    const float J = reinterpret_cast<FnGetF>(jv[slot::JudgeGetTime])(judge);
    if (!std::isfinite(J)) return;
    auto* in = static_cast<uint8_t*>(input);
    for (int lane = 1; lane <= 6; lane++) {
        uint8_t* v = in + 0x28 + lane * 0x18;
        if (!readable(v, 24)) return;
        float* b = at<float*>(v, 0);
        float* e = at<float*>(v, 8);
        float* cap = at<float*>(v, 16);
        if (!b || e < b || e > cap || e - b > 64 || (e > b && !readable(b, size_t(e - b) * 4))) continue;
        if (paused) {
            std::memcpy(v + 8, &b, sizeof(b));
            continue;
        }
        for (float* p = b; p < e; p++) *p = float(double(J) - s * (double(J) - double(*p)));
    }
}

void hk_input_update(void* self) {
    g_orig_input_update(self);
    if (self == g_s.input) fix_presses(self);
}

}  // namespace tr::core::detail
