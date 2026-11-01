// ScrollSpeed v5 - Crimson Desert ASI plugin
// Mouse wheel (Ctrl+wheel) sets a "stick limit": the left stick values the game reads from the
// controller are scaled down before the game sees them, exactly like pushing the stick less far.
// The game itself then turns that smaller tilt into a slower walk speed.
//   - Hooks XInputGetState (the same entry point the CrimsonDesertAnalogMovement mod hooks).
//   - Does not touch game memory or game speed data. Requires a connected XInput controller.
//   - Cannot go FASTER than a fully pushed stick (that is the game's maximum run).
// Controller alternative: hold Back/View and press D-Pad Up/Down (those presses are hidden from the game).
// Ctrl + middle mouse click = reset to full range.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

// Minimal XInput types (identical layout to xinput.h)
struct XPad { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; };
struct XState { DWORD dwPacketNumber; XPad Gamepad; };
typedef DWORD(WINAPI* GetState_t)(DWORD, XState*);
static const WORD BTN_UP = 0x0001, BTN_DOWN = 0x0002, BTN_BACK = 0x0020;

// ---------------- config ----------------
static unsigned g_modVk = 0x11;               // Ctrl; 0 = wheel always adjusts
static float g_step = 0.05f, g_min = 0.10f, g_start = 1.0f;
static float g_deadZone = 7849.0f;            // XInput default left-stick dead zone
static bool g_padAdjust = true, g_showTitle = true, g_log = true;
static std::wstring g_dir;

static std::atomic<float> g_limit{1.0f};      // 1.0 = full stick range, g_min = barely moving
static std::atomic<bool> g_dirty{false};
static std::atomic<long> g_calls{0}, g_scaled{0};
static GetState_t g_origA = nullptr, g_origEx = nullptr;

static FILE* g_f = nullptr;
static void Log(const char* fmt, ...) {
    if (!g_log || !g_f) return;
    va_list a; va_start(a, fmt); vfprintf(g_f, fmt, a); va_end(a);
    fputc('\n', g_f); fflush(g_f);
}

static void AdjustLimit(float delta) {
    float v = g_limit.load() + delta;
    v = std::min(1.0f, std::max(g_min, std::round(v * 100.0f) / 100.0f));
    g_limit.store(v); g_dirty.store(true);
}

// ---------------- XInput detours ----------------
static void Process(XState* s) {
    g_calls++;
    static WORD prev = 0;
    WORD b = s->Gamepad.wButtons;
    if (g_padAdjust && (b & BTN_BACK)) {                        // Back + DPad Up/Down adjusts the limit
        WORD pressed = b & ~prev;
        if (pressed & BTN_UP) AdjustLimit(+g_step);
        if (pressed & BTN_DOWN) AdjustLimit(-g_step);
        s->Gamepad.wButtons &= ~(BTN_UP | BTN_DOWN);            // hide from the game
    }
    prev = b;

    float lim = g_limit.load(std::memory_order_relaxed);
    if (lim < 0.999f) {
        float x = (float)s->Gamepad.sThumbLX, y = (float)s->Gamepad.sThumbLY;
        float m = std::sqrt(x * x + y * y);
        if (m > g_deadZone) {
            float k = (g_deadZone + lim * (32767.0f - g_deadZone)) / 32767.0f;
            s->Gamepad.sThumbLX = (SHORT)(x * k);
            s->Gamepad.sThumbLY = (SHORT)(y * k);
            g_scaled++;
        }
    }
}
static DWORD WINAPI HookA(DWORD i, XState* s) { DWORD r = g_origA(i, s); if (r == ERROR_SUCCESS && s) Process(s); return r; }
static DWORD WINAPI HookEx(DWORD i, XState* s) { DWORD r = g_origEx(i, s); if (r == ERROR_SUCCESS && s) Process(s); return r; }

// ---------------- mouse hook (own thread, blocking message loop = no lag) ----------------
static bool GameFocused() {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_MOUSEWHEEL || wp == WM_MBUTTONDOWN || wp == WM_MBUTTONUP) &&
        GameFocused() && (g_modVk == 0 || (GetAsyncKeyState((int)g_modVk) & 0x8000))) {
        auto* ms = (MSLLHOOKSTRUCT*)lp;
        if (wp == WM_MOUSEWHEEL) {
            short d = (short)((ms->mouseData >> 16) & 0xFFFF);
            AdjustLimit((d / (float)WHEEL_DELTA) * g_step);
            return 1;
        }
        if (wp == WM_MBUTTONDOWN) { g_limit.store(1.0f); g_dirty.store(true); }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
static DWORD WINAPI MouseThread(LPVOID) {
    HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    Log(h ? "mouse hook installed" : "mouse hook FAILED");
    MSG m; while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

// ---------------- helpers ----------------
static BOOL CALLBACK FindWnd(HWND h, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}
static void UpdateTitle() {
    static HWND wnd = nullptr; static wchar_t orig[256] = L"";
    if (!wnd || !IsWindow(wnd)) { wnd = nullptr; EnumWindows(FindWnd, (LPARAM)&wnd); if (wnd) GetWindowTextW(wnd, orig, 256); }
    if (!wnd) return;
    float v = g_limit.load(); wchar_t buf[320];
    if (v >= 0.999f) swprintf(buf, 320, L"%ls", orig);
    else swprintf(buf, 320, L"%ls   [Stick limit %d%%]", orig, (int)std::round(v * 100.0f));
    SetWindowTextW(wnd, buf);
}

static void LoadConfig() {
    std::wstring ini = g_dir + L"ScrollSpeed.ini";
    wchar_t b[64];
    auto rd = [&](const wchar_t* k, const wchar_t* d) { GetPrivateProfileStringW(L"Settings", k, d, b, 64, ini.c_str()); return std::wstring(b); };
    g_modVk = wcstoul(rd(L"Modifier", L"11").c_str(), nullptr, 16);
    g_step = (float)_wtof(rd(L"Step", L"0.05").c_str());
    g_min = (float)_wtof(rd(L"Min", L"0.10").c_str());
    g_start = (float)_wtof(rd(L"Start", L"1.0").c_str());
    g_deadZone = (float)_wtof(rd(L"DeadZone", L"7849").c_str());
    g_padAdjust = _wtoi(rd(L"PadAdjust", L"1").c_str()) != 0;
    g_showTitle = _wtoi(rd(L"ShowInTitle", L"1").c_str()) != 0;
    g_log = _wtoi(rd(L"DebugLog", L"1").c_str()) != 0;
}

static DWORD WINAPI Init(LPVOID hMod) {
    CreateMutexW(nullptr, TRUE, L"Local\\ScrollSpeedV5");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;       // loaded twice -> second copy exits
    wchar_t path[MAX_PATH]; GetModuleFileNameW((HMODULE)hMod, path, MAX_PATH);
    g_dir = path; g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    LoadConfig();
    g_f = _wfopen((g_dir + L"ScrollSpeed.log").c_str(), L"w");
    Log("ScrollSpeed v5 starting");
    g_limit.store(std::min(1.0f, std::max(g_min, g_start)));
    CreateThread(nullptr, 0, MouseThread, nullptr, 0, nullptr);

    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return 0; }
    const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    bool hooked = false; ULONGLONG t0 = GetTickCount64(), lastStat = t0; long lastCalls = 0, lastScaled = 0;
    for (;;) {
        if (!hooked && GetTickCount64() - t0 < 600000) {
            for (const wchar_t* d : dlls) {
                HMODULE xm = GetModuleHandleW(d);
                if (!xm) continue;
                void* a = (void*)GetProcAddress(xm, "XInputGetState");
                void* ex = (void*)GetProcAddress(xm, MAKEINTRESOURCEA(100));   // XInputGetStateEx (undocumented ordinal)
                if (a && MH_CreateHook(a, (LPVOID)HookA, (LPVOID*)&g_origA) == MH_OK && MH_EnableHook(a) == MH_OK) {
                    Log("hooked XInputGetState in %ls", d); hooked = true;
                    if (ex && ex != a && MH_CreateHook(ex, (LPVOID)HookEx, (LPVOID*)&g_origEx) == MH_OK && MH_EnableHook(ex) == MH_OK)
                        Log("hooked XInputGetStateEx in %ls", d);
                    break;
                }
            }
        }
        if (g_dirty.exchange(false)) { Log("stick limit = %.2f", g_limit.load()); if (g_showTitle) UpdateTitle(); }
        if (GetTickCount64() - lastStat > 10000) {          // diagnostics every 10 s
            long c = g_calls.load(), s = g_scaled.load();
            Log("[10s] XInputGetState calls=%ld, stick samples scaled=%ld, limit=%.2f, hooked=%d", c - lastCalls, s - lastScaled, g_limit.load(), (int)hooked);
            lastCalls = c; lastScaled = s; lastStat = GetTickCount64();
        }
        Sleep(200);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(nullptr, 0, Init, h, 0, nullptr); }
    return TRUE;
}
