#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "hud.hpp"
#include "log.hpp"
#include "panel.hpp"
#include "remote.hpp"

#include "check.hpp"

using namespace tr;

static int find_btn(const panel::Layout& l, panel::Action a) {
    for (size_t i = 0; i < l.buttons.size(); i++)
        if (l.buttons[i].act == a) return int(i);
    return -1;
}
static POINT centre(const panel::Layout& l, panel::Action a) {
    const RECT& r = l.buttons[size_t(find_btn(l, a))].r;
    return {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
}
static bool inside(const RECT& r, int w, int h) { return r.left >= 0 && r.top >= 0 && r.right <= w && r.bottom <= h && r.right > r.left && r.bottom > r.top; }
static bool overlap(const RECT& a, const RECT& b) { return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom; }

static void pure_tests() {
    panel::Model m;
    m.in_song = m.training = m.started = m.bgm = true;
    m.measure = 12, m.measures = 69, m.start = 5, m.end = 8, m.speed = 0.75;
    for (int width : {360, 540, 720, 1080, 1440}) {
        const panel::Layout l = panel::build(m, width);
        CHECK(l.w == width && l.h > 0);
        CHECK(l.buttons.size() == 15);
        for (size_t i = 0; i < l.buttons.size(); i++) {
            CHECK(inside(l.buttons[i].r, l.w, l.h));
            for (size_t j = i + 1; j < l.buttons.size(); j++) CHECK(!overlap(l.buttons[i].r, l.buttons[j].r));
            CHECK(!overlap(l.buttons[i].r, l.speed_box));
        }
        CHECK(inside(l.speed_box, l.w, l.h) && inside(l.header1, l.w, l.h));
        for (size_t i = 0; i < l.buttons.size(); i++) {
            const RECT& r = l.buttons[i].r;
            CHECK(panel::hit(l, (r.left + r.right) / 2, (r.top + r.bottom) / 2) == int(i));
        }
        CHECK(panel::hit(l, l.speed_box.left + 2, l.speed_box.top + 2) == -1);
        CHECK(panel::hit(l, -5, 3) == -1 && panel::hit(l, l.w + 5, 3) == -1);
    }

    panel::Model off = m;
    off.in_song = false;
    panel::Layout l = panel::build(off, 720);
    for (auto a : {panel::Action::SetStart, panel::Action::SetEnd, panel::Action::Clear, panel::Action::Prev, panel::Action::JumpStart, panel::Action::Next})
        CHECK(!l.buttons[size_t(find_btn(l, a))].enabled);
    for (auto a : {panel::Action::Toggle, panel::Action::Repeat, panel::Action::Close, panel::Action::SpeedDown, panel::Action::SpeedUp,
                   panel::Action::ScaleWindows, panel::Action::FixedScroll, panel::Action::NoCamera})
        CHECK(l.buttons[size_t(find_btn(l, a))].enabled);
    CHECK(!l.buttons[size_t(find_btn(l, panel::Action::Pause))].enabled);
    CHECK(panel::hit(l, centre(l, panel::Action::SetStart).x, centre(l, panel::Action::SetStart).y) == -1);
    panel::Model nobgm = m;
    nobgm.bgm = false;
    CHECK(!panel::seek_ready(nobgm) && panel::seek_ready(m));
    panel::Model notrain = m;
    notrain.training = false;
    CHECK(!panel::seek_ready(notrain));

    CHECK(std::string(panel::command(panel::Action::Toggle)) == "toggle");
    CHECK(std::string(panel::command(panel::Action::SpeedUp)) == "speed_up");
    CHECK(std::string(panel::command(panel::Action::SpeedDown)) == "speed_down");
    CHECK(std::string(panel::command(panel::Action::Prev)) == "prev" && std::string(panel::command(panel::Action::Next)) == "next");
    CHECK(panel::command(panel::Action::Close) == nullptr);
    CHECK(std::string(panel::command(panel::Action::Pause)) == "pause");
    CHECK(std::string(panel::command(panel::Action::ScaleWindows)) == "scale_windows");
    CHECK(std::string(panel::command(panel::Action::FixedScroll)) == "fixed_scroll");
    CHECK(std::string(panel::command(panel::Action::NoCamera)) == "no_camera");

    l = panel::build(m, 720);
    CHECK(l.buttons[0].label == L"Training: ON" && l.buttons[0].on);
    notrain.repeat = false;
    l = panel::build(notrain, 720);
    CHECK(l.buttons[0].label == L"Training: OFF" && !l.buttons[0].on);
    CHECK(l.buttons[1].label == L"Repeat: OFF" && !l.buttons[1].on);
    CHECK(panel::line1(m).find(L"Measure 12 / 69") != std::wstring::npos);
    CHECK(panel::line1(m).find(L"Start 5 → End 8 repeat") != std::wstring::npos);
    CHECK(panel::line1(m).find(L"0.75×") != std::wstring::npos);
    CHECK(panel::line2(m) == L"Ready");
    {
        panel::Model pm = m;
        l = panel::build(pm, 720);
        const panel::Button& pb = l.buttons[size_t(find_btn(l, panel::Action::Pause))];
        CHECK(pb.enabled && !pb.on && pb.label.find(L"Pause") != std::wstring::npos);
        pm.paused = 1;
        l = panel::build(pm, 720);
        const panel::Button& rb = l.buttons[size_t(find_btn(l, panel::Action::Pause))];
        CHECK(rb.enabled && rb.on && rb.label.find(L"Resume") != std::wstring::npos);
        CHECK(panel::line2(pm) == L"PAUSED");
        pm.paused = 2;
        CHECK(panel::line2(pm).find(L"end of the chart") != std::wstring::npos);
        panel::Model jm = off;
        jm.scale_windows = true;
        l = panel::build(jm, 720);
        const panel::Button& on_btn = l.buttons[size_t(find_btn(l, panel::Action::ScaleWindows))];
        CHECK(on_btn.on && on_btn.enabled && on_btn.label == L"Fixed Judge: ON");
        jm.scale_windows = false;
        l = panel::build(jm, 720);
        const panel::Button& off_btn = l.buttons[size_t(find_btn(l, panel::Action::ScaleWindows))];
        CHECK(!off_btn.on && off_btn.label == L"Fixed Judge: OFF");
        jm.fixed_scroll = jm.no_camera = true;
        l = panel::build(jm, 720);
        const panel::Button& fs = l.buttons[size_t(find_btn(l, panel::Action::FixedScroll))];
        const panel::Button& nc = l.buttons[size_t(find_btn(l, panel::Action::NoCamera))];
        CHECK(fs.on && fs.label == L"Fixed Scroll: ON" && nc.on && nc.label == L"Camera: OFF");
    }
    CHECK(panel::line2(nobgm) == L"Detecting the music…");
    panel::Model intro = m;
    intro.started = false;
    CHECK(panel::line2(intro) == L"Waiting for the chart to start…");
    CHECK(panel::line2(notrain) == L"Training is OFF");
    CHECK(panel::line2(off).find(L"Not in a chart") == 0);
    panel::Model refused = m;
    refused.speed_ok = false;
    CHECK(panel::line2(refused).find(L"Speed unavailable") == 0);
    panel::Model noloop = m;
    noloop.start = noloop.end = 0;
    CHECK(panel::line1(noloop).find(L"no loop") != std::wstring::npos);
    panel::Model only_start = noloop;
    only_start.start = 7;
    CHECK(panel::line1(only_start).find(L"Start 7 → End not set") != std::wstring::npos);
}

static bool wait_for(bool (*cond)(), int ms);
static LRESULT CALLBACK fake_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(h, msg, wp, lp); }

static HWND g_fake = nullptr;
static int g_fake_w, g_fake_h, g_fake_x, g_fake_y;
static DWORD WINAPI fake_game(void*) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = fake_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FakeGame";
    wc.hbrBackground = CreateSolidBrush(RGB(30, 60, 120));
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"SOUND VOLTEX - Main Screen", WS_POPUP | WS_VISIBLE, g_fake_x, g_fake_y,
                             g_fake_w, g_fake_h, nullptr, nullptr, wc.hInstance, nullptr);
    UpdateWindow(w);
    g_fake = w;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}

static bool wait_for(bool (*cond)(), int ms) {
    for (int i = 0; i < ms / 10; i++) {
        if (cond()) return true;
        Sleep(10);
    }
    return cond();
}

static void click(HWND w, POINT p) {
    SendMessageW(w, WM_MOUSEMOVE, 0, MAKELPARAM(p.x, p.y));
    SendMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
    SendMessageW(w, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
}

static std::string next_command() {
    auto c = remote::pop_command(GetTickCount64());
    return c ? *c : std::string();
}

static std::atomic<int> g_mode{0};
static std::atomic<bool> g_quit{false};
static void publish_once(bool bgm) {
    remote::Status st;
    st.training = true;
    st.repeat = true;
    st.start = 5, st.end = 8, st.measure = 12, st.measures = 69;
    st.bgm = bgm;
    st.started = true;
    st.speed = 0.75;
    remote::publish(st);
}
static DWORD WINAPI publisher(void*) {
    while (!g_quit) {
        if (g_mode == 1) publish_once(true);
        else if (g_mode == 2) publish_once(false);
        Sleep(100);
    }
    return 0;
}
static void set_mode(int mode) {
    g_mode = mode;
    if (mode) {
        publish_once(mode == 1);
        Sleep(120);
    }
}

static void save_screen(const char* path, int x, int y, int w, int h) {
    HDC scr = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(scr);
    HBITMAP bmp = CreateCompatibleBitmap(scr, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, scr, x, y, SRCCOPY);
    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(bi), bi.biWidth = w, bi.biHeight = -h, bi.biPlanes = 1, bi.biBitCount = 32, bi.biCompression = BI_RGB;
    std::string px(size_t(w) * size_t(h) * 4, 0);
    GetDIBits(mem, bmp, 0, UINT(h), px.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{0x4D42, DWORD(sizeof(fh) + sizeof(bi) + px.size()), 0, 0, DWORD(sizeof(fh) + sizeof(bi))};
    if (FILE* f = std::fopen(path, "wb")) {
        std::fwrite(&fh, sizeof(fh), 1, f);
        std::fwrite(&bi, sizeof(bi), 1, f);
        std::fwrite(px.data(), 1, px.size(), f);
        std::fclose(f);
        std::printf("saved %s\n", path);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
}

static bool panel_visible() { return hud::panel_window() && IsWindowVisible(hud::panel_window()); }
static bool panel_hidden() { return !panel_visible(); }
static bool panel_ready() { return panel_visible(); }

int wmain() {
    CreateDirectoryW(L"build", nullptr);
    DeleteFileW(L"build\\test_overlay.log");
    log_init(L"build\\test_overlay.log");
    pure_tests();

    CreateDirectoryW(L"build\\overlay_test", nullptr);
    remote::init(L"build\\overlay_test");
    remote::seed(true, true, 0.75);
    HANDLE pub = CreateThread(nullptr, 0, publisher, nullptr, 0, nullptr);
    set_mode(1);

    const int kW = 540, kH = 960, kX = 60, kY = 40;
    g_fake_w = kW, g_fake_h = kH, g_fake_x = kX, g_fake_y = kY;
    HANDLE fake_thread = CreateThread(nullptr, 0, fake_game, nullptr, 0, nullptr);
    CHECK(wait_for([] { return g_fake != nullptr; }, 2000));
    HWND fake = g_fake;
    CHECK(fake != nullptr);
    (void)fake_thread;

    hud::test_force_front(true);
    hud::start({0xC0});
    Sleep(200);
    CHECK(!hud::panel_open());
    CHECK(hud::panel_window() != nullptr);
    CHECK(panel_hidden());

    hud::set_panel_open(true);
    const DWORD t_open = GetTickCount();
    const bool first_ok = wait_for(panel_ready, 3000);
    std::printf("panel visible after %lu ms (%d)\n", GetTickCount() - t_open, int(first_ok));
    CHECK(first_ok);
    HWND pw = hud::panel_window();

    panel::Model m;
    m.in_song = m.training = m.started = m.bgm = true;
    m.measure = 12, m.measures = 69, m.start = 5, m.end = 8, m.speed = 0.75;
    panel::Layout l = panel::build(m, kW);
    RECT pr{};
    GetWindowRect(pw, &pr);
    CHECK(pr.left == kX && pr.top == kY && pr.right - pr.left == kW && pr.bottom - pr.top == l.h);

    HWND hudw = FindWindowW(L"SdvxTrainingHud", nullptr);
    CHECK(hudw != nullptr && !IsWindowVisible(hudw));

    if (const char* shot = std::getenv("OVERLAY_SHOT")) {
        POINT hover = centre(l, panel::Action::Next);
        SendMessageW(pw, WM_MOUSEMOVE, 0, MAKELPARAM(hover.x, hover.y));
        Sleep(150);
        save_screen(shot, kX, kY, kW, l.h + 40);
        SendMessageW(pw, WM_MOUSELEAVE, 0, 0);
    }

    while (!next_command().empty()) {}
    click(pw, centre(l, panel::Action::SetStart));
    CHECK(next_command() == "set_start");
    click(pw, centre(l, panel::Action::SetEnd));
    CHECK(next_command() == "set_end");
    click(pw, centre(l, panel::Action::Clear));
    CHECK(next_command() == "clear");
    click(pw, centre(l, panel::Action::Prev));
    CHECK(next_command() == "prev");
    click(pw, centre(l, panel::Action::JumpStart));
    CHECK(next_command() == "jump_start");
    click(pw, centre(l, panel::Action::Next));
    CHECK(next_command() == "next");
    click(pw, centre(l, panel::Action::SpeedUp));
    CHECK(next_command() == "speed_up");
    click(pw, centre(l, panel::Action::SpeedDown));
    CHECK(next_command() == "speed_down");
    click(pw, centre(l, panel::Action::Toggle));
    CHECK(next_command() == "toggle");
    click(pw, centre(l, panel::Action::Repeat));
    CHECK(next_command() == "repeat");
    click(pw, centre(l, panel::Action::Pause));
    CHECK(next_command() == "pause");
    click(pw, centre(l, panel::Action::ScaleWindows));
    CHECK(next_command() == "scale_windows");
    click(pw, centre(l, panel::Action::FixedScroll));
    CHECK(next_command() == "fixed_scroll");
    click(pw, centre(l, panel::Action::NoCamera));
    CHECK(next_command() == "no_camera");
    CHECK(next_command().empty());

    POINT start_btn = centre(l, panel::Action::SetStart), end_btn = centre(l, panel::Action::SetEnd);
    SendMessageW(pw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(start_btn.x, start_btn.y));
    SendMessageW(pw, WM_LBUTTONUP, 0, MAKELPARAM(end_btn.x, end_btn.y));
    CHECK(next_command().empty());
    click(pw, {l.speed_box.left + 3, l.speed_box.top + 3});
    CHECK(next_command().empty());
    {
        POINT p = centre(l, panel::Action::Next), q = centre(l, panel::Action::SetStart);
        SendMessageW(pw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(p.x, p.y));
        Sleep(900);
        SendMessageW(pw, WM_LBUTTONUP, 0, MAKELPARAM(p.x, p.y));
        int n = 0;
        while (next_command() == "next") n++;
        CHECK(n >= 3);
        SendMessageW(pw, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(q.x, q.y));
        Sleep(900);
        SendMessageW(pw, WM_LBUTTONUP, 0, MAKELPARAM(q.x, q.y));
        CHECK(next_command() == "set_start");
        CHECK(next_command().empty());
    }
    SendMessageW(pw, WM_MOUSEWHEEL, MAKEWPARAM(0, 120), MAKELPARAM(start_btn.x, start_btn.y));
    CHECK(next_command() == "speed_up");
    SendMessageW(pw, WM_MOUSEWHEEL, MAKEWPARAM(0, WORD(-120)), MAKELPARAM(start_btn.x, start_btn.y));
    CHECK(next_command() == "speed_down");
    CHECK(SendMessageW(pw, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE);

    set_mode(2);
    Sleep(150);
    m.bgm = false;
    l = panel::build(m, kW);
    click(pw, centre(l, panel::Action::SetStart));
    CHECK(next_command().empty());
    click(pw, centre(l, panel::Action::SpeedUp));
    CHECK(next_command() == "speed_up");

    set_mode(0);
    Sleep(700);
    click(pw, centre(panel::build(panel::Model{}, kW), panel::Action::Toggle));
    CHECK(remote::snapshot().s.training == false);
    CHECK(next_command() == "toggle");

    set_mode(1);
    m.bgm = true;
    l = panel::build(m, kW);
    click(pw, centre(l, panel::Action::Close));
    CHECK(!hud::panel_open());
    CHECK(wait_for(panel_hidden, 1000));
    CHECK(next_command().empty());
    CHECK(wait_for([] { return IsWindowVisible(FindWindowW(L"SdvxTrainingHud", nullptr)) != 0; }, 1000));
    if (const char* shot = std::getenv("OVERLAY_SHOT_HUD")) {
        Sleep(150);
        save_screen(shot, kX, kY, kW, 140);
    }

    hud::set_panel_open(true);
    CHECK(wait_for(panel_ready, 2000));
    hud::set_panel_open(false);
    CHECK(wait_for(panel_hidden, 1000));

    hud::set_panel_open(true);
    CHECK(wait_for(panel_ready, 2000));
    SetWindowPos(fake, nullptr, kX + 100, kY + 30, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(200);
    GetWindowRect(pw, &pr);
    CHECK(pr.left == kX + 100 && pr.top == kY + 30);

    hud::test_force_front(false);
    CHECK(wait_for(panel_hidden, 1000));
    hud::test_force_front(true);
    CHECK(wait_for(panel_ready, 1000));
    hud::set_panel_open(false);

    g_quit = true;
    WaitForSingleObject(pub, 1000);
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
