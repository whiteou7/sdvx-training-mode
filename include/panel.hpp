#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace tr::panel {

struct Model {
    bool in_song = false;
    bool training = false;
    bool repeat = true;
    bool started = false;
    bool bgm = false;
    bool speed_ok = true;
    int start = 0, end = 0;
    int measure = 0, measures = 0;
    double speed = 1.0;
    int paused = 0;
    bool scale_windows = true;
    bool fixed_scroll = false;
    bool no_camera = false;
    int hover = -1;
    int down = -1;
    bool operator==(const Model&) const = default;
};

enum class Action { Toggle, Repeat, Close, SetStart, SetEnd, Clear, Prev, JumpStart, Next, SpeedDown, SpeedUp, Pause, ScaleWindows, FixedScroll, NoCamera };

struct Button {
    RECT r;
    Action act;
    std::wstring label;
    bool enabled;
    bool on;
};

struct Layout {
    int w = 0, h = 0;
    double scale = 1.0;
    std::vector<Button> buttons;
    RECT header1{}, header2{}, speed_box{};
};

Layout build(const Model& m, int width_px);
int hit(const Layout& l, int x, int y);
const char* command(Action a);
std::wstring loop_text(const Model& m);
std::wstring line1(const Model& m);
std::wstring line2(const Model& m);
bool seek_ready(const Model& m);

void draw(HDC dc, const Layout& l, const Model& m);

}  // namespace tr::panel
