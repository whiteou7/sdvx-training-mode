#pragma once

#include <windows.h>

#include <string>

namespace tr::hud {

struct Config {
    int toggle_vk = 0xC0;  // backtick
};

void start(const Config& cfg);

void set_panel_open(bool open);
bool panel_open();
HWND panel_window();
void test_force_front(bool on);

}  // namespace tr::hud
