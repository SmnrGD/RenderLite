#include <Geode/Geode.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LoadingLayer.hpp>
#include <Geode/modify/CCDirector.hpp>
#include <Geode/modify/CCNode.hpp>
#include <Geode/modify/CCEGLView.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
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
using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

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

// ---------- Dark window title bar ----------

struct FindWindowData {
    DWORD pid;
    HWND result;    // a window with a title bar
    HWND fallback;  // any other game window (for example borderless)
};

static BOOL CALLBACK findGameWindow(HWND hwnd, LPARAM lParam) {
    auto data = reinterpret_cast<FindWindowData*>(lParam);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != data->pid) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) {
        data->result = hwnd;
        return FALSE;
    }
    if (!data->fallback) data->fallback = hwnd;
    return TRUE;
}

static std::string s_titleMode = "Pure black";
static HWND s_titleHwnd = nullptr;
static std::string s_titleApplied;

static void applyTitleBar() {
    // Do nothing until the setting has been changed away from Off
    if (s_titleMode == "Off" && s_titleApplied.empty()) return;

    FindWindowData data{GetCurrentProcessId(), nullptr, nullptr};
    EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&data));
    HWND target = data.result ? data.result : data.fallback;
    if (!target) return;

    // Already done for this window and this setting
    if (target == s_titleHwnd && s_titleMode == s_titleApplied) return;

    HMODULE dwm = LoadLibraryA("dwmapi.dll");
    if (!dwm) return;
    auto fn = reinterpret_cast<DwmSetWindowAttributeFn>(
        reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute"))
    );
    if (!fn) return;

    BOOL dark = (s_titleMode == "Off") ? FALSE : TRUE;
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (newer Windows 10 and 11), 19 on older builds
    if (FAILED(fn(target, 20, &dark, sizeof(dark)))) {
        fn(target, 19, &dark, sizeof(dark));
    }

    // 35 = DWMWA_CAPTION_COLOR (Windows 11). 0xFFFFFFFF = back to the default color
    COLORREF color = (s_titleMode == "Pure black") ? 0x00000000 : 0xFFFFFFFF;
    fn(target, 35, &color, sizeof(color));

    // Make Windows redraw the title bar right away
    SetWindowPos(target, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    s_titleHwnd = target;
    s_titleApplied = s_titleMode;
}

#endif // GEODE_IS_WINDOWS

// ---------- Anti-crash ----------

static bool s_antiCrash = false;
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

// ---------- Draw divide ----------
// The game logic keeps running every frame, but the picture is only drawn
// at the render FPS. On the frames in between, drawing the scene and showing
// the picture on screen are skipped.

static bool s_drawDivide = false;
static int s_renderFps = 0;   // 0 = match the monitor
static int s_logicFps = 0;    // 0 = do not change
static bool s_skipRender = false;
static double s_accum = 0.0;
static double s_targetInterval = 1.0 / 60.0;
static int s_frameCounter = 0;
static std::chrono::steady_clock::time_point s_lastFrame = std::chrono::steady_clock::now();
static std::chrono::steady_clock::time_point s_lastTitleCheck = std::chrono::steady_clock::now();

static int monitorRefreshRate() {
#ifdef GEODE_IS_WINDOWS
    DEVMODEA mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &mode) &&
        mode.dmDisplayFrequency > 1) {
        return static_cast<int>(mode.dmDisplayFrequency);
    }
#endif
    return 60;
}

static void updateTargetInterval() {
    int fps = s_renderFps > 0 ? s_renderFps : monitorRefreshRate();
    if (fps < 10) fps = 10;
    s_targetInterval = 1.0 / static_cast<double>(fps);
}

static void applyLogicFps(int fps) {
    if (fps <= 0) return; // 0 = leave the game's own setting alone
    if (fps < 30) fps = 30;
    if (fps > 480) fps = 480;
    CCApplication::sharedApplication()->setAnimationInterval(1.0 / static_cast<double>(fps));
}

// ---------- Draw divide hotkey ----------

#ifdef GEODE_IS_WINDOWS

static int s_hotkeyVk = 0;          // 0 = no hotkey
static bool s_hotkeyWasDown = false;

static int keyNameToVk(std::string const& name) {
    if (name == "None") return 0;
    if (name == "Insert") return VK_INSERT;
    if (name == "Home") return VK_HOME;
    if (name == "End") return VK_END;
    if (name == "Page Up") return VK_PRIOR;
    if (name == "Page Down") return VK_NEXT;
    if (name == "Pause") return VK_PAUSE;
    if (name == "Scroll Lock") return VK_SCROLL;
    if (name.size() >= 2 && name[0] == 'F') {
        int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 12) return VK_F1 + (n - 1);
    }
    return 0;
}

static bool gameWindowFocused() {
    HWND front = GetForegroundWindow();
    if (!front) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(front, &pid);
    return pid == GetCurrentProcessId();
}

static void toggleDrawDivide() {
    Loader::get()->queueInMainThread([] {
        auto mod = Mod::get();
        bool now = !mod->getSettingValue<bool>("draw-divide");
        mod->setSettingValue<bool>("draw-divide", now);
        Notification::create(
            now ? "Draw divide: ON" : "Draw divide: OFF",
            now ? NotificationIcon::Success : NotificationIcon::None,
            1.5f
        )->show();
    });
}

static void pollHotkey() {
    if (s_hotkeyVk == 0) return;
    bool down = (GetAsyncKeyState(s_hotkeyVk) & 0x8000) != 0;
    if (down && !s_hotkeyWasDown && gameWindowFocused()) {
        toggleDrawDivide();
    }
    s_hotkeyWasDown = down;
}

#endif // GEODE_IS_WINDOWS

class $modify(DrawDivideDirector, CCDirector) {
    void drawScene() {
#ifdef GEODE_IS_WINDOWS
        pollHotkey();

        // Every two seconds, check that the title bar color is applied
        // (the window is rebuilt when you switch between fullscreen and windowed)
        auto checkNow = std::chrono::steady_clock::now();
        if (checkNow - s_lastTitleCheck > std::chrono::seconds(2)) {
            s_lastTitleCheck = checkNow;
            applyTitleBar();
        }
#endif

        // Keep the logic FPS applied, in case the game changes it
        if (s_logicFps > 0 && (++s_frameCounter % 120) == 0) {
            applyLogicFps(s_logicFps);
        }

        if (s_drawDivide) {
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - s_lastFrame).count();
            s_lastFrame = now;
            if (dt > 0.25) dt = 0.25;
            s_accum += dt;

            // Draw when about one full render frame has passed
            if (s_accum >= s_targetInterval * 0.85) {
                s_accum -= s_targetInterval;
                if (s_accum < 0.0 || s_accum > s_targetInterval) s_accum = 0.0;
                s_skipRender = false;
            } else {
                s_skipRender = true;
            }
        } else {
            s_skipRender = false;
        }

        CCDirector::drawScene();
        s_skipRender = false;
    }
};

class $modify(DrawDivideNode, CCNode) {
    void visit() {
        if (s_skipRender) {
            auto scene = CCDirector::sharedDirector()->getRunningScene();
            if (static_cast<CCNode*>(this) == static_cast<CCNode*>(scene)) return;
        }
        CCNode::visit();
    }
};

class $modify(DrawDivideView, CCEGLView) {
    void swapBuffers() {
        if (s_skipRender) return;
        CCEGLView::swapBuffers();
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
        applyLogicFps(s_logicFps);

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

    s_drawDivide = mod->getSettingValue<bool>("draw-divide");
    s_renderFps = static_cast<int>(mod->getSettingValue<int64_t>("render-fps"));
    s_logicFps = static_cast<int>(mod->getSettingValue<int64_t>("logic-fps"));
    updateTargetInterval();

    listenForSettingChanges<bool>("draw-divide", [](bool value) {
        s_drawDivide = value;
        s_accum = 0.0;
        s_lastFrame = std::chrono::steady_clock::now();
    });
    listenForSettingChanges<int64_t>("render-fps", [](int64_t value) {
        s_renderFps = static_cast<int>(value);
        updateTargetInterval();
    });
    listenForSettingChanges<int64_t>("logic-fps", [](int64_t value) {
        s_logicFps = static_cast<int>(value);
        applyLogicFps(s_logicFps);
    });

#ifdef GEODE_IS_WINDOWS
    s_hotkeyVk = keyNameToVk(mod->getSettingValue<std::string>("draw-divide-key"));
    listenForSettingChanges<std::string>("draw-divide-key", [](std::string value) {
        s_hotkeyVk = keyNameToVk(value);
    });

    // The title bar is applied from the draw loop once the window exists
    s_titleMode = mod->getSettingValue<std::string>("title-bar");
    listenForSettingChanges<std::string>("title-bar", [](std::string value) {
        s_titleMode = value;
        applyTitleBar();
    });

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
    listenForSettingChanges<std::string>("throttling-mode", [](std::string value) {
        applyThrottling(value);
    });
    listenForSettingChanges<std::string>("timer-resolution", [](std::string value) {
        applyTimer(value);
    });
    listenForSettingChanges<std::string>("power-plan", [](std::string value) {
        applyPowerPlan(value);
    });
    listenForSettingChanges<std::string>("background-priority", [](std::string value) {
        applyBackground(value, true);
    });
    listenForSettingChanges<bool>("focus-mode", [](bool value) {
        applyFocusMode(value);
    });
    listenForSettingChanges<std::string>("gpu-preference", [](std::string value) {
        applyGpuPreference(value);
    });
#endif
}
