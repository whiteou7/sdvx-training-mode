#pragma once

#include <string>
#include <vector>

namespace tr {

// Measure start times in chart seconds, using the same rules as vox2ksh's shared/vox_parser.py Timeline.
// BEAT RESOLUTION is cells per quarter note, BPM is per quarter note, and BPM pauses ("4-") still advance time.
struct VoxTimeline {
    int res = 48;
    int end_measure = 0;
    std::vector<double> measure_start;

    bool parse(const std::string& text, std::string* err = nullptr);
    bool load(const std::wstring& path, std::string* err = nullptr);

    int measure_at(double t) const;
    double start_of(int measure) const;
    int count() const { return int(measure_start.size()); }
};

}  // namespace tr
