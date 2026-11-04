#include "panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tr::panel {

namespace {

constexpr double kDesignW = 1080.0;
constexpr int kMargin = 24, kGap = 14, kButtonH = 60, kHeader1H = 46, kHeader2H = 34;
constexpr int kCols = 4, kRows = 4;

int px(double v, double scale) { return int(std::lround(v * scale)); }

COLORREF fill_color(const Button& b, bool hover, bool down) {
    if (!b.enabled) return RGB(30, 32, 38);
    if (down) return RGB(26, 130, 108);
    if (b.on) return hover ? RGB(30, 124, 102) : RGB(22, 98, 82);
    return hover ? RGB(72, 86, 114) : RGB(46, 52, 66);
}

}  // namespace

bool seek_ready(const Model& m) { return m.in_song && m.training && m.bgm; }

const char* command(Action a) {
    switch (a) {
    case Action::Toggle: return "toggle";
    case Action::Repeat: return "repeat";
    case Action::SetStart: return "set_start";
    case Action::SetEnd: return "set_end";
    case Action::Clear: return "clear";
    case Action::Prev: return "prev";
    case Action::JumpStart: return "jump_start";
    case Action::Next: return "next";
    case Action::SpeedDown: return "speed_down";
    case Action::SpeedUp: return "speed_up";
    case Action::Pause: return "pause";
    case Action::ScaleWindows: return "scale_windows";
    case Action::FixedScroll: return "fixed_scroll";
    case Action::NoCamera: return "no_camera";
    case Action::Close: return nullptr;
    }
    return nullptr;
}

std::wstring loop_text(const Model& m) {
    wchar_t loop[64] = L"no loop";
    if (m.start && m.end) swprintf(loop, 64, L"Start %d → End %d%ls", m.start, m.end, m.repeat ? L" repeat" : L"");
    else if (m.start) swprintf(loop, 64, L"Start %d → End not set", m.start);
    return loop;
}

std::wstring line1(const Model& m) {
    wchar_t buf[256];
    if (!m.in_song) {
        swprintf(buf, 256, L"Training %ls   ·   %.2f×", m.training ? L"ON" : L"OFF", m.speed);
        return buf;
    }
    swprintf(buf, 256, L"Measure %d / %d   ·   %ls   ·   %.2f×", m.measure, m.measures, loop_text(m).c_str(), m.speed);
    return buf;
}

std::wstring line2(const Model& m) {
    if (!m.in_song) return m.training ? L"Not in a chart. Training starts with the next chart." : L"Not in a chart. Training is off.";
    if (!m.training) return L"Training is OFF";
    if (!m.started) return L"Waiting for the chart to start…";
    if (!m.bgm) return L"Detecting the music…";
    if (!m.speed_ok && m.speed != 1.0) return L"Speed unavailable: the music is not on the time-stretch bus";
    if (m.paused == 2) return L"PAUSED at the end of the chart: seek back, then resume, or leave training";
    if (m.paused) return L"PAUSED";
    return L"Ready";
}

Layout build(const Model& m, int width_px) {
    Layout l;
    l.scale = std::max(0.2, width_px / kDesignW);
    const double s = l.scale;
    const int margin = px(kMargin, s), gap = px(kGap, s), bh = px(kButtonH, s);
    l.w = std::max(1, width_px);
    const int inner = l.w - 2 * margin;
    const int bw = (inner - (kCols - 1) * gap) / kCols;

    int y = margin / 2;
    l.header1 = {margin, y, l.w - margin, y + px(kHeader1H, s)};
    y = l.header1.bottom;
    l.header2 = {margin, y, l.w - margin, y + px(kHeader2H, s)};
    y = l.header2.bottom + gap;

    const bool ready = seek_ready(m);
    struct Spec {
        Action act;
        const wchar_t* label;
        bool enabled, on;
    };
    const std::wstring train = m.training ? L"Training: ON" : L"Training: OFF";
    const std::wstring rep = m.repeat ? L"Repeat: ON" : L"Repeat: OFF";
    const std::wstring pause = m.paused ? L"▶  Resume" : L"❚❚  Pause";
    const std::wstring judge = m.scale_windows ? L"Fixed Judge: ON" : L"Fixed Judge: OFF";
    const std::wstring scroll = m.fixed_scroll ? L"Fixed Scroll: ON" : L"Fixed Scroll: OFF";
    const std::wstring camera = m.no_camera ? L"Camera: OFF" : L"Camera: ON";
    const Spec specs[kRows][kCols] = {
        {{Action::Toggle, train.c_str(), true, m.training}, {Action::Repeat, rep.c_str(), true, m.repeat},
         {Action::Pause, pause.c_str(), ready, m.paused != 0}, {Action::Close, L"✕  Close", true, false}},
        {{Action::SetStart, L"Set Start", ready, false}, {Action::SetEnd, L"Set End", ready, false},
         {Action::Clear, L"Clear Start/End", ready, false}, {Action::JumpStart, L"Jump to Start", ready, false}},
        {{Action::Prev, L"◀  Measure", ready, false}, {Action::Next, L"Measure  ▶", ready, false},
         {Action::FixedScroll, scroll.c_str(), true, m.fixed_scroll}, {Action::NoCamera, camera.c_str(), true, m.no_camera}},
        {{Action::SpeedDown, L"Speed −", true, false}, {Action::Close, L"", false, false}, {Action::SpeedUp, L"Speed +", true, false},
         {Action::ScaleWindows, judge.c_str(), true, m.scale_windows}},
    };
    for (int row = 0; row < kRows; row++) {
        for (int col = 0; col < kCols; col++) {
            const Spec& sp = specs[row][col];
            const int x = margin + col * (bw + gap);
            const RECT r{x, y, x + bw, y + bh};
            if (row == 3 && col == 1) {
                l.speed_box = r;
                continue;
            }
            l.buttons.push_back({r, sp.act, sp.label, sp.enabled, sp.on});
        }
        y += bh + gap;
    }
    l.h = y - gap + margin;
    return l;
}

int hit(const Layout& l, int x, int y) {
    for (size_t i = 0; i < l.buttons.size(); i++) {
        const Button& b = l.buttons[i];
        if (b.enabled && x >= b.r.left && x < b.r.right && y >= b.r.top && y < b.r.bottom) return int(i);
    }
    return -1;
}

void draw(HDC dc, const Layout& l, const Model& m) {
    const double s = l.scale;
    auto font = [&](double size, int weight) {
        return CreateFontW(-px(size, s), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    };
    HFONT big = font(36, FW_BOLD), mid = font(26, FW_SEMIBOLD), small = font(25, FW_NORMAL), huge = font(40, FW_BOLD);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, big);
    const UINT kText = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;

    RECT all{0, 0, l.w, l.h};
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(dc, &all, black);
    DeleteObject(black);

    SetTextColor(dc, RGB(120, 255, 210));
    RECT r = l.header1;
    DrawTextW(dc, line1(m).c_str(), -1, &r, DT_LEFT | kText);
    SelectObject(dc, small);
    SetTextColor(dc, RGB(225, 225, 225));
    r = l.header2;
    DrawTextW(dc, line2(m).c_str(), -1, &r, DT_LEFT | kText);

    SelectObject(dc, mid);
    for (size_t i = 0; i < l.buttons.size(); i++) {
        const Button& b = l.buttons[i];
        const bool hover = int(i) == m.hover, down = int(i) == m.down && hover;
        HBRUSH fill = CreateSolidBrush(fill_color(b, hover, down));
        HBRUSH edge = CreateSolidBrush(b.enabled ? (b.on ? RGB(70, 210, 170) : RGB(96, 108, 134)) : RGB(52, 55, 62));
        FillRect(dc, &b.r, fill);
        FrameRect(dc, &b.r, edge);
        DeleteObject(fill);
        DeleteObject(edge);
        SetTextColor(dc, b.enabled ? RGB(240, 242, 246) : RGB(108, 112, 120));
        RECT t = b.r;
        DrawTextW(dc, b.label.c_str(), -1, &t, DT_CENTER | kText);
    }

    wchar_t sp[32];
    swprintf(sp, 32, L"%.2f×", m.speed);
    SelectObject(dc, huge);
    SetTextColor(dc, m.speed == 1.0 ? RGB(225, 225, 225) : RGB(120, 255, 210));
    r = l.speed_box;
    DrawTextW(dc, sp, -1, &r, DT_CENTER | kText);

    SelectObject(dc, old_font);
    DeleteObject(big);
    DeleteObject(mid);
    DeleteObject(small);
    DeleteObject(huge);
}

}  // namespace tr::panel
