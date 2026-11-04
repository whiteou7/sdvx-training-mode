#include <windows.h>

#include <string>

#include "core.hpp"
#include "log.hpp"

namespace {

DWORD WINAPI init_thread(void*) {
    HMODULE game = nullptr;
    for (int i = 0; i < 1200 && !(game = GetModuleHandleW(L"soundvoltex.dll")); i++) Sleep(100);
    if (!game) {
        tr::logf("soundvoltex.dll not loaded after 120 s; giving up");
        return 0;
    }
    if (tr::core::init(game)) tr::core::install_hook();
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(inst, path, MAX_PATH);
        std::wstring dir(path);
        dir = dir.substr(0, dir.find_last_of(L'\\'));
        tr::log_init((dir + L"\\sdvx_training.log").c_str());
        tr::core::set_dir(dir);
        tr::core::load_config();
        tr::logf("sdvx_training loaded (build " __DATE__ " " __TIME__ ")");
        if (HANDLE h = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr)) CloseHandle(h);
    }
    return TRUE;
}
