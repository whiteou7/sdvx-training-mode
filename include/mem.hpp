#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace tr {

// Use this before reading any pointer that didn't come directly from the game's call arguments.
inline bool readable(const void* p, size_t n) {
    auto cur = reinterpret_cast<uintptr_t>(p);
    const uintptr_t end = cur + n;
    if (cur < 0x10000 || end < cur || end > 0x00007FFFFFFFFFFFull) return false;
    while (cur < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<void*>(cur), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD prot = mbi.Protect & 0xff;
        if ((mbi.Protect & PAGE_GUARD) || prot == PAGE_NOACCESS) return false;
        if (!(prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return false;
        cur = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

template <typename T>
inline bool read(const void* p, T& out) {
    if (!readable(p, sizeof(T))) return false;
    std::memcpy(&out, p, sizeof(T));
    return true;
}

template <typename T>
inline T at(const void* base, size_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(base) + off, sizeof(T));
    return v;
}

}  // namespace tr
