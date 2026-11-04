#include "hud.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "log.hpp"
#include "panel.hpp"
#include "remote.hpp"

namespace tr::hud {

namespace {

std::atomic<bool> g_started{false};
Config g_cfg;
HWND g_wnd = nullptr;
std::atomic<HWND> g_panel_wnd{nullptr};
std::atomic<bool> g_panel_open{false};
std::atomic<bool> g_force_front{false};

// spice2x names the main window "... - Main Screen"
HWND find_game_window() {
    struct Ctx {
        HWND best;
        long area;
        DWORD pid;
    } c{nullptr, 0, GetCurrentProcessId()};
    EnumWindows(
        [](HWND h, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid != c->pid || h == g_wnd || h == g_panel_wnd.load() || !IsWindowVisible(h) || IsIconic(h)) return TRUE;
            wchar_t title[256] = {};
            DWORD_PTR got = 0;
            SendMessageTimeoutW(h, WM_GETTEXT, 256, reinterpret_cast<LPARAM>(title), SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &got);
            if (wcsstr(title, L"Main Screen")) {
                c->best = h;
                return FALSE;
            }
            RECT r;
            GetClientRect(h, &r);
            const long area = long(r.right) * long(r.bottom);
            if (area > c->area) c->area = area, c->best = h;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&c));
    return c.best;
}

// EnumWindows is slow and this runs every few ms, so the result is cached for a second.
HWND game_window() {
    static HWND cached = nullptr;
    static uint64_t checked = 0;
    const uint64_t now = GetTickCount64();
    if (cached && (!IsWindow(cached) || !IsWindowVisible(cached) || IsIconic(cached))) cached = nullptr;
    if (!cached && now - checked < 250) return nullptr;
    if (!cached || now - checked > 1000) {
        cached = find_game_window();
        checked = now;
    }
    return cached;
}

bool game_in_front() {
    if (g_force_front) return true;
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

bool client_on_screen(HWND game, RECT& client) {
    if (!game || !GetClientRect(game, &client)) return false;
    MapWindowPoints(game, nullptr, reinterpret_cast<POINT*>(&client), 2);
    return client.right - client.left >= 100;
}

struct Canvas {
    HDC dc = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old = nullptr;
    void* bits = nullptr;
    int w = 0, h = 0;

    bool open(int width, int height) {
        w = width, h = height;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!dib || !bits) {
            close();
            return false;
        }
        old = SelectObject(dc, dib);
        std::memset(bits, 0, size_t(w) * h * 4);
        return true;
    }

    void commit(HWND wnd, int x, int y, uint32_t base_alpha) {
        GdiFlush();
        auto* px = static_cast<uint32_t*>(bits);
        for (int i = 0; i < w * h; i++) {
            const uint32_t c = px[i] & 0xFFFFFF;
            const uint32_t m = std::max({c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF});
            px[i] = ((base_alpha + m * (255 - base_alpha) / 255) << 24) | c;
        }
        POINT dst{x, y};
        SIZE sz{w, h};
        POINT src{0, 0};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        UpdateLayeredWindow(wnd, nullptr, &dst, &sz, dc, &src, 0, &bf, ULW_ALPHA);
    }

    void close() {
        if (old) SelectObject(dc, old);
        if (dib) DeleteObject(dib);
        if (dc) DeleteDC(dc);
        dc = nullptr, dib = nullptr, old = nullptr, bits = nullptr;
    }
};

struct View {
    bool show = false;
    std::wstring line1;
    std::wstring line2;
};

panel::Model make_model();

View hud_view() {
    const panel::Model m = make_model();
    View v;
    v.show = m.in_song && m.training;
    if (!v.show) return v;
    wchar_t buf[256];
    swprintf(buf, 256, L"TRAINING   %d / %d   ·   %ls   ·   %.2f×%ls", m.measure, m.measures, panel::loop_text(m).c_str(), m.speed,
             m.scale_windows && m.speed != 1.0 ? L"  (judge ×)" : L"");
    v.line1 = buf;
    v.line2 = panel::line2(m);
    return v;
}

void render_hud(const View& v, const RECT& client) {
    const int cw = client.right - client.left;
    const double scale = cw / 1080.0;  // the game's layout is 1080 units wide
    const int w = cw, h = std::max(40, int(std::lround(104 * scale)));
    const int pad = std::max(4, int(std::lround(14 * scale)));
    Canvas cv;
    if (!cv.open(w, h)) return;

    HFONT big = CreateFontW(-int(std::lround(36 * scale)), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                            ANTIALIASED_QUALITY, 0, L"Segoe UI");
    HFONT small = CreateFontW(-int(std::lround(26 * scale)), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                              ANTIALIASED_QUALITY, 0, L"Segoe UI");
    SetBkMode(cv.dc, TRANSPARENT);
    RECT r1{pad, pad / 2, w - pad, h / 2 + pad / 4};
    RECT r2{pad, h / 2 + pad / 4, w - pad, h - pad / 3};
    HGDIOBJ old_font = SelectObject(cv.dc, big);
    SetTextColor(cv.dc, RGB(120, 255, 210));
    DrawTextW(cv.dc, v.line1.c_str(), -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(cv.dc, small);
    SetTextColor(cv.dc, RGB(225, 225, 225));
    DrawTextW(cv.dc, v.line2.c_str(), -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(cv.dc, old_font);
    DeleteObject(big);
    DeleteObject(small);
    cv.commit(g_wnd, client.left, client.top, 0xA8);
    cv.close();
}

void tick_hud(bool panel_shown) {
    const View v = hud_view();
    static View drawn;
    static RECT drawn_at{};
    static bool shown = false;
    HWND game = (!panel_shown && v.show && game_in_front()) ? game_window() : nullptr;
    RECT client{};
    if (client_on_screen(game, client)) {
        if (!shown || v.line1 != drawn.line1 || v.line2 != drawn.line2 || !EqualRect(&client, &drawn_at)) {
            render_hud(v, client);
            drawn = v;
            drawn_at = client;
        }
        if (!shown) {
            SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            shown = true;
        }
    } else if (shown) {
        ShowWindow(g_wnd, SW_HIDE);
        shown = false;
    }
}

struct PanelUi {
    bool shown = false;
    panel::Model model;
    panel::Layout layout;
    RECT at{};
    int hover = -1, down = -1;
    bool tracking = false;
    bool repeated = false;
} g_ui;

constexpr UINT_PTR kHoldTimer = 2;
constexpr UINT kHoldDelayMs = 400, kHoldRepeatMs = 90;

panel::Model make_model() {
    const remote::Snapshot sn = remote::snapshot();
    panel::Model m;
    m.in_song = sn.in_song;
    m.training = sn.s.training;
    m.repeat = sn.s.repeat;
    m.started = sn.s.started;
    m.bgm = sn.s.bgm;
    m.speed_ok = sn.s.speed_ok;
    m.start = sn.s.start;
    m.end = sn.s.end;
    m.measure = sn.s.measure;
    m.measures = sn.s.measures;
    m.speed = sn.s.speed;
    m.paused = sn.s.paused;
    m.scale_windows = sn.s.scale_windows;
    m.fixed_scroll = sn.s.fixed_scroll;
    m.no_camera = sn.s.no_camera;
    m.hover = g_ui.hover;
    m.down = g_ui.down;
    return m;
}

void render_panel(const RECT& client) {
    const panel::Model m = make_model();
    const int cw = client.right - client.left;
    panel::Layout l = panel::build(m, cw);
    Canvas cv;
    if (!cv.open(l.w, l.h)) return;
    panel::draw(cv.dc, l, m);
    cv.commit(g_panel_wnd, client.left, client.top, 0xB4);
    cv.close();
    g_ui.model = m;
    g_ui.layout = std::move(l);
    g_ui.at = client;
}

bool tick_panel() {
    HWND game = (g_panel_open && game_in_front()) ? game_window() : nullptr;
    RECT client{};
    if (client_on_screen(game, client)) {
        if (!g_ui.shown || make_model() != g_ui.model || !EqualRect(&client, &g_ui.at)) render_panel(client);
        if (!g_ui.shown) {
            SetWindowPos(g_panel_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
            g_ui.shown = true;
        }
        return true;
    }
    if (g_ui.shown) {
        ShowWindow(g_panel_wnd, SW_HIDE);
        g_ui.shown = false;
        g_ui.hover = g_ui.down = -1;
    }
    return false;
}

void redraw_panel() {
    if (g_ui.shown) render_panel(g_ui.at);
}

void fire(int idx) {
    if (idx < 0 || idx >= int(g_ui.layout.buttons.size())) return;
    const panel::Action act = g_ui.layout.buttons[size_t(idx)].act;
    if (const char* cmd = panel::command(act)) remote::post(cmd);
    else g_panel_open = false;
}

LRESULT CALLBACK panel_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    const int x = int(short(LOWORD(lp))), y = int(short(HIWORD(lp)));
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(g_ui.hover >= 0 ? 32649 : 32512)));
        return TRUE;
    case WM_MOUSEMOVE: {
        if (!g_ui.tracking) {
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            g_ui.tracking = TrackMouseEvent(&t) != 0;
        }
        const int idx = panel::hit(g_ui.layout, x, y);
        if (idx != g_ui.hover) {
            g_ui.hover = idx;
            redraw_panel();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_ui.tracking = false;
        if (g_ui.hover != -1 && g_ui.down == -1) {
            g_ui.hover = -1;
            redraw_panel();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        g_ui.hover = panel::hit(g_ui.layout, x, y);
        g_ui.down = g_ui.hover;
        g_ui.repeated = false;
        SetCapture(h);
        SetTimer(h, kHoldTimer, kHoldDelayMs, nullptr);
        redraw_panel();
        return 0;
    }
    case WM_TIMER:
        if (wp == kHoldTimer) {
            const int d = g_ui.down;
            const bool step = d >= 0 && d == g_ui.hover && d < int(g_ui.layout.buttons.size()) &&
                              (g_ui.layout.buttons[size_t(d)].act == panel::Action::Prev ||
                               g_ui.layout.buttons[size_t(d)].act == panel::Action::Next);
            if (!step) return 0;
            fire(d);
            g_ui.repeated = true;
            SetTimer(h, kHoldTimer, kHoldRepeatMs, nullptr);
            return 0;
        }
        break;
    case WM_LBUTTONUP: {
        KillTimer(h, kHoldTimer);
        const int idx = panel::hit(g_ui.layout, x, y);
        const int was_down = g_ui.down;
        g_ui.down = -1;
        g_ui.hover = idx;
        if (GetCapture() == h) ReleaseCapture();
        if (idx >= 0 && idx == was_down && !g_ui.repeated) fire(idx);
        redraw_panel();
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (short(HIWORD(wp)) != 0) remote::post(short(HIWORD(wp)) > 0 ? "speed_up" : "speed_down");
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void poll_toggle_key() {
    static bool prev = false;
    if (!g_cfg.toggle_vk) return;
    const bool down = (GetAsyncKeyState(g_cfg.toggle_vk) & 0x8000) != 0;
    if (down && !prev && game_in_front()) g_panel_open = !g_panel_open;
    prev = down;
}

void tick() {
    poll_toggle_key();
    const bool panel_shown = tick_panel();
    tick_hud(panel_shown);
}

DWORD WINAPI thread_main(void*) {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"SdvxTrainingHud";
    RegisterClassExW(&wc);
    WNDCLASSEXW pc{};
    pc.cbSize = sizeof(pc);
    pc.lpfnWndProc = panel_proc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    pc.lpszClassName = L"SdvxTrainingPanel";
    RegisterClassExW(&pc);
    g_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            wc.lpszClassName, L"SDVX Training HUD", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    HWND panel_wnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                     pc.lpszClassName, L"SDVX Training Panel", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                                     inst, nullptr);
    if (!g_wnd || !panel_wnd) {
        logf("hud: window creation failed (%lu)", GetLastError());
        return 0;
    }
    g_panel_wnd = panel_wnd;
    SetTimer(g_wnd, 1, 25, nullptr); 
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.hwnd == g_wnd) tick();
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace

void start(const Config& cfg) {
    if (g_started) return;
    g_cfg = cfg;
    g_started = true;
    if (HANDLE h = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr)) CloseHandle(h);
}

void set_panel_open(bool open) { g_panel_open = open; }
bool panel_open() { return g_panel_open; }
HWND panel_window() { return g_panel_wnd; }
void test_force_front(bool on) { g_force_front = on; }

}  // namespace tr::hud
