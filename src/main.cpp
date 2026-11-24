#include <Geode/Geode.hpp>

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

static void applyPriority(bool high) {
#ifdef GEODE_IS_WINDOWS
    SetPriorityClass(
        GetCurrentProcess(),
        high ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS
    );
#endif
}

static void applyThrottling(bool disable) {
#ifdef GEODE_IS_WINDOWS
    HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if (!kernel) return;

    auto fn = reinterpret_cast<SetProcessInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel, "SetProcessInformation"))
    );
    if (!fn) return;

    RLPowerThrottlingState state{};
    state.Version = 1;
    state.ControlMask = disable ? 0x1 : 0x0;
    state.StateMask = 0x0;

    // 4 = ProcessPowerThrottling
    fn(GetCurrentProcess(), 4, &state, sizeof(state));
#endif
}

static bool s_timerActive = false;

static void applyTimer(bool precise) {
#ifdef GEODE_IS_WINDOWS
    HMODULE winmm = LoadLibraryA("winmm.dll");
    if (!winmm) return;

    auto begin = reinterpret_cast<TimePeriodFn>(
        reinterpret_cast<void*>(GetProcAddress(winmm, "timeBeginPeriod"))
    );
    auto end = reinterpret_cast<TimePeriodFn>(
        reinterpret_cast<void*>(GetProcAddress(winmm, "timeEndPeriod"))
    );
    if (!begin || !end) return;

    if (precise && !s_timerActive) {
        begin(1);
        s_timerActive = true;
    } else if (!precise && s_timerActive) {
        end(1);
        s_timerActive = false;
    }
#endif
}

$on_mod(Loaded) {
    auto mod = Mod::get();
    applyPriority(mod->getSettingValue<bool>("high-priority"));
    applyThrottling(mod->getSettingValue<bool>("disable-throttling"));
    applyTimer(mod->getSettingValue<bool>("precise-timer"));

    listenForSettingChanges<bool>("high-priority", [](bool value) {
        applyPriority(value);
    });
    listenForSettingChanges<bool>("disable-throttling", [](bool value) {
        applyThrottling(value);
    });
    listenForSettingChanges<bool>("precise-timer", [](bool value) {
        applyTimer(value);
    });
}
