#include "core/internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace tr::core::detail {

FnRotate g_orig_rotate = nullptr;
FnSetF g_orig_cam_dist = nullptr, g_orig_cam_pitch = nullptr;
uint64_t g_last_frame_ms = 0;

static bool camera_off() {
    return g_trainer.enabled() && g_trainer.no_camera() && g_s.world && GetTickCount64() - g_last_frame_ms < 500;
}

void hk_rotate(void* self, const float* center, float angle) {
    if (g_s.world) g_s.engine = self;
    g_orig_rotate(self, center, camera_off() ? 0.0f : angle);
}

void hk_cam_dist(void* self, float v) {
    if (g_s.world) g_s.gworld = self;
    g_orig_cam_dist(self, camera_off() ? *g_api.cam_distance : v);
}

void hk_cam_pitch(void* self, float v) {
    if (g_s.world) g_s.gworld = self;
    g_orig_cam_pitch(self, camera_off() ? *g_api.cam_pitch : v);
}

FnVoid g_orig_morph = nullptr;

void hk_morph(void* self) {
    g_orig_morph(self);
    auto* v = camera_off() ? static_cast<float*>(field(self, g_api.off_morph_values)) : nullptr;
    if (v && readable(v, 8)) v[0] = v[1] = 0.0f;
}

// The updaters only call the setters when a value changes, so this resets a lane that was already tilted when the switch was turned on.
void apply_camera() {
    if (!g_orig_rotate || !camera_off()) return;
    if (!g_s.camera_logged) {
        g_s.camera_logged = true;
        logf("training: camera events off (lane rotation 0, camera distance %.2f, pitch %.2f)", *g_api.cam_distance,
             *g_api.cam_pitch);
    }
    if (g_s.engine && is_class(g_s.engine, cls::Engine)) {
        using FnCenter = float* (*)(void*, float*);
        float c[2];
        reinterpret_cast<FnCenter>(g_api.vft_engine[slot::EngineCenter])(g_s.engine, c);
        g_orig_rotate(g_s.engine, c, 0.0f);
    }
    if (g_s.gworld && is_class(g_s.gworld, cls::GWorld)) {
        g_orig_cam_dist(g_s.gworld, *g_api.cam_distance);
        g_orig_cam_pitch(g_s.gworld, *g_api.cam_pitch);
    }
}

constexpr uint32_t kSpcRecord = 0x38, kSpcType = 0x18;

// These are removed before world init builds the chart, so turning lane events off only affects the next chart.
static void drop_events(uint8_t* data) {
    auto* list = data + g_api.off_spc_list;  // std::list {head, size}; nodes {next, prev, record}
    uint8_t* head = nullptr;
    size_t size = 0;
    if (!g_api.off_spc_list || !g_trainer.no_camera() || !read(list, head) || !read(list + 8, size) || !readable(head, 16)) return;
    int lanes = 0;
    uint8_t* n = at<uint8_t*>(head, 0);
    for (size_t i = 0; i < size && n != head && readable(n, 0x10 + kSpcRecord); i++) {
        uint8_t* next = at<uint8_t*>(n, 0);
        const int type = at<int>(n, 0x10 + kSpcType);
        if (type == g_api.spc_lane_y || (type >= g_api.spc_morph0 && type < g_api.spc_morph0 + 4)) {
            // shortcut: the node is unlinked, not freed (the game's allocator), a few 0x48-byte nodes per chart
            uint8_t* prev = at<uint8_t*>(n, 8);
            std::memcpy(prev, &next, 8);
            std::memcpy(next + 8, &prev, 8);
            lanes++;
        }
        n = next;
    }
    size -= lanes;
    std::memcpy(list + 8, &size, 8);
    if (lanes) logf("training: dropped %d lane events from this chart", lanes);
}

FnWorldInit g_orig_world_init = nullptr;

static uint8_t* g_fs_fd = nullptr;
static std::vector<float> g_fs_mult;
static std::vector<uint8_t> g_fs_stop;
static uint8_t g_fs_const = 0;
static bool g_fs_changed = false;

// With ConstantScroll on, the game scrolls every BPM section at the main BPM. Scroll stops still happen.
bool set_fixed_scroll(void* world, bool on) {
    uint8_t* fd = nullptr;
    if (!g_api.fd_rep || !world || !read(static_cast<uint8_t*>(world) + g_api.off_world_fdata, fd) ||
        !readable(fd, g_api.off_fd_const + 1) || on == (fd == g_fs_fd))
        return false;
    uint8_t* beg = at<uint8_t*>(fd, g_api.off_fd_table);
    uint8_t* end = at<uint8_t*>(fd, g_api.off_fd_table + 8);
    const size_t n = beg && end > beg ? size_t(end - beg) / g_api.fd_stride : 0;
    if (!n || !readable(beg, n * g_api.fd_stride)) return false;
    auto entry = [&](size_t i) { return beg + i * g_api.fd_stride; };
    auto mult = [&](size_t i) { return reinterpret_cast<float*>(entry(i) + g_api.fd_mult); };
    if (!on) {
        if (g_fs_mult.size() == n)
            for (size_t i = 0; i < n; i++) {
                *mult(i) = g_fs_mult[i];
                entry(i)[g_api.fd_stop] = g_fs_stop[i];
            }
        fd[g_api.off_fd_const] = g_fs_const;
        g_fs_fd = nullptr;
        if (g_fs_changed) logf("training: fixed scroll off (%zu BPM sections restored)", n);
        return g_fs_changed;
    }
    g_fs_mult.resize(n);
    g_fs_stop.resize(n);
    for (size_t i = 0; i < n; i++) {
        g_fs_mult[i] = *mult(i);
        g_fs_stop[i] = entry(i)[g_api.fd_stop];
    }
    g_fs_const = fd[g_api.off_fd_const];
    float rep = 0;
    if (!g_fs_const) {
        fd[g_api.off_fd_const] = 1;
        rep = g_api.fd_rep(fd);
        for (size_t i = 0; i < n; i++) {
            const float bpm = at<float>(entry(i), g_api.fd_bpm);
            if (bpm > 0) *mult(i) = *mult(i) / bpm * rep;
        }
    }
    size_t stops = 0;
    for (size_t i = 0; i < n; i++)
        if (entry(i)[g_api.fd_stop]) {
            entry(i)[g_api.fd_stop] = 0;
            stops++;
        }
    g_fs_fd = fd;
    g_fs_changed = !g_fs_const || stops;
    if (g_fs_const) logf("training: fixed scroll on (the chart levels its own scroll; %zu scroll stops removed)", stops);
    else logf("training: fixed scroll on (%zu BPM sections levelled to %.1f BPM, %zu scroll stops removed)", n, rep, stops);
    return g_fs_changed;
}

void hk_world_init(void* world, void* arg) {
    g_fs_fd = nullptr;
    void* vox = g_trainer.enabled() ? field(world, g_api.off_world_vox) : nullptr;
    auto* data = vox ? static_cast<uint8_t*>(vox) + g_api.off_vox_data : nullptr;
    if (data) drop_events(data);
    g_orig_world_init(world, arg);
    set_fixed_scroll(world, g_trainer.enabled() && g_trainer.fixed_scroll());
}

}  // namespace tr::core::detail
