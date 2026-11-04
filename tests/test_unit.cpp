#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "trainer.hpp"
#include "vox.hpp"

#include "check.hpp"

using namespace tr;

static std::wstring widen(const std::string& s) {
    std::wstring w;
    for (unsigned char c : s) w.push_back(wchar_t(c));
    return w;
}

static void test_vox(const char* expected_path) {
    std::ifstream in(expected_path);
    CHECK(in.good());
    std::string line;
    int charts = 0, bad = 0;
    double worst = 0;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string path, end_s, times_s;
        std::getline(ls, path, '\t');
        std::getline(ls, end_s, '\t');
        std::getline(ls, times_s);
        std::vector<double> want;
        std::istringstream ts(times_s);
        for (std::string t; std::getline(ts, t, ',');) want.push_back(std::stod(t));

        VoxTimeline tl;
        std::string err;
        bool ok = tl.load(widen(path), &err);
        charts++;
        bool chart_ok = ok && tl.end_measure == std::stoi(end_s) && tl.count() == int(want.size());
        if (chart_ok) {
            for (size_t i = 0; i < want.size(); i++) {
                double d = std::fabs(tl.measure_start[i] - want[i]);
                worst = std::max(worst, d);
                if (d > 1e-9 * std::max(1.0, std::fabs(want[i]))) chart_ok = false;
            }
        }
        if (!chart_ok) {
            bad++;
            std::printf("vox mismatch: %s (ok=%d err=%s end=%d/%s count=%d/%zu)\n", path.c_str(), ok, err.c_str(),
                        tl.end_measure, end_s.c_str(), tl.count(), want.size());
        }
    }
    std::printf("vox: %d charts, %d mismatched, worst abs diff %.3g s\n", charts, bad, worst);
    CHECK(charts > 100);
    CHECK(bad == 0);
}

static VoxTimeline grid120(int end_measure_1based) {
    std::string vox =
        "#BEAT INFO\n001,01,00\t4\t4\n#END\n#BPM INFO\n001,01,00\t120.0000\t4\n#END\n#END POSITION\n" +
        std::to_string(1000 + end_measure_1based).substr(1) + ",01,00\n#END\n";
    VoxTimeline tl;
    std::string err;
    bool ok = tl.parse(vox, &err);
    CHECK(ok);
    return tl;
}

static void test_grid_basics() {
    VoxTimeline tl = grid120(20);
    CHECK(tl.end_measure == 19);
    CHECK(tl.count() == 21);
    CHECK(std::fabs(tl.start_of(5) - 10.0) < 1e-12);
    CHECK(tl.measure_at(0.0) == 0);
    CHECK(tl.measure_at(9.999) == 4);
    CHECK(tl.measure_at(10.0) == 5);
    CHECK(tl.measure_at(-3.0) == 0);
    CHECK(tl.measure_at(1e6) == tl.count() - 1);

    VoxTimeline odd;
    CHECK(odd.parse("#BEAT INFO\n001,01,00\t7\t8\n003,01,00\t4\t4\n#END\n#BPM INFO\n001,01,00\t120.0000\t4\n"
                    "#END\n#END POSITION\n005,01,00\n#END\n"));
    CHECK(std::fabs(odd.start_of(1) - 1.75) < 1e-12);
    CHECK(std::fabs(odd.start_of(2) - 3.5) < 1e-12);
    CHECK(std::fabs(odd.start_of(3) - 5.5) < 1e-12);

    VoxTimeline bpm;
    CHECK(bpm.parse("#BEAT INFO\n001,01,00\t4\t4\n#END\n#BPM INFO\n001,01,00\t120.0000\t4\n001,03,00\t240.0000\t4\n"
                    "#END\n#END POSITION\n003,01,00\n#END\n"));
    CHECK(std::fabs(bpm.start_of(1) - 1.5) < 1e-12);
    CHECK(std::fabs(bpm.start_of(2) - 2.5) < 1e-12);
}

static Keys key(bool Keys::*k) {
    Keys out;
    out.*k = true;
    return out;
}

static void test_trainer() {
    VoxTimeline tl = grid120(20);
    Trainer t(TrainerConfig{1, 0.35});
    t.reset_session(&tl);
    std::string msg;

    CHECK(t.start() == 0 && t.end() == tl.count() - 1);

    CHECK(!t.update(11.0, key(&Keys::jump_start), &msg));
    CHECK(!t.enabled());
    CHECK(!t.update(11.0, key(&Keys::toggle), &msg));
    CHECK(t.enabled());
    CHECK(msg.find("training enabled") != std::string::npos);

    CHECK(!t.update(10.5, key(&Keys::set_start), &msg));
    CHECK(t.start() == 5);
    CHECK(!t.update(14.2, key(&Keys::set_end), &msg));
    CHECK(t.end() == 8);

    CHECK(!t.update(15.99, {}, &msg));
    auto s = t.update(16.0, {}, &msg);
    CHECK(s && std::fabs(*s - 8.0) < 1e-12);

    s = t.update(12.3, key(&Keys::jump_start), &msg);
    CHECK(s && std::fabs(*s - 8.0) < 1e-12);

    s = t.update(11.0, key(&Keys::prev), &msg);
    CHECK(s && std::fabs(*s - 10.0) < 1e-12);
    s = t.update(10.1, key(&Keys::prev), &msg);
    CHECK(s && std::fabs(*s - 8.0) < 1e-12);
    s = t.update(0.1, key(&Keys::prev), &msg);
    CHECK(s && std::fabs(*s - 0.0) < 1e-12);

    s = t.update(11.0, key(&Keys::next), &msg);
    CHECK(s && std::fabs(*s - 12.0) < 1e-12);
    s = t.update(1e6, key(&Keys::next), &msg);
    CHECK(s && std::fabs(*s - tl.start_of(tl.count() - 2)) < 1e-12);

    CHECK(!t.update(11.0, key(&Keys::repeat), &msg));
    CHECK(!t.repeat());
    CHECK(!t.update(16.5, {}, &msg));
    CHECK(!t.update(11.0, key(&Keys::repeat), &msg));
    CHECK(t.repeat());

    CHECK(!t.update(6.5, key(&Keys::set_end), &msg));
    CHECK(t.end() == 4 && t.start() == 3);
    CHECK(!t.update(9.0, key(&Keys::set_start), &msg));
    CHECK(t.start() == 4 && t.end() == 5);

    CHECK(!t.update(9.0, key(&Keys::clear), &msg));
    CHECK(t.start() == 0 && t.end() == tl.count() - 1);
    CHECK(msg.find("whole chart") != std::string::npos);
    CHECK(!t.update(5.0, {}, &msg));
    s = t.update(1e6, {}, &msg);
    CHECK(s && *s == 0.0);

    CHECK(!t.update(0.5, key(&Keys::set_start), &msg));
    CHECK(!t.update(0.5, key(&Keys::set_end), &msg));
    s = t.update(2.0, {}, &msg);
    CHECK(s && *s == 0.0);

    t.reset_session(&tl);
    CHECK(t.start() == 0 && t.end() == tl.count() - 1 && t.enabled() && t.repeat());

    t.reset_session(nullptr);
    CHECK(t.start() == -1 && t.end() == -1);
    CHECK(!t.update(5.0, key(&Keys::jump_start), &msg));
    CHECK(!t.update(5.0, key(&Keys::toggle), &msg));
    CHECK(!t.enabled());
}

static void test_speed_setting() {
    VoxTimeline tl = grid120(10);
    Trainer t;
    t.reset_session(&tl);
    std::string msg;
    CHECK(t.speed_setting() == 1.0 && t.speed() == 1.0);
    CHECK(!t.update(1.0, key(&Keys::speed_down), &msg));
    CHECK(std::fabs(t.speed_setting() - 0.95) < 1e-12 && t.speed() == 1.0);
    t.update(1.0, key(&Keys::toggle), &msg);
    CHECK(std::fabs(t.speed() - 0.95) < 1e-12);
    for (int i = 0; i < 30; i++) t.update(1.0, key(&Keys::speed_down), &msg);
    CHECK(t.speed() == 0.5);
    for (int i = 0; i < 9; i++) t.update(1.0, key(&Keys::speed_up), &msg);
    CHECK(std::fabs(t.speed() - 0.95) < 1e-12);
    for (int i = 0; i < 30; i++) t.update(1.0, key(&Keys::speed_up), &msg);
    CHECK(t.speed() == 2.0);
    t.reset_session(&tl);
    CHECK(t.speed() == 2.0);
    CHECK(msg.find("speed=2.00") != std::string::npos);
}

static void test_scale_windows_setting() {
    VoxTimeline tl = grid120(10);
    Trainer t;
    t.reset_session(&tl);
    std::string msg;
    CHECK(t.scale_windows() && !t.enabled());
    CHECK(!t.update(1.0, key(&Keys::scale_windows), &msg));
    CHECK(!t.scale_windows() && msg.find("fixed") != std::string::npos);
    CHECK(t.status(1.0).find("judge_scale=off") != std::string::npos);
    t.update(1.0, key(&Keys::toggle), &msg);
    t.reset_session(&tl);
    CHECK(!t.scale_windows());
    t.update(1.0, key(&Keys::scale_windows), &msg);
    CHECK(t.scale_windows() && msg.find("scale with speed") != std::string::npos);
    t.set_scale_windows(false);
    CHECK(!t.scale_windows());
}

static void test_scroll_camera_and_to_start() {
    VoxTimeline tl = grid120(10);
    Trainer t;
    t.reset_session(&tl);
    std::string msg;
    CHECK(!t.fixed_scroll() && !t.no_camera());
    t.update(1.0, key(&Keys::fixed_scroll), &msg);
    t.update(1.0, key(&Keys::no_camera), &msg);
    CHECK(t.fixed_scroll() && t.no_camera() && t.status(1.0).find("fixed_scroll=on no_camera=on") != std::string::npos);
    t.update(1.0, key(&Keys::toggle), &msg);
    CHECK(t.update(5.0, key(&Keys::jump_start), &msg) && t.to_start());
    CHECK(t.update(5.0, key(&Keys::next), &msg) && !t.to_start());
    CHECK(t.update(1e6, {}, &msg) && t.to_start());
    CHECK(!t.update(5.0, {}, &msg) && !t.to_start());
}

int main(int argc, char** argv) {
    test_grid_basics();
    test_trainer();
    test_speed_setting();
    test_scale_windows_setting();
    test_scroll_camera_and_to_start();
    if (argc > 1) test_vox(argv[1]);
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
