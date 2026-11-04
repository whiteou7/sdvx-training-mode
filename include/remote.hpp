#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "trainer.hpp"

namespace tr::remote {

// Scripts control the hook through two files next to the DLL. sdvx_training.cmd is read and deleted in one step,
// and sdvx_training.status is written to a temp file and renamed. All file I/O runs on a background thread.
struct Status {
    bool training = false;
    bool repeat = true;
    int start = 0, end = 0;
    int measure = 0;
    int measures = 0;
    bool bgm = false;
    double time = 0;
    bool started = false;
    double speed = 1.0;
    bool speed_ok = true;  // false when the music isn't on the time-stretch bus
    int paused = 0;  // 1 = paused by the user, 2 = held at the chart end
    bool scale_windows = true;
    bool fixed_scroll = false;
    bool no_camera = false;
};

struct Snapshot {
    Status s;
    bool in_song = false;
    std::string song;
};
Snapshot snapshot();
void post(const std::string& cmd);

void init(const std::wstring& dir);
void start_thread();

void poll_once();
void write_status_once(bool force = false);

void seed(bool training, bool repeat, double speed, bool scale_windows = true, bool fixed_scroll = false,
          bool no_camera = false);
void set_speed_config(const TrainerConfig& cfg);
void publish(const Status& s);
void set_song(const std::string& name);
// Commands older than 1.5 s were sent outside a chart and are dropped, except settings, which still apply.
std::optional<std::string> pop_command(uint64_t now_ms);

}  // namespace tr::remote
