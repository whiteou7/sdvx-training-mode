#include <windows.h>

#include <cstdio>

#include "game.hpp"
#include "rtti.hpp"

using namespace tr;

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    HMODULE game = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!game) {
        std::printf("map game dll failed: %lu\n", GetLastError());
        return 2;
    }
    Module m;
    m.init(game);
    RttiIndex rtti;
    rtti.build(m);
    GameApi api;
    if (!resolve_game(m, rtti, api)) {
        std::printf("resolve failed: %s\n", api.error.c_str());
        return 1;
    }
    void* before = api.vft_world[slot::WorldUpdate];

    HMODULE hook = LoadLibraryW(argv[2]);
    if (!hook) {
        std::printf("load hook failed: %lu\n", GetLastError());
        return 1;
    }
    auto* hb = reinterpret_cast<uint8_t*>(hook);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(hb + reinterpret_cast<IMAGE_DOS_HEADER*>(hb)->e_lfanew);
    const size_t hsize = nt->OptionalHeader.SizeOfImage;

    for (int i = 0; i < 100; i++) {
        auto* now = static_cast<uint8_t*>(api.vft_world[slot::WorldUpdate]);
        if (now != before) {
            const bool inside = now >= hb && now < hb + hsize;
            std::printf("world update slot patched: %p -> %p (%s hook dll)\n", before, now, inside ? "inside" : "OUTSIDE");
            return inside ? 0 : 1;
        }
        Sleep(100);
    }
    std::printf("slot was not patched within 10 s\n");
    return 1;
}
