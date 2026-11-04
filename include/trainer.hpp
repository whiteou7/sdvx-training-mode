#pragma once

#include <optional>
#include <string>

#include "vox.hpp"

namespace tr {

struct Keys {
    bool toggle = false;
    bool repeat = false;
    bool set_start = false;
    bool set_end = false;
    bool jump_start = false;
    bool prev = false;
    bool next = false;
    bool clear = false;
    bool speed_up = false;
    bool speed_down = false;
    bool pause = false;
    bool scale_windows = false;
    bool fixed_scroll = false;
    bool no_camera = false;
};

struct TrainerConfig {
    int preroll_measures = 1;
    double restart_grace = 0.35;  // pressing prev this soon after a measure starts goes back one more measure
    double speed_mod_step = 0.05;
    double speed_mod_min = 0.5, speed_mod_max = 2.0;
};

class Trainer {
public:
    explicit Trainer(TrainerConfig cfg = {}) : cfg_(cfg) {}

    void reset_session(const VoxTimeline* tl);
    std::optional<double> update(double now, const Keys& k, std::string* msg);

    bool enabled() const { return enabled_; }
    bool repeat() const { return repeat_; }
    int start() const { return start_; }
    int end() const { return end_; }  // one past the last loop measure
    std::string status(double now) const;
    double speed_setting() const { return speed_; }
    double speed() const { return enabled_ ? speed_ : 1.0; }
    bool scale_windows() const { return scale_windows_; }
    void set_scale_windows(bool on) { scale_windows_ = on; }
    bool fixed_scroll() const { return fixed_scroll_; }
    bool no_camera() const { return no_camera_; }
    bool to_start() const { return to_start_; }

    void set_enabled(bool e) { enabled_ = e; }
    void set_speed(double s);

private:
    double loop_start_time() const;
    void reset_loop();

    TrainerConfig cfg_;
    const VoxTimeline* tl_ = nullptr;
    bool enabled_ = false;
    bool repeat_ = true;
    int start_ = -1, end_ = -1;
    double speed_ = 1.0;
    bool scale_windows_ = true;
    bool fixed_scroll_ = false, no_camera_ = false;
    bool to_start_ = false;
};

}  // namespace tr
