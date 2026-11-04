#include "rtti.hpp"

#include <windows.h>

#include <cstring>
#include <unordered_set>

#include "mem.hpp"

namespace tr {

bool Module::init(void* hmodule) {
    base = static_cast<uint8_t*>(hmodule);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    image_size = nt->OptionalHeader.SizeOfImage;
    const auto& exc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    exc_rva = exc.VirtualAddress;
    exc_size = exc.Size;
    auto sec = IMAGE_FIRST_SECTION(nt);
    sections.clear();
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        sections.push_back({sec[i].VirtualAddress, sec[i].Misc.VirtualSize,
                            (sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0});
    }
    return true;
}

bool Module::contains(const void* p, size_t n) const {
    // Integer math so a pointer near the top of the address space cannot wrap past the check.
    const uintptr_t q = reinterpret_cast<uintptr_t>(p), b = reinterpret_cast<uintptr_t>(base), e = b + image_size;
    return q >= b && q <= e && n <= e - q;
}

bool Module::in_exec(const void* p) const {
    if (!contains(p)) return false;
    const uint32_t r = rva(p);
    for (auto& s : sections)
        if (s.exec && r >= s.rva && r < s.rva + s.size) return true;
    return false;
}

const uint8_t* Module::function_start(const void* p) const {
    if (!exc_rva || !exc_size || !contains(p) || !contains(base + exc_rva, exc_size)) return nullptr;
    struct Rf {
        uint32_t begin, end, unwind;
    };
    const auto* tab = reinterpret_cast<const Rf*>(base + exc_rva);
    const size_t n = exc_size / sizeof(Rf);
    const uint32_t target = rva(p);
    size_t lo = 0, hi = n;  // sorted by BeginAddress
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (tab[mid].begin <= target) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return nullptr;
    Rf f = tab[lo - 1];
    if (target >= f.end) return nullptr;
    for (int depth = 0; depth < 16; depth++) {  // UNW_FLAG_CHAININFO: a cold part points back at its function
        const uint32_t ui = f.unwind & ~1u;
        if (!contains(base + ui, 4)) return nullptr;
        const uint8_t* info = base + ui;
        if (!(info[0] >> 3 & 4)) return base + f.begin;
        const uint32_t chain = ui + 4 + ((info[2] + 1u) & ~1u) * 2;
        if (!contains(base + chain, sizeof(Rf))) return nullptr;
        std::memcpy(&f, base + chain, sizeof(Rf));
    }
    return nullptr;
}

// x64 RTTICompleteObjectLocator: signature, offset, cdOffset, pTypeDescriptor, pClassDescriptor, pSelf (RVAs)
static bool is_col(const Module& m, uint32_t rva, uint32_t* td_rva, uint32_t* offset) {
    if (rva + 24 > m.image_size) return false;
    const uint8_t* p = m.base + rva;
    if (at<uint32_t>(p, 0) != 1 || at<uint32_t>(p, 20) != rva) return false;
    const uint32_t td = at<uint32_t>(p, 12);
    if (td + 20 > m.image_size) return false;
    const char* name = reinterpret_cast<const char*>(m.base + td + 16);
    if (std::strncmp(name, ".?A", 3) != 0) return false;
    if (td_rva) *td_rva = td;
    if (offset) *offset = at<uint32_t>(p, 4);
    return true;
}

void RttiIndex::build(const Module& m) {
    m_ = &m;
    by_name_.clear();

    std::unordered_map<uint64_t, std::pair<std::string, uint32_t>> cols;  // COL address -> (name, offset). A vftable is a COL pointer followed by code pointers.
    for (auto& s : m.sections) {
        if (s.exec || s.size < 24) continue;
        for (uint32_t rva = s.rva; rva + 24 <= s.rva + s.size; rva += 4) {
            uint32_t td, off;
            if (!is_col(m, rva, &td, &off)) continue;
            const char* name = reinterpret_cast<const char*>(m.base + td + 16);
            size_t len = strnlen(name, 4096);
            cols.emplace(reinterpret_cast<uint64_t>(m.base + rva), std::make_pair(std::string(name, len), off));
        }
    }

    for (auto& s : m.sections) {
        if (s.exec || s.size < 16) continue;
        for (uint32_t rva = s.rva; rva + 16 <= s.rva + s.size; rva += 8) {
            const uint64_t v = at<uint64_t>(m.base, rva);
            auto it = cols.find(v);
            if (it == cols.end()) continue;
            void** vft = reinterpret_cast<void**>(m.base + rva + 8);
            uint32_t n = 0;
            while (rva + 8 + 8 * (n + 1) <= s.rva + s.size && m.in_exec(vft[n])) n++;
            if (!n) continue;
            by_name_[it->second.first].push_back({vft, it->second.second, n});
        }
    }
}

const std::vector<VfTable>* RttiIndex::find(const std::string& mangled) const {
    auto it = by_name_.find(mangled);
    return it == by_name_.end() ? nullptr : &it->second;
}

void** RttiIndex::primary(const std::string& mangled) const {
    auto v = find(mangled);
    if (!v) return nullptr;
    for (auto& t : *v)
        if (t.offset == 0) return t.vft;
    return nullptr;
}

std::string RttiIndex::class_of(const void* obj, uint32_t* subobject_offset) const {
    if (!m_) return {};
    void** vptr;
    if (!read(obj, vptr) || !vptr) return {};
    if (!m_->contains(vptr, 8) || !m_->contains(reinterpret_cast<uint8_t*>(vptr) - 8, 8)) return {};
    const uint8_t* col = reinterpret_cast<const uint8_t*>(vptr[-1]);
    if (!m_->contains(col, 24)) return {};
    uint32_t td, off;
    if (!is_col(*m_, m_->rva(col), &td, &off)) return {};
    if (subobject_offset) *subobject_offset = off;
    const char* name = reinterpret_cast<const char*>(m_->base + td + 16);
    return std::string(name, strnlen(name, 4096));
}

}  // namespace tr
