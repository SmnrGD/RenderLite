#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LoadingLayer.hpp>

#include <cctype>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <Windows.h>
#include <TlHelp32.h>
#endif

using namespace geode::prelude;

#ifdef GEODE_IS_WINDOWS

// ---------- Helpers ----------

struct RLPowerThrottlingState {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
};

using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
using TimePeriodFn = UINT(WINAPI*)(UINT);

// Runs a command hidden and returns everything it printed
static std::string runCommand(std::string const& cmd) {
    std::string output;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return output;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<char> buf(cmd.begin(), cmd.end());
    buf.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr, buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi
    );
    CloseHandle(writePipe);

    if (ok) {
        char tmp[512];
        DWORD got = 0;
        while (ReadFile(readPipe, tmp, sizeof(tmp), &got, nullptr) && got > 0) {
            output.append(tmp, got);
        }
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(readPipe);
    return output;
}

// Finds the first GUID (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx) in a text
static std::string findGuid(std::string const& text) {
    for (size_t i = 0; i + 36 <= text.size(); i++) {
        if (text[i + 8] != '-' || text[i + 13] != '-' ||
            text[i + 18] != '-' || text[i + 23] != '-') continue;
        bool good = true;
        for (size_t k = 0; k < 36; k++) {
            if (k == 8 || k == 13 || k == 18 || k == 23) continue;
            if (!std::isxdigit(static_cast<unsigned char>(text[i + k]))) {
                good = false;
                break;
            }
        }
        if (good) return text.substr(i, 36);
    }
    return "";
}

// ---------- Restore on exit ----------
// A tiny hidden PowerShell process waits for the game to close (even after
// a crash) and then runs restore.ps1, which puts your settings back.

static std::string s_originalPlan;
static bool s_planChanged = false;
static bool s_toastChanged = false;
static DWORD s_originalToast = 1;
static std::vector<DWORD> s_loweredPids;
static bool s_watcherStarted = false;

static void startWatcher(std::filesystem::path const& script) {
    std::string cmd =
        "powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden "
        "-Command \"Wait-Process -Id " + std::to_string(GetCurrentProcessId()) +
        " -ErrorAction SilentlyContinue; "
        "Invoke-Expression (Get-Content -Raw -LiteralPath '" + script.string() + "')\"";

    DWORD flagList[2] = {
        CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
        CREATE_NO_WINDOW
    };

    for (DWORD flags : flagList) {
        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        std::vector<char> buf(cmd.begin(), cmd.end());
        buf.push_back('\0');

        if (CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                           flags, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            s_watcherStarted = true;
            return;
        }
    }
}

static void updateRestoreScript() {
    std::string script;

    if (s_planChanged && !s_originalPlan.empty()) {
        script += "powercfg /setactive " + s_originalPlan + "\n";
    }
    if (s_toastChanged) {
        script +=
            "Set-ItemProperty -Path 'HKCU:\\Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications' "
            "-Name ToastEnabled -Value " + std::to_string(s_originalToast) + " -Type DWord\n";
    }
    if (!s_loweredPids.empty()) {
        script += "foreach ($p in @(";
        for (size_t i = 0; i < s_loweredPids.size(); i++) {
            if (i > 0) script += ",";
            script += std::to_string(s_loweredPids[i]);
        }
        script += ")) { try { (Get-Process -Id $p -ErrorAction Stop).PriorityClass = 'Normal' } catch {} }\n";
    }

    auto path = Mod::get()->getSaveDir() / "restore.ps1";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << script;
    }

    if (!s_watcherStarted && !script.empty()) {
        startWatcher(path);
    }
}

// ---------- CPU priority, throttling, timer ----------

static void applyPriority(std::string const& level) {
    DWORD cls = NORMAL_PRIORITY_CLASS;
    if (level == "High") cls = HIGH_PRIORITY_CLASS;
    else if (level == "Above normal") cls = ABOVE_NORMAL_PRIORITY_CLASS;
    SetPriorityClass(GetCurrentProcess(), cls);
}

static void applyThrottling(std::string const& mode) {
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

    bool both = false;
    if (mode == "CPU and timer throttling off") {
        state.ControlMask = 0x1 | 0x4; // execution speed + timer resolution
        both = true;
    } else if (mode == "CPU throttling off") {
        state.ControlMask = 0x1;
    } else {
        state.ControlMask = 0x0; // let Windows decide
    }

    // 4 = ProcessPowerThrottling
    BOOL ok = fn(GetCurrentProcess(), 4, &state, sizeof(state));
    if (!ok && both) {
        // Older Windows does not know the timer flag, so try without it
        state.ControlMask = 0x1;
        fn(GetCurrentProcess(), 4, &state, sizeof(state));
    }
}

static int s_timerPeriod = 0;

static void applyTimer(std::string const& choice) {
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
}

// ---------- Power plan ----------

static void applyPowerPlan(std::string const& choice) {
    static const char* kBalanced = "381b4222-f694-41f0-9685-ff5bb260df2e";
    static const char* kHigh = "8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c";
    static const char* kUltimateSource = "e9a42b02-d5df-448d-aa00-03f14749eb61";

    if (choice == "No change") {
        if (s_planChanged && !s_originalPlan.empty()) {
            runCommand("powercfg /setactive " + s_originalPlan);
            s_planChanged = false;
            updateRestoreScript();
        }
        return;
    }

    if (s_originalPlan.empty()) {
        s_originalPlan = findGuid(runCommand("powercfg /getactivescheme"));
    }
    if (s_originalPlan.empty()) {
        log::warn("RenderLite: could not read the current power plan");
        return;
    }

    std::string guid;
    if (choice == "Balanced") {
        guid = kBalanced;
    } else if (choice == "High performance") {
        guid = kHigh;
    } else if (choice == "Ultimate performance") {
        auto mod = Mod::get();
        std::string list = runCommand("powercfg /list");
        guid = mod->getSavedValue<std::string>("ultimate-guid", "");
        if (guid.empty() || list.find(guid) == std::string::npos) {
            guid = findGuid(runCommand(std::string("powercfg -duplicatescheme ") + kUltimateSource));
            if (!guid.empty()) {
                mod->setSavedValue<std::string>("ultimate-guid", guid);
            }
        }
        // Some PCs do not have Ultimate performance, so use High performance
        if (guid.empty()) guid = kHigh;
    }

    if (guid.empty()) return;
    runCommand("powercfg /setactive " + guid);
    s_planChanged = true;
    updateRestoreScript();
}

// ---------- Background apps ----------

static bool isProtectedProcess(wchar_t const* name) {
    static wchar_t const* skip[] = {
        L"explorer.exe", L"dwm.exe", L"csrss.exe", L"winlogon.exe",
        L"services.exe", L"lsass.exe", L"svchost.exe", L"ctfmon.exe",
        L"sihost.exe", L"fontdrvhost.exe", L"audiodg.exe", L"taskmgr.exe",
        L"powershell.exe", L"cmd.exe", L"conhost.exe", L"obs64.exe",
        L"obs32.exe", L"discord.exe", L"steam.exe", L"steamwebhelper.exe",
        L"geometrydash.exe"
    };
    for (auto s : skip) {
        if (_wcsicmp(name, s) == 0) return true;
    }
    return false;
}

static void restoreBackgroundNow() {
    for (DWORD pid : s_loweredPids) {
        HANDLE h = OpenProcess(PROCESS_SET_INFORMATION, FALSE, pid);
        if (h) {
            SetPriorityClass(h, NORMAL_PRIORITY_CLASS);
            CloseHandle(h);
        }
    }
    s_loweredPids.clear();
}

static void applyBackground(std::string const& mode, bool reset) {
    if (reset) restoreBackgroundNow();

    DWORD target = 0;
    if (mode == "Medium") target = BELOW_NORMAL_PRIORITY_CLASS;
    else if (mode == "Strongest") target = IDLE_PRIORITY_CLASS;

    if (target != 0) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            DWORD self = GetCurrentProcessId();

            if (Process32FirstW(snap, &entry)) {
                do {
                    DWORD pid = entry.th32ProcessID;
                    if (pid <= 4 || pid == self) continue;
                    if (isProtectedProcess(entry.szExeFile)) continue;

                    HANDLE h = OpenProcess(
                        PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, pid
                    );
                    if (!h) continue;

                    // Only touch programs that are at normal priority
                    if (GetPriorityClass(h) == NORMAL_PRIORITY_CLASS) {
                        if (SetPriorityClass(h, target)) {
                            s_loweredPids.push_back(pid);
                        }
                    }
                    CloseHandle(h);
                } while (Process32NextW(snap, &entry));
            }
            CloseHandle(snap);
        }
    }

    updateRestoreScript();
}

// ---------- Focus mode ----------

static DWORD readToast() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    HKEY key;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications",
                      0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExA(key, "ToastEnabled", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
    }
    return value;
}

static void writeToast(DWORD value) {
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExA(key, "ToastEnabled", 0, REG_DWORD,
                       reinterpret_cast<BYTE const*>(&value), sizeof(value));
        RegCloseKey(key);
    }
}

static void applyFocusMode(bool on) {
    if (on) {
        if (!s_toastChanged) {
            s_originalToast = readToast();
            s_toastChanged = true;
        }
        writeToast(0);
    } else if (s_toastChanged) {
        writeToast(s_originalToast);
        s_toastChanged = false;
    }
    updateRestoreScript();
}

// ---------- GPU preference ----------

static void applyGpuPreference(std::string const& choice) {
    if (choice == "No change") return;

    char exe[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, exe, MAX_PATH)) return;

    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER,
                        "Software\\Microsoft\\DirectX\\UserGpuPreferences",
                        0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }

    if (choice == "High performance GPU") {
        char const* value = "GpuPreference=2;";
        RegSetValueExA(key, exe, 0, REG_SZ,
                       reinterpret_cast<BYTE const*>(value),
                       static_cast<DWORD>(std::strlen(value) + 1));
    } else if (choice == "Windows default") {
        RegDeleteValueA(key, exe);
    }
    RegCloseKey(key);
}

#endif // GEODE_IS_WINDOWS

// ---------- Anti-crash ----------

static bool s_antiCrash = true;
static int s_faults = 0;

static void reportFault(int count) {
    log::error("RenderLite anti-crash: caught an error in PlayLayer::update (in a row: {})", count);
}

class $modify(SafePlayLayer, PlayLayer) {
    void update(float dt) {
#ifdef GEODE_IS_WINDOWS
        // If it keeps failing 120 frames in a row, stop hiding it
        if (s_antiCrash && s_faults < 120) {
            bool faulted = false;
            __try {
                PlayLayer::update(dt);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                faulted = true;
            }
            if (faulted) {
                s_faults++;
                reportFault(s_faults);
            } else {
                s_faults = 0;
            }
            return;
        }
#endif
        PlayLayer::update(dt);
        s_faults = 0;
    }
};

// ---------- Fast loading ----------

static double s_originalInterval = 0.0;

class $modify(FastLoadingLayer, LoadingLayer) {
    bool init(bool fromReload) {
        if (Mod::get()->getSettingValue<bool>("fast-loading")) {
            s_originalInterval = CCDirector::sharedDirector()->getAnimationInterval();
            CCApplication::sharedApplication()->setAnimationInterval(1.0 / 1000.0);
        }
        return LoadingLayer::init(fromReload);
    }
};

class $modify(RLMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        // Loading is done, so put the normal frame rate back
        if (s_originalInterval > 0.0) {
            CCApplication::sharedApplication()->setAnimationInterval(s_originalInterval);
            s_originalInterval = 0.0;
        }

#ifdef GEODE_IS_WINDOWS
        // Catch programs that were started after the game
        applyBackground(Mod::get()->getSettingValue<std::string>("background-priority"), false);
#endif
        return true;
    }
};

// ---------- Setup ----------

$on_mod(Loaded) {
    auto mod = Mod::get();

    s_antiCrash = mod->getSettingValue<bool>("anti-crash");
    listenForSettingChanges<bool>("anti-crash", [](bool value) {
        s_antiCrash = value;
    });

#ifdef GEODE_IS_WINDOWS
    applyPriority(mod->getSettingValue<std::string>("priority-level"));
    applyThrottling(mod->getSettingValue<std::string>("throttling-mode"));
    applyTimer(mod->getSettingValue<std::string>("timer-resolution"));
    applyPowerPlan(mod->getSettingValue<std::string>("power-plan"));
    applyBackground(mod->getSettingValue<std::string>("background-priority"), true);
    applyFocusMode(mod->getSettingValue<bool>("focus-mode"));
    applyGpuPreference(mod->getSettingValue<std::string>("gpu-preference"));

    listenForSettingChanges<std::string>("priority-level", [](std::string value) {
        applyPriority(value);
    });
