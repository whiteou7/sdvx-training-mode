#include "vox.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <tuple>

namespace tr {

namespace {

struct Pos {
    int m, b, t;
};

bool parse_pos(const std::string& s, Pos& p) {
    int m, b, t;
    if (std::sscanf(s.c_str(), "%d,%d,%d", &m, &b, &t) != 3) return false;
    p = {m - 1, b - 1, t};
    return true;
}

std::vector<std::string> split_ws(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream is(line);
    std::string tok;
    while (is >> tok) out.push_back(tok);
    return out;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "//" lines are skipped, "#END" closes a section, and any other "#..." line opens one.
std::map<std::string, std::vector<std::string>> read_sections(const std::string& text) {
    std::map<std::string, std::vector<std::string>> out;
    std::string cur;
    bool open = false;
    std::istringstream is(text);
    std::string raw;
    while (std::getline(is, raw)) {
        std::string ls = trim(raw);
        if (ls.rfind("//", 0) == 0) continue;
        std::string token = trim(ls.substr(0, ls.find("//")));
        if (token == "#END") {
            open = false;
            continue;
        }
        if (!ls.empty() && ls[0] == '#') {
            cur = ls;
            out[cur];
            open = true;
            continue;
        }
        if (open && !ls.empty()) out[cur].push_back(ls);
    }
    return out;
}

}  // namespace

bool VoxTimeline::parse(const std::string& text, std::string* err) {
    auto sec = read_sections(text);
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };

    res = 48;
    if (auto it = sec.find("#BEAT RESOLUTION"); it != sec.end() && !it->second.empty()) {
        res = std::atoi(it->second[0].c_str());
        if (res <= 0) return fail("bad BEAT RESOLUTION");
    }

    std::vector<std::tuple<int, int, int>> beats;
    for (auto& l : sec["#BEAT INFO"]) {
        auto f = split_ws(l);
        Pos p;
        if (f.size() < 3 || !parse_pos(f[0], p)) continue;
        beats.emplace_back(p.m, std::atoi(f[1].c_str()), std::atoi(f[2].c_str()));
    }
    if (beats.empty()) beats.emplace_back(0, 4, 4);
    std::sort(beats.begin(), beats.end());

    struct Bpm {
        Pos p;
        double bpm;
    };
    std::vector<Bpm> bpms;
    for (auto& l : sec["#BPM INFO"]) {
        auto f = split_ws(l);
        Pos p;
        if (f.size() < 2 || !parse_pos(f[0], p)) continue;
        bpms.push_back({p, std::atof(f[1].c_str())});
    }
    if (bpms.empty()) bpms.push_back({{0, 0, 0}, 120.0});
    std::sort(bpms.begin(), bpms.end(), [](const Bpm& a, const Bpm& b) {
        return std::tie(a.p.m, a.p.b, a.p.t, a.bpm) < std::tie(b.p.m, b.p.b, b.p.t, b.bpm);
    });

    end_measure = 0;
    if (auto it = sec.find("#END POSITION"); it != sec.end() && !it->second.empty()) {
        Pos p;
        if (parse_pos(split_ws(it->second[0])[0], p)) end_measure = std::max(0, p.m);
    }

    const int n_meas = std::max(end_measure + 2, 2);
    const int n_tab = std::max(n_meas, 4000);
    std::vector<long long> mtick(n_tab + 1);
    std::vector<int> mcpb(n_tab + 1);
    {
        long long acc = 0;
        size_t mi = 0;
        for (int meas = 0; meas <= n_tab; meas++) {
            while (mi + 1 < beats.size() && std::get<0>(beats[mi + 1]) <= meas) mi++;
            mtick[meas] = acc;
            const int num = std::get<1>(beats[mi]), den = std::get<2>(beats[mi]);
            const int cpb = int(std::lround((4.0 / den) * res));
            mcpb[meas] = cpb;
            acc += (long long)num * cpb;
        }
    }
    auto abs_tick = [&](const Pos& p) -> long long {
        int m = std::clamp(p.m, 0, n_tab);
        return mtick[m] + (long long)p.b * mcpb[m] + p.t;
    };

    std::vector<std::pair<long long, double>> pts;
    for (auto& b : bpms) pts.emplace_back(abs_tick(b.p), b.bpm);
    std::stable_sort(pts.begin(), pts.end(), [](auto& a, auto& b) { return a < b; });
    std::vector<double> psec(pts.size(), 0.0);
    for (size_t i = 1; i < pts.size(); i++) {
        const double dt = double(pts[i].first - pts[i - 1].first);
        psec[i] = psec[i - 1] + dt / res * (60.0 / pts[i - 1].second);
    }
    auto seconds = [&](long long tick) {
        size_t i = 0;
        for (size_t k = 0; k < pts.size(); k++) {
            if (pts[k].first <= tick) i = k;
            else break;
        }
        return psec[i] + double(tick - pts[i].first) / res * (60.0 / pts[i].second);
    };

    measure_start.clear();
    for (int m = 0; m < n_meas; m++) measure_start.push_back(seconds(mtick[m]));
    for (size_t i = 1; i < measure_start.size(); i++)
        if (!(measure_start[i] > measure_start[i - 1])) return fail("non-increasing measure times");
    return true;
}

bool VoxTimeline::load(const std::wstring& path, std::string* err) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        if (err) *err = "cannot open vox file";
        return false;
    }
    std::string text;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return parse(text, err);
}

int VoxTimeline::measure_at(double t) const {
    if (measure_start.empty()) return 0;
    auto it = std::upper_bound(measure_start.begin(), measure_start.end(), t);
    if (it == measure_start.begin()) return 0;
    return int(it - measure_start.begin()) - 1;
}

double VoxTimeline::start_of(int measure) const {
    if (measure_start.empty()) return 0.0;
    measure = std::clamp(measure, 0, int(measure_start.size()) - 1);
    return measure_start[measure];
}

}  // namespace tr
