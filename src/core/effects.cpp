#include "core/internal.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace tr::core::detail {

namespace {

void noop(void*) {}
void* g_noop_vft[6] = {reinterpret_cast<void*>(&noop), reinterpret_cast<void*>(&noop), reinterpret_cast<void*>(&noop),
                       reinterpret_cast<void*>(&noop), reinterpret_cast<void*>(&noop), reinterpret_cast<void*>(&noop)};
struct {
    void** vft;
} g_noop = {g_noop_vft};

bool is_vft(void* obj, void** vft) {
    void** v = nullptr;
    return read(obj, v) && v == vft;
}

}  // namespace

// fumen+0x18 is the object manager.
float effects_time(void* fumen) {
    void* om = field(fumen, 0x18);
    void* draw = om ? field(om, 0x10) : nullptr;
    return draw && g_api.vft_draw && is_vft(draw, g_api.vft_draw)
               ? reinterpret_cast<FnGetF>(g_api.vft_draw[slot::DrawGetTime])(draw)
               : -1.0f;
}

// After a seek, the game's rebuild triggers every chart effect up to the new time again, restarting animations.
// Only the effects the seek skipped over are let through. If the seek went back past an effect, all of them replay.
void rebuild_after_seek(void* fumen, float before) {
    void* om = field(fumen, 0x18);
    const float after = effects_time(fumen);
    std::vector<std::pair<uint8_t*, void*>> muted;
    if (g_api.vft_event && om && before >= 0 && after >= 0 && readable(om, g_api.off_events + 16)) {
        auto* beg = at<uint8_t*>(om, g_api.off_events);
        auto* end = at<uint8_t*>(om, g_api.off_events + 8);
        std::vector<uint8_t*> fx;
        for (uint8_t* p = beg; p && p < end && readable(p, 16); p += 16)
            if (auto* e = at<uint8_t*>(p, 0); e && readable(e, g_api.off_event_fn + 8) && is_vft(e, g_api.vft_event))
                fx.push_back(e);
        bool crossed = false;
        for (uint8_t* e : fx) {
            const float t = at<float>(e, g_api.off_event_time);
            crossed |= after < before && t > after && t <= before;
        }
        if (!crossed)
            for (uint8_t* e : fx)
                if (at<float>(e, g_api.off_event_time) <= std::min(before, after)) {
                    muted.emplace_back(e, at<void*>(e, g_api.off_event_fn));
                    void* stub = &g_noop;
                    std::memcpy(e + g_api.off_event_fn, &stub, 8);
                }
        if (crossed) logf("seek went back past a chart effect: replaying the effects up to %.3f", after);
    }
    g_api.fumen_rebuild(fumen);
    for (auto& [e, fn] : muted) std::memcpy(e + g_api.off_event_fn, &fn, 8);
}

}  // namespace tr::core::detail
