#pragma once

#include <windows.h>

#include "log.hpp"

namespace tr::core::detail {

inline bool patch_slot(void** slot_addr, void* fn, void** orig) {
    DWORD old;
    if (!VirtualProtect(slot_addr, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = InterlockedExchangePointer(slot_addr, fn);
    VirtualProtect(slot_addr, sizeof(void*), old, &old);
    return true;
}

// Publishes the original before swapping the slot since the game thread may call the hook immediately.
template <typename Fn>
bool hook_slot(void** vft, int slot_index, Fn hook, Fn& original, const char* name) {
    original = reinterpret_cast<Fn>(vft[slot_index]);
    void* replaced = nullptr;
    if (!patch_slot(&vft[slot_index], reinterpret_cast<void*>(hook), &replaced)) {
        logf("%s hook failed (%lu)", name, GetLastError());
        return false;
    }
    if (replaced != reinterpret_cast<void*>(original)) logf("warning: %s slot changed during install", name);
    logf("%s hook installed", name);
    return true;
}

}  // namespace tr::core::detail
