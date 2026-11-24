#include <Geode/Geode.hpp>
#include <string>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#endif

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS
struct RLPowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};

using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using TimePeriodFn = UINT(WINAPI*)(UINT);
#endif

static void applyPriority(std::string const& level) {
#ifdef GEODE_IS_WINDOWS
    DWORD cls = NORMAL_PRIORITY_CLASS;
    if (level == "High") cls = HIGH_PRIORITY_CLASS;
    else if (level == "Above normal") cls = ABOVE_NORMAL_PRIORITY_CLASS;
    SetPriorityClass(GetCurrentProcess(), cls);
#endif
}

static void applyThrottling(std::string const& mode) {
#ifdef GEODE_IS_WINDOWS
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if (!kernel) return;

    auto fn = reinterpret_cast<SetProcessInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "SetProcessInformation"))
    );
    // Older Windows versions do not have this function, so do nothing there
    if (!fn) return;

    RLPowerThrottlingState state{};
    state.Version = 1;
    state.StateMask = 0x0;

    if (mode == "CPU and timer throttling off") {
        state.ControlMask = 0x1 | 0x4; // execution speed + timer resolution
    } else if (mode == "CPU throttling off") {
        state.ControlMask = 0x1;       // execution speed only
    } else {
        state.ControlMask = 0x0;       // let Windows decide
    }

    // 4 = ProcessPowerThrottling
    fn(GetCurrentProcess(), 4, &state, sizeof(state));
#endif
}

static int s_timerPeriod = 0;

static void applyTimer(std::string const& choice) {
#ifdef GEODE_IS_WINDOWS
    int wanted = 0;
    if (choice == "1 ms") wanted = 1;
    else if (choice == "2 ms") wanted = 2;

    if (wanted == s_timerPeriod) return;

    HMODULE winmm = LoadLibraryA("winmm.dll");
    if (!winmm) return;

    auto begin = reinterpret_cast<TimePeriodFn>(
        reinterpret_cast<void*>(GetProcAddress(winmm, "timeBeginPeriod"))
    );
    auto end = reinterpret_cast<TimePeriodFn>(
        reinterpret_cast<void*>(GetProcAddress(winmm, "timeEndPeriod"))
    );
    if (!begin || !end) return;

    if (s_timerPeriod != 0) end(static_cast<UINT>(s_timerPeriod));
    if (wanted != 0) begin(static_cast<UINT>(wanted));
    s_timerPeriod = wanted;
#endif
}

$on_mod(Loaded) {
    auto mod = Mod::get();
    applyPriority(mod->getSettingValue<std::string>("priority-level"));
    applyThrottling(mod->getSettingValue<std::string>("throttling-mode"));
    applyTimer(mod->getSettingValue<std::string>("timer-resolution"));

    listenForSettingChanges<std::string>("priority-level", [](std::string value) {
        applyPriority(value);
    });
    listenForSettingChanges<std::string>("throttling-mode", [](std::string value) {
        applyThrottling(value);
    });
    listenForSettingChanges<std::string>("timer-resolution", [](std::string value) {
        applyTimer(value);
    });
}
