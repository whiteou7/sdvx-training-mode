#include "game.hpp"

#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "mem.hpp"

namespace tr {

namespace {

bool match_at(const uint8_t* p, const std::vector<int>& pat) {
    for (size_t i = 0; i < pat.size(); i++)
        if (pat[i] >= 0 && p[i] != pat[i]) return false;
    return true;
}

const uint8_t* find(const uint8_t* begin, const uint8_t* end, const std::vector<int>& pat) {
    for (const uint8_t* p = begin; p + pat.size() <= end; p++)
        if (match_at(p, pat)) return p;
    return nullptr;
}

constexpr int X = -1;

// mov rcx,[rcx+disp32]; test rcx,rcx; je short; mov rax,[rcx]; call [rax+0x38]   -- timer->Update()
const std::vector<int> kHeadTimer = {0x48, 0x8B, 0x89, X, X, X, X, 0x48, 0x85, 0xC9, 0x74, X,
                                     0x48, 0x8B, 0x01, 0xFF, 0x50, 0x38};
// mov rcx,[rdi+disp32]; test rcx,rcx; je short; mov rax,[rcx]; call [rax+0x18]   -- tumami cursor reset
const std::vector<int> kTailTumami = {0x48, 0x8B, 0x8F, X, X, X, X, 0x48, 0x85, 0xC9, 0x74, X,
                                      0x48, 0x8B, 0x01, 0xFF, 0x50, 0x18};
// mov rcx,[rdi+disp32]; test rcx,rcx; je short; call rel32   -- rebuild call
const std::vector<int> kTailCall = {0x48, 0x8B, 0x8F, X, X, X, X, 0x48, 0x85, 0xC9, 0x74, X, 0xE8, X, X, X, X};

// mov rsi,[r13+disp32]; mov rbx,[r13+disp32]   -- active voice vector end / begin in MasterRender
const std::vector<int> kActiveList = {0x49, 0x8B, 0xB5, X, X, X, X, 0x49, 0x8B, 0x9D, X, X, X, X};

// GameTimer slot 1 (is stopped): cmp byte [rcx+d8],0; sete al; ret
const std::vector<int> kTimerRunning = {0x80, 0x79, X, 0x00, 0x0F, 0x94, 0xC0, 0xC3};
// GameTimer Update: mov rax,[rbp+d8]; xorps; add rax,rbx; cvtsi2ss   -- (acc + elapsed) * 1e-9
const std::vector<int> kTimerAcc = {0x48, 0x8B, 0x45, X, 0x0F, 0x57, 0xC0, 0x48, 0x03, 0xC3, 0xF3, 0x48, 0x0F, 0x2A, 0xC0};
// NoteReferee update: mov eax,[rdi+now]; mov [rdi+now-4],eax   -- previous judge time = current
const std::vector<int> kRefereeNow = {0x8B, 0x87, X, X, X, X, 0x89, 0x87, X, X, X, X};
// TimeStretch read, pass-through: mov rcx,[rdi+src]; mov rax,[rcx]; mov r8d,r12d; mov rdx,r13; call [rax+8]
const std::vector<int> kStretchSource = {0x48, 0x8B, 0x8F, X, X, X, X, 0x48, 0x8B, 0x01, 0x45, 0x8B, 0xC4,
                                         0x49, 0x8B, 0xD5, 0xFF, 0x50, 0x08};
// TimeStretch SetRate: setb [rsp+on]; xorps; comiss xmm6,xmm0; setbe [rsp+mute]
const std::vector<int> kStretchSetRate = {0x0F, 0x92, 0x44, 0x24, X, 0x0F, 0x57, 0xC0, 0x0F, 0x2F, 0xF0, 0x0F, 0x96, 0x44, 0x24, X};

// DrawTimer GetTime: mov rax,[rcx+d8]; xorps; cvtsi2ss xmm0,[rcx+d8]   -- ns clock
const std::vector<int> kDrawClock = {0x48, 0x8B, 0x41, X, 0x0F, 0x57, 0xC0, 0xF3, 0x48, 0x0F, 0x2A, 0x41, X};
// DrawTimer slot 4: lea rax,[rcx+d8]; ret   -- resync counters
const std::vector<int> kDrawStats = {0x48, 0x8D, 0x41, X, 0xC3};
// DrawTimer slot 5, storing the shared_ptr table: ... mov [rdi+t],rax; mov rbx,[rdi+t+8]; mov [rdi+t+8],rcx
const std::vector<int> kDrawTable = {0x48, 0x8B, 0x4A, 0x08, 0x48, 0x85, 0xC9, 0x74, 0x08, 0xF0, 0xFF, 0x41, 0x08,
                                     0x48, 0x8B, 0x4A, 0x08, 0x48, 0x8B, 0x02, 0x48, 0x89, 0x47, X,
                                     0x48, 0x8B, 0x5F, X, 0x48, 0x89, 0x4F, X};

// Text pattern: hex bytes, 
// ?? - any byte
// @name:n - n unknown bytes readable by name afterwards.
struct Pat {
    std::vector<int> b;
    std::unordered_map<std::string, size_t> at;
    explicit Pat(const char* text) {
        std::istringstream in(text);
        std::string t;
        while (in >> t) {
            if (t[0] == 64) {  // the at sign
                const size_t c = t.find(58);  // the colon
                at[t.substr(1, c - 1)] = b.size();
                for (int i = 0, n = std::atoi(t.c_str() + c + 1); i < n; i++) b.push_back(-1);
            } else if (t == "??") {
                b.push_back(-1);
            } else {
                b.push_back(int(std::strtol(t.c_str(), nullptr, 16)));
            }
        }
    }
    uint32_t u32(const uint8_t* hit, const char* name) const { return tr::at<uint32_t>(hit, at.at(name)); }
    uint32_t u8(const uint8_t* hit, const char* name) const { return hit[at.at(name)]; }
    uint32_t u16(const uint8_t* hit, const char* name) const { return tr::at<uint16_t>(hit, at.at(name)); }
    const uint8_t* rel_target(const uint8_t* hit, const char* name) const {
        const size_t p = at.at(name);
        return hit + p + 4 + tr::at<int32_t>(hit, p);
    }
};

// Returns the only match in the executable sections, or nullptr if there is none or more than one.
const uint8_t* find_unique(const Module& m, const Pat& pat) {
    const uint8_t* found = nullptr;
    for (auto& s : m.sections) {
        if (!s.exec) continue;
        const uint8_t* b = m.base + s.rva;
        for (const uint8_t* p = find(b, b + s.size, pat.b); p; p = find(p + 1, b + s.size, pat.b)) {
            if (found) return nullptr;
            found = p;
        }
    }
    return found;
}

bool has_slots(const RttiIndex& rtti, const char* name, int n) {
    if (const auto* v = rtti.find(name))
        for (auto& t : *v)
            if (t.offset == 0 && t.slots > uint32_t(n)) return true;
    return false;
}

uint32_t disp32(const uint8_t* insn) { return at<uint32_t>(insn, 3); }

const uint8_t* call_target(const uint8_t* e8) { return e8 + 5 + at<int32_t>(e8, 1); }

// The game fills its keyword table at startup with lea rdx,[name]; mov dword [rip+id slot],id; then strncpy.
std::vector<int> keyword_ids(const Module& m, const std::vector<std::string>& names) {
    std::vector<std::vector<const uint8_t*>> strs(names.size());
    for (auto& s : m.sections) {
        if (s.exec) continue;
        const std::string_view sec(reinterpret_cast<const char*>(m.base + s.rva), s.size);
        for (size_t i = 0; i < names.size(); i++) {
            const std::string key = std::string(1, 0) + names[i] + std::string(1, 0);
            for (size_t p = sec.find(key); p != sec.npos; p = sec.find(key, p + 1)) strs[i].push_back(m.base + s.rva + p + 1);
        }
    }
    std::vector<int> ids(names.size(), -1);
    for (auto& s : m.sections) {
        if (!s.exec) continue;
        const uint8_t* b = m.base + s.rva;
        for (const uint8_t* p = b; p + 17 <= b + s.size; p++) {
            if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || p[7] != 0xC7 || p[8] != 0x05) continue;
            const uint8_t* target = p + 7 + at<int32_t>(p, 3);
            for (size_t i = 0; i < names.size(); i++)
                for (const uint8_t* str : strs[i])
                    if (str == target) ids[i] = ids[i] == -1 || ids[i] == at<int32_t>(p, 13) ? at<int32_t>(p, 13) : 0;
        }
    }
    for (int& id : ids) id = std::max(id, 0);
    return ids;
}

}  // namespace

// If a pattern here isn't found, its fields stay empty and only the feature that needs it is turned off.
void resolve_optional(const Module& m, const RttiIndex& rtti, GameApi& api) {
    // The game's quick retry, called from both the playing state (B) and the end state (A). It stops the timer and audio,
    // increments the retry counter, and switches to the retry state, which rebuilds the chart without saving a result.
    static const Pat kRetryPlaying(
        "48 8B 8E @timer:4 48 8B 01 FF 50 20 48 8B 8E @audio:4 E8 @stop:4 48 8D 55 ?? 48 8B 8E @ctx:4 E8 @counter:4 "
        "48 8B 08 FF 01 48 8D 4D ?? E8 @release:4 48 8D 05 @retry:4 E9 ?? ?? ?? ??");
    static const Pat kRetryEnd(
        "48 8B 8E @timer:4 48 85 C9 74 ?? 48 8B 01 FF 50 20 48 8B 8E @audio:4 48 85 C9 74 ?? E8 @stop:4 48 8D 54 24 ?? "
        "48 8B 8E @ctx:4 E8 @counter:4 48 8B 08 FF 01 48 8D 4C 24 ?? E8 @release:4 BA 02 00 00 00 48 8D 0D ?? ?? ?? ?? "
        "E8 ?? ?? ?? ?? 48 8D 05 @retry:4 48 89 44 24 ?? 44 89 74 24 ?? 0F 10 44 24 ?? 0F 11 46 @pending:1 "
        "44 89 B5 ?? ?? ?? ?? C6 85 ?? ?? ?? ?? 01 48 8B 85 ?? ?? ?? ?? 48 89 46 @subreq:1");
    static const Pat kStateCall("4C 8B 4F @state:1 4D 85 C9 74 ?? 40 38 77 ??");
    const uint8_t* pb = find_unique(m, kRetryPlaying);
    const uint8_t* pa = find_unique(m, kRetryEnd);
    const uint8_t* upd = static_cast<const uint8_t*>(api.vft_world[slot::WorldUpdate]);
    const uint8_t* sc = find(upd, upd + 0x400, kStateCall.b);
    if (pb && pa && sc) {
        const uint8_t* retry_b = kRetryPlaying.rel_target(pb, "retry");
        const uint8_t* retry_a = kRetryEnd.rel_target(pa, "retry");
        const uint8_t* stop_a = kRetryEnd.rel_target(pa, "stop");
        const uint8_t* cnt_a = kRetryEnd.rel_target(pa, "counter");
        const uint8_t* rel_a = kRetryEnd.rel_target(pa, "release");
        const bool same = retry_a == retry_b && stop_a == kRetryPlaying.rel_target(pb, "stop") &&
                          cnt_a == kRetryPlaying.rel_target(pb, "counter") && rel_a == kRetryPlaying.rel_target(pb, "release") &&
                          kRetryPlaying.u32(pb, "timer") == api.off_timer && kRetryEnd.u32(pa, "timer") == api.off_timer &&
                          kRetryPlaying.u32(pb, "audio") == api.off_audio && kRetryEnd.u32(pa, "audio") == api.off_audio &&
                          kRetryPlaying.u32(pb, "ctx") == kRetryEnd.u32(pa, "ctx");
        const uint32_t pending = kRetryEnd.u8(pa, "pending"), subreq = kRetryEnd.u8(pa, "subreq");
        const uint32_t state = kStateCall.u8(sc, "state");
        const uint8_t* play = m.function_start(pb);
        const uint8_t* end = m.function_start(pa);
        if (same && m.in_exec(retry_a) && m.in_exec(stop_a) && m.in_exec(cnt_a) && m.in_exec(rel_a) && play && end &&
            play != end && pending == 0x48 && subreq == 0x64 && state == 0x18) {
            api.state_playing = play;
            api.state_end = end;
            api.state_retry = retry_a;
            api.audio_stop = reinterpret_cast<FnVoid>(const_cast<uint8_t*>(stop_a));
            api.counter_get = reinterpret_cast<FnCounterGet>(const_cast<uint8_t*>(cnt_a));
            api.sp_release = reinterpret_cast<FnVoid>(const_cast<uint8_t*>(rel_a));
            api.off_state_fn = state;
            api.off_pending = pending;
            api.off_subreq = subreq;
            api.off_context = kRetryEnd.u32(pa, "ctx");
        }
    }

    static const Pat kFinished("0F B6 81 @fin:4 C3");
    if (const auto* fw = rtti.find(cls::World)) {
        for (auto& t : *fw) {
            if (t.offset != 0 || t.slots <= slot::WorldFinished) continue;
            const uint8_t* f = static_cast<const uint8_t*>(t.vft[slot::WorldFinished]);
            if (m.contains(f, 8) && match_at(f, kFinished.b)) api.off_finished = kFinished.u32(f, "fin");
        }
    }

    static const Pat kChord("C6 43 @held:1 01 F3 0F 58 73 @time:1 F3 0F 11 73 @time2:1");
    if (void** cv = rtti.primary(cls::ForceCrash)) {
        const uint8_t* f = static_cast<const uint8_t*>(cv[slot::CrashUpdate]);
        const uint8_t* h = m.contains(f, 0x100) ? find(f, f + 0x100, kChord.b) : nullptr;
        if (h && kChord.u8(h, "time") == kChord.u8(h, "time2") && kChord.u8(h, "held") < kChord.u8(h, "time")) {
            api.vft_crash = cv;
            api.off_crash_held = kChord.u8(h, "held");
            api.off_crash_time = kChord.u8(h, "time");
        }
    }

    static const Pat kCrashMark(
        "48 8B 56 @first:1 4C 8B 46 @count:1 4C 03 C2 49 3B D0 74 ?? 45 33 C9 0F 1F 00 48 8B 4E @mapsize:1 48 FF C9 48 23 CA "
        "48 8B 46 @map:1 48 8B 0C C8 48 8B 01 48 8B 48 08 48 39 08 74 ?? 44 89 49 F8 66 C7 41 FC 00 01 48 FF C2 49 3B D0 "
        "75 ?? C6 86 @forced:4 01");
    // later builds use ebx for the zero instead of r9d
    static const Pat kCrashMark2(
        "48 8B 56 @first:1 4C 8B 46 @count:1 4C 03 C2 49 3B D0 74 ?? 48 8B 4E @mapsize:1 48 FF C9 48 23 CA "
        "48 8B 46 @map:1 48 8B 0C C8 48 8B 01 48 8B 48 08 48 39 08 74 ?? 89 59 F8 66 C7 41 FC 00 01 48 FF C2 49 3B D0 "
        "75 ?? C6 86 @forced:4 01");
    if (void** fv = rtti.primary(cls::Fumen)) {
        const uint8_t* f = static_cast<const uint8_t*>(fv[1]);
        for (const Pat* pat : {&kCrashMark, &kCrashMark2}) {
            const uint8_t* h = m.contains(f, 0x800) ? find(f, f + 0x800, pat->b) : nullptr;
            if (!h) continue;
            api.off_players_first = pat->u8(h, "first");
            api.off_players_count = pat->u8(h, "count");
            api.off_players_mapsize = pat->u8(h, "mapsize");
            api.off_players_map = pat->u8(h, "map");
            api.off_forced_crash = pat->u32(h, "forced");
            break;
        }
    }

    static const Pat kEnd("0F 2F @m1:1 @end:2 00 00 0F 93 C0 88 @m2:1 @flag:2 00 00");
    if (api.fumen_rebuild) {
        const uint8_t* rb = reinterpret_cast<const uint8_t*>(api.fumen_rebuild);
        const uint8_t* call = nullptr;
        for (const uint8_t* p = rb; p < rb + 0x80 && m.contains(p, 8); p++)
            if (p[0] == 0xE8) {
                call = call_target(p);
                break;
            }
        const uint8_t* h = call && m.in_exec(call) && m.contains(call, 0x300) ? find(call, call + 0x300, kEnd.b) : nullptr;
        if (h && (kEnd.u8(h, "m1") & 7) == (kEnd.u8(h, "m2") & 7) && kEnd.u16(h, "flag") == kEnd.u16(h, "end") + 4u) {
            api.off_chart_end = kEnd.u16(h, "end");
            api.off_chart_over = kEnd.u16(h, "flag");
        }
    }

    if (void** iv = rtti.primary(cls::Input)) api.vft_input = iv;
    if (has_slots(rtti, cls::Judge, slot::JudgeGetTime)) api.vft_judge = rtti.primary(cls::Judge);
    // DrawTimer GetTime: mov rax,[rcx+offs]; ...; mulss [1e-9]; addss xmm0,[rax].  JudgeTimer: addss xmm6,[rax+at].
    static const Pat kDrawOffs("48 8B 41 @offs:1 0F 57 C0 F3 48 0F 2A 41 ?? F3 0F 59 05 ?? ?? ?? ?? F3 0F 58 00");
    static const Pat kJudgeOffs("48 8B 47 @offs:1 F3 0F 58 70 @at:1");
    const uint8_t* dg = api.vft_draw ? static_cast<const uint8_t*>(api.vft_draw[slot::DrawGetTime]) : nullptr;
    const uint8_t* jg = api.vft_judge ? static_cast<const uint8_t*>(api.vft_judge[slot::JudgeGetTime]) : nullptr;
    const uint8_t* jh = jg && m.contains(jg, 0x100) ? find(jg, jg + 0x100, kJudgeOffs.b) : nullptr;
    if (dg && m.contains(dg, 0x20) && match_at(dg, kDrawOffs.b) && jh && kJudgeOffs.u8(jh, "at") % 4 == 0) {
        api.off_draw_offs = kDrawOffs.u8(dg, "offs");
        api.off_judge_offs = kJudgeOffs.u8(jh, "offs");
        api.judge_offs_at = kJudgeOffs.u8(jh, "at");
    }
    static const Pat kRange("4C 8B 4F @range:1 4C 8B 47 @table:1 0F 57 C0 4D 85 C0");
    static const Pat kSpeed("48 8B 41 @speed:1");
    if (api.vft_draw && api.off_draw_table) {
        const uint8_t* tb = static_cast<const uint8_t*>(api.vft_draw[slot::DrawSetTable]);
        const uint8_t* h = m.contains(tb, 0x200) ? find(tb, tb + 0x200, kRange.b) : nullptr;
        void** rv = rtti.primary(cls::DrawRange);
        const uint8_t* rs = rv ? static_cast<const uint8_t*>(rv[1]) : nullptr;
        static const std::vector<int> kDivide = {0xF3, 0x0F, 0x5E, 0x18};  // divss xmm3,[rax]: 1 / multiplier
        if (h && rs && m.contains(rs, 0x40) && match_at(rs, kSpeed.b) && find(rs, rs + 0x30, kDivide) &&
            kRange.u8(h, "table") == api.off_draw_table) {
            api.off_draw_range = kRange.u8(h, "range");
            api.off_range_speed = kSpeed.u8(rs, "speed");
            api.vft_range = rv;
        }
    }

    // World init passes the parsed vox to MakeFumenData. The parser stores ConstantScroll with setne + mov [r15+flag].
    static const Pat kMakeFumen("49 8B 95 @vox:4 48 83 C2 @data:1 48 8D 4C 24 ?? E8 ?? ?? ?? ?? 49 8D 8D @fumen:4 48 8B D0");
    static const Pat kConstScroll("39 45 ?? 0F 95 C0 41 88 47 @flag:1");
    const uint8_t* mf = find_unique(m, kMakeFumen);
    const uint8_t* cs = find_unique(m, kConstScroll);
    const auto* init = static_cast<const uint8_t*>(api.vft_world[slot::WorldInit]);
    if (mf && cs && m.function_start(mf) == init && kMakeFumen.u32(mf, "fumen") == 0x98 && kMakeFumen.u32(mf, "vox") < 0x200) {
        api.off_world_vox = kMakeFumen.u32(mf, "vox");
        api.off_vox_data = kMakeFumen.u8(mf, "data");
        api.off_const_scroll = kConstScroll.u8(cs, "flag");
    }

    static const Pat kScrollFix("C6 86 @flag:4 01 48 8B CE E8 @rep:4 49 8B 57 08 49 8B 0F 48 3B CA 74 ?? 66 90 "
                                "F3 0F 10 49 @mult:1 F3 0F 5E 49 @bpm:1 F3 0F 59 C8 F3 0F 11 49 ?? 48 83 C1 @stride:1");
    static const Pat kScrollTable("48 8B F1 48 89 4D ?? 4C 8D 79 @table:1 4C 89 7D ?? 49 8B 0F 49 89 4F 08");
    const uint8_t* sf = api.off_world_vox ? find_unique(m, kScrollFix) : nullptr;
    const uint8_t* fs = sf ? m.function_start(sf) : nullptr;
    static const Pat kScrollStop("0F B6 00 F3 41 0F 10 01 F3 0F 11 09 F3 0F 11 41 @bpm:1 88 41 @stop:1 C7 41 @mult:1 00 00 80 3F");
    const uint8_t* st = fs && fs < sf ? find(fs, sf, kScrollTable.b) : nullptr;
    const uint8_t* ss = st ? find(fs, sf, kScrollStop.b) : nullptr;
    if (ss && kScrollStop.u8(ss, "bpm") == kScrollFix.u8(sf, "bpm") && kScrollStop.u8(ss, "mult") == kScrollFix.u8(sf, "mult") &&
        kScrollStop.u8(ss, "stop") < kScrollFix.u8(sf, "stride") &&kScrollFix.u32(sf, "flag") < 0x1000 && kScrollFix.u8(sf, "stride") >= 0x10 &&
        kScrollFix.u8(sf, "mult") + 4 <= kScrollFix.u8(sf, "stride") && kScrollFix.u8(sf, "bpm") + 4 <= kScrollFix.u8(sf, "stride")) {
        api.off_world_fdata = kMakeFumen.u32(mf, "fumen");
        api.off_fd_table = kScrollTable.u8(st, "table");
        api.off_fd_const = kScrollFix.u32(sf, "flag");
        api.fd_stride = kScrollFix.u8(sf, "stride");
        api.fd_bpm = kScrollFix.u8(sf, "bpm");
        api.fd_mult = kScrollFix.u8(sf, "mult");
        api.fd_stop = kScrollStop.u8(ss, "stop");
        api.fd_rep = reinterpret_cast<FnGetF>(const_cast<uint8_t*>(kScrollFix.rel_target(sf, "rep")));
    }

    // At the end of its update, the morph updater writes its two curve values through [this+values].
    static const Pat kMorph("48 8B 43 @values:1 F3 0F 11 00 48 8B 4B ?? 48 85 C9 74 ?? 48 8B 01 48 8D 54 24 ?? FF 50 08 "
                            "48 8B 43 @values2:1 F3 0F 11 40 04");
    if (const auto* mv = rtti.find(cls::Morph))
        for (auto& t : *mv) {
            const auto* u = t.offset == 8 && t.slots > slot::MorphUpdate ? static_cast<const uint8_t*>(t.vft[slot::MorphUpdate]) : nullptr;
            const uint8_t* h = u && m.contains(u, 0xa0) ? find(u, u + 0xa0, kMorph.b) : nullptr;
            if (h && kMorph.u8(h, "values") == kMorph.u8(h, "values2")) {
                api.vft_morph = t.vft;
                api.off_morph_values = kMorph.u8(h, "values");
            }
        }

    // EventObject slot 2 returns its time, and slot 1 calls slot 2 of its std::function target.
    // The rebuild walks the object manager's list: mov rbx,[r15+list]; lea r12,[EventObject vft].
    static const Pat kEventTime("F3 0F 10 41 @t:1 C3");
    static const Pat kEventFire("48 83 EC 28 48 8B 49 @fn:1 48 85 C9 74 ?? 48 8B 01 48 83 C4 28 48 FF 60 10");
    static const Pat kEventList("49 8B 9F @list:4 4C 8D 25 @vft:4 49 8B AF @end:4");
    if (void** ev = rtti.primary(cls::Event); ev && has_slots(rtti, cls::Event, slot::EventTime)) {
        const auto* tm = static_cast<const uint8_t*>(ev[slot::EventTime]);
        const auto* fi = static_cast<const uint8_t*>(ev[slot::EventFire]);
        const uint8_t* el = find_unique(m, kEventList);
        if (m.contains(tm, 8) && match_at(tm, kEventTime.b) && m.contains(fi, 0x20) && match_at(fi, kEventFire.b) && el &&
            kEventList.rel_target(el, "vft") == reinterpret_cast<const uint8_t*>(ev) &&
            kEventList.u32(el, "end") == kEventList.u32(el, "list") + 8 && kEventList.u32(el, "list") < 0x400) {
            api.vft_event = ev;
            api.off_events = kEventList.u32(el, "list");
            api.off_event_time = kEventTime.u8(tm, "t");
            api.off_event_fn = kEventFire.u8(fi, "fn");
        }
    }

    // The parser skips Realize (0x10) and BAROFF (0x19) and adds every other #SPCONTROLER record to the list.
    static const Pat kSpcList("83 F8 10 74 ?? 83 F8 19 74 ?? 49 8D 8F @list:4 48 8D 95 ?? ?? ?? ?? E8");
    const uint8_t* sl = find_unique(m, kSpcList);
    if (api.off_world_vox && sl && m.function_start(sl) == m.function_start(cs) && kSpcList.u32(sl, "list") < 0x400) {
        const auto ids = keyword_ids(m, {"LaneY", "Morphing0", "Morphing1", "Morphing2", "Morphing3"});
        if (std::count(ids.begin(), ids.end(), 0) == 0 && ids[2] == ids[1] + 1 && ids[3] == ids[1] + 2 && ids[4] == ids[1] + 3) {
            api.off_spc_list = kSpcList.u32(sl, "list");
            api.spc_lane_y = ids[0];
            api.spc_morph0 = ids[1];
        }
    }

    static const Pat kRotate("48 8B 02 48 8B D9 48 8D 54 24 ?? 48 89 81 ?? ?? ?? ?? F3 0F 11 91");
    static const Pat kCamDist("48 8B 49 08 0F 57 C0 0F 2E 81");
    static const Pat kCamPitch("48 8B 49 08 0F 2E 89");
    // CameraUpdater's reset event: movss xmm1,[distance]; call [rax+0x28] ... movss xmm1,[pitch]; call [rax+0x38]
    static const Pat kCamReset1("48 8B 08 48 8B 01 F3 0F 10 0D @v:4 FF 50 28");
    static const Pat kCamReset2("48 8B 08 48 8B 01 F3 0F 10 0D @v:4 FF 50 38");
    void** ev = rtti.primary(cls::Engine);
    void** gv = rtti.primary(cls::GWorld);
    const uint8_t* r1 = find_unique(m, kCamReset1);
    const uint8_t* r2 = r1 ? find(r1, r1 + 0x80, kCamReset2.b) : nullptr;
    if (ev && gv && r1 && r2) {
        const auto* rot = static_cast<const uint8_t*>(ev[slot::EngineRotate]);
        const auto* dist = static_cast<const uint8_t*>(gv[slot::CamDistance]);
        const auto* pitch = static_cast<const uint8_t*>(gv[slot::CamPitch]);
        const auto* d = reinterpret_cast<const float*>(kCamReset1.rel_target(r1, "v"));
        const auto* p = reinterpret_cast<const float*>(kCamReset2.rel_target(r2, "v"));
        if (m.contains(rot, 0x40) && find(rot, rot + 0x40, kRotate.b) && m.contains(dist, 0x10) && match_at(dist, kCamDist.b) &&
            m.contains(pitch, 0x10) && match_at(pitch, kCamPitch.b) && m.contains(d, 4) && m.contains(p, 4)) {
            api.vft_engine = ev;
            api.vft_gworld = gv;
            api.cam_distance = d;
            api.cam_pitch = p;
        }
    }
}

bool resolve_game(const Module& m, const RttiIndex& rtti, GameApi& api) {
    api = GameApi{};
    auto need = [&](const char* name, void**& out) {
        out = rtti.primary(name);
        if (!out) api.error += std::string("missing vftable ") + name + "; ";
    };
    need(cls::World, api.vft_world);
    need(cls::Timer, api.vft_timer);
    need(cls::Voice, api.vft_voice);
    need(cls::Audio, api.vft_audio);
    need(cls::Fumen, api.vft_fumen);
    need(cls::Tumami, api.vft_tumami);
    need(cls::Referee, api.vft_referee);
    if (!api.ok()) return false;

    if (!has_slots(rtti, cls::World, slot::WorldUpdate)) api.error += "GameWorldKFC vftable too short; ";
    if (!has_slots(rtti, cls::Timer, slot::TimerSeek)) api.error += "GameTimer vftable too short; ";
    if (!has_slots(rtti, cls::Voice, slot::VoiceConnCount)) api.error += "VoiceImpl vftable too short; ";
    for (auto& t : *rtti.find(cls::Voice))
        if (t.offset == 8 && t.slots > slot::VoiceUpdate) api.vft_voice_update = t.vft;
    if (!api.vft_voice_update) api.error += "VoiceImpl IVoiceUpdate vftable not found; ";
    if (const auto* fm = rtti.find(cls::VoiceMaster))
        for (auto& t : *fm)
            if (t.offset == 16 && t.slots > slot::MasterRender) api.vft_master_render = t.vft;
    if (!api.vft_master_render) api.error += "VoiceMasterImpl render vftable not found; ";
    if (!api.ok()) return false;

    const uint8_t* upd = static_cast<const uint8_t*>(api.vft_world[slot::WorldUpdate]);
    const uint8_t* end = upd + 0x400;
    if (!m.in_exec(upd) || !m.contains(upd, 0x400)) {
        api.error = "WorldUpdate not in module";
        return false;
    }

    const uint8_t* h = find(upd, upd + 0x40, kHeadTimer);
    if (!h) {
        api.error = "WorldUpdate head pattern (timer->Update) not found";
        return false;
    }
    api.off_timer = disp32(h);

    const uint8_t* t1 = find(h, end, kTailTumami);
    const uint8_t* t2 = t1 ? find(t1 + kTailTumami.size(), end, kTailTumami) : nullptr;
    const uint8_t* c1 = t2 ? find(t2 + kTailTumami.size(), end, kTailCall) : nullptr;
    const uint8_t* c2 = c1 ? find(c1 + kTailCall.size(), end, kTailCall) : nullptr;
    if (!t1 || t2 != t1 + kTailTumami.size() || c1 != t2 + kTailTumami.size() || c2 != c1 + kTailCall.size()) {
        api.error = "WorldUpdate rebuild-branch pattern not found";
        return false;
    }
    api.off_tumami_l = disp32(t1);
    api.off_tumami_r = disp32(t2);
    api.off_fumen = disp32(c1);
    api.off_tutorial = disp32(c2);
    const uint8_t* f1 = call_target(c1 + 12);
    const uint8_t* f2 = call_target(c2 + 12);
    if (!m.in_exec(f1) || !m.in_exec(f2)) {
        api.error = "rebuild call targets outside .text";
        return false;
    }
    api.fumen_rebuild = reinterpret_cast<FnVoid>(const_cast<uint8_t*>(f1));
    api.tutorial_rebuild = reinterpret_cast<FnVoid>(const_cast<uint8_t*>(f2));

    for (uint32_t off : {api.off_timer, api.off_tumami_l, api.off_tumami_r, api.off_fumen, api.off_tutorial})
        if (off == 0 || off > 0x2000 || (off & 7)) api.error = "implausible GameWorldKFC field offset";
    if (!api.ok()) return false;

    const uint8_t* mr = static_cast<const uint8_t*>(api.vft_master_render[slot::MasterRender]);
    if (!m.in_exec(mr) || !m.contains(mr, 0x400)) {
        api.error = "MasterRender not in module";
        return false;
    }
    const uint8_t* lst = find(mr, mr + 0x400, kActiveList);
    if (!lst) {
        api.error = "MasterRender active-voice list pattern not found";
        return false;
    }
    api.off_active_end = at<uint32_t>(lst, 3);
    api.off_active_begin = at<uint32_t>(lst, 10);
    bool calls_update = false;
    for (const uint8_t* p = lst; p < lst + 0x50 && !calls_update; p++)
        calls_update = p[0] == 0xE8 && call_target(p) == static_cast<const uint8_t*>(api.vft_voice_update[slot::VoiceUpdate]);
    if (!calls_update) api.error = "MasterRender loop does not call IVoiceUpdate::Update";
    if (api.off_active_end != api.off_active_begin + 8) api.error = "implausible active-voice list offsets";
    if (!api.ok()) return false;

    const uint8_t* stopped = static_cast<const uint8_t*>(api.vft_timer[1]);
    const uint8_t* tupd = static_cast<const uint8_t*>(api.vft_timer[slot::TimerUpdate]);
    if (!m.contains(stopped, kTimerRunning.size()) || !match_at(stopped, kTimerRunning) || !m.contains(tupd, 0x100)) {
        api.error = "GameTimer layout not recognised";
        return false;
    }
    api.off_timer_running = stopped[2];
    const uint8_t* acc = find(tupd, tupd + 0x100, kTimerAcc);
    if (!acc) {
        api.error = "GameTimer accumulator pattern not found";
        return false;
    }
    api.off_timer_acc = acc[3];

    const uint8_t* rupd = static_cast<const uint8_t*>(api.vft_referee[slot::RefereeUpdate]);
    const uint8_t* now = m.contains(rupd, 0x100) ? find(rupd, rupd + 0x100, kRefereeNow) : nullptr;
    if (!now || at<uint32_t>(now, 8) + 4 != at<uint32_t>(now, 2) || at<uint32_t>(now, 2) > 0x1000) {
        api.error = "NoteReferee judge-time pattern not found";
        return false;
    }
    api.off_referee_now = at<uint32_t>(now, 2);
    if (!has_slots(rtti, cls::Referee, slot::RefereeTimeout)) api.error = "NoteReferee vftable too short";
    if (!api.ok()) return false;

    void** st = rtti.primary(cls::Stretch);
    if (!st) st = rtti.primary(cls::Stretch2);
    if (st) {
        const uint8_t* rd = static_cast<const uint8_t*>(st[slot::FeederRead]);
        const uint8_t* src = m.contains(rd, 0x600) ? find(rd, rd + 0x600, kStretchSource) : nullptr;
        const uint8_t* sr = static_cast<const uint8_t*>(st[slot::StretchSetRate]);
        const bool set_rate = m.contains(sr, 0x100) && find(sr, sr + 0x100, kStretchSetRate);
        if (src && set_rate && at<uint32_t>(src, 3) < 0x10000) {
            api.vft_stretch = st;
            api.off_stretch_source = at<uint32_t>(src, 3);
        }
    }

    if (void** dv = rtti.primary(cls::Draw)) {
        const bool long_enough = has_slots(rtti, cls::Draw, slot::DrawSetTable);
        const uint8_t* gt = static_cast<const uint8_t*>(dv[slot::DrawGetTime]);
        const uint8_t* st = static_cast<const uint8_t*>(dv[slot::DrawStats]);
        const uint8_t* tb = static_cast<const uint8_t*>(dv[slot::DrawSetTable]);
        if (long_enough && m.contains(gt, 0x20) && m.contains(st, 8) && m.contains(tb, 0x80) &&
            match_at(gt, kDrawClock) && match_at(st, kDrawStats)) {
            const uint8_t* tp = find(tb, tb + 0x80, kDrawTable);
            const uint32_t clock = gt[12], stats = st[3];
            if (tp && tp[27] == tp[23] + 8 && tp[31] == tp[27] && clock >= 0x10 && clock < 0x100 && !(clock & 7) &&
                stats < 0x100 && !(stats & 3) && !(tp[23] & 7)) {
                api.vft_draw = dv;
                api.off_draw_clock = clock;
                api.off_draw_stats = stats;
                api.off_draw_table = tp[23];
                api.draw_recompute = reinterpret_cast<FnDrawSetTable>(const_cast<uint8_t*>(tb));
            }
        }
    }
    // lea r14,[r15+vox]; ...; lea rcx,["VoxFile Target:%s"].
// If this isn't found, the default offset is kept. Without the chart file there is no measure grid.
    static const Pat kVoxLog("4D 8D B7 @vox:4 49 8B D6 49 83 7E 18 10 72 03 49 8B 16 48 8D 0D @fmt:4");
    if (const uint8_t* h = find_unique(m, kVoxLog)) {
        const uint8_t* fmt = kVoxLog.rel_target(h, "fmt");
        static const char kFmt[] = "VoxFile Target";
        const uint32_t vox = kVoxLog.u32(h, "vox");
        if (m.contains(fmt, sizeof(kFmt)) && std::memcmp(fmt, kFmt, sizeof(kFmt) - 1) == 0 && vox < 0x2000 && !(vox & 7))
            api.off_vox_path = vox;
    }
    resolve_optional(m, rtti, api);
    return true;
}

}  // namespace tr
