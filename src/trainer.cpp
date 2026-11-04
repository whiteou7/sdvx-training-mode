#include "trainer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tr {

void Trainer::reset_session(const VoxTimeline* tl) {
    tl_ = tl;
    reset_loop();
}

void Trainer::reset_loop() {
    start_ = end_ = -1;
    if (tl_ && tl_->count() >= 2) {
        start_ = 0;
        end_ = tl_->count() - 1;
    }
}

void Trainer::set_speed(double s) {
    // round to the step so repeated +/- doesn't build up rounding error
    s = std::round(s / cfg_.speed_mod_step) * cfg_.speed_mod_step;
    speed_ = std::clamp(s, cfg_.speed_mod_min, cfg_.speed_mod_max);
}

double Trainer::loop_start_time() const {
    return tl_->start_of(std::max(0, start_ - cfg_.preroll_measures));
}

std::string Trainer::status(double now) const {
    char buf[200];
    const int cur = tl_ ? tl_->measure_at(now) + 1 : 0;
    std::snprintf(buf, sizeof(buf), "training=%s repeat=%s start=%d end=%d measure=%d speed=%.2f judge_scale=%s fixed_scroll=%s no_camera=%s t=%.3f",
                  enabled_ ? "on" : "off", repeat_ ? "on" : "off", start_ >= 0 ? start_ + 1 : 0, end_ >= 0 ? end_ : 0, cur, speed_,
                  scale_windows_ ? "on" : "off", fixed_scroll_ ? "on" : "off", no_camera_ ? "on" : "off", now);
    return buf;
}

std::optional<double> Trainer::update(double now, const Keys& k, std::string* msg) {
    auto say = [&](const char* what) {
        if (msg) *msg = std::string(what) + " | " + status(now);
    };
    to_start_ = false;

    if (k.toggle) {
        enabled_ = !enabled_;
        say(enabled_ ? "training enabled" : "training disabled");
        return std::nullopt;
    }
    if (k.scale_windows) {
        scale_windows_ = !scale_windows_;
        say(scale_windows_ ? "judge windows scale with speed" : "judge windows stay fixed");
    }
    if (k.fixed_scroll) {
        fixed_scroll_ = !fixed_scroll_;
        say(fixed_scroll_ ? "fixed scroll on" : "fixed scroll off");
    }
    if (k.no_camera) {
        no_camera_ = !no_camera_;
        say(no_camera_ ? "camera events off" : "camera events on");
    }
    if (k.speed_up || k.speed_down) {
        set_speed(speed_ + (k.speed_up ? cfg_.speed_mod_step : -cfg_.speed_mod_step));
        say(k.speed_up ? "speed up" : "speed down");
    }
    if (!enabled_ || !tl_ || tl_->count() < 2) return std::nullopt;

    const int last = tl_->count() - 2;
    const int cur = std::min(tl_->measure_at(now), last);

    if (k.repeat) {
        repeat_ = !repeat_;
        say(repeat_ ? "repeat on" : "repeat off");
    }
    if (k.clear) {
        reset_loop();
        say("loop reset to the whole chart");
    }
    if (k.set_start) {
        start_ = cur;
        if (end_ >= 0 && end_ <= start_) end_ = start_ + 1;
        say("loop start set");
    }
    if (k.set_end) {
        end_ = cur + 1;
        if (start_ < 0 || start_ >= end_) start_ = std::max(0, end_ - 1);
        say("loop end set");
    }

    if (k.jump_start) {
        const double t = start_ >= 0 ? loop_start_time() : tl_->start_of(cur);
        say("jump to loop start");
        to_start_ = true;
        return t;
    }
    if (k.prev) {
        int target = cur;
        if (now - tl_->start_of(cur) < cfg_.restart_grace) target = cur - 1;
        target = std::max(0, target);
        say("seek back");
        return tl_->start_of(target);
    }
    if (k.next) {
        say("seek forward");
        return tl_->start_of(std::min(cur + 1, last));
    }

    if (repeat_ && start_ >= 0 && end_ > start_ && now >= tl_->start_of(end_)) {
        say("loop");
        to_start_ = true;
        return loop_start_time();
    }
    return std::nullopt;
}

}  // namespace tr
