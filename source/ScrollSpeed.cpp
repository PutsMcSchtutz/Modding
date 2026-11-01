// ScrollSpeed - Crimson Desert ASI plugin
// Hold a modifier key (default Ctrl) and use the mouse wheel to change game speed live.
// Method: hooks QueryPerformanceCounter (same patch-proof approach as JustSkip), so no
// game offsets are needed and it survives game updates.
// NOTE: this scales the whole game clock, not only the player's movement.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <MinHook.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

// ---------------- config ----------------
static unsigned g_modVk = 0x11;     // Ctrl. 0 = no modifier (wheel always controls speed)
static double g_step = 0.25;        // speed change per wheel notch
static double g_min = 0.25, g_max = 8.0, g_start = 1.0;
static bool g_showTitle = true, g_log = false;
static std::wstring g_dir;

// ---------------- time scaling ----------------
typedef BOOL(WINAPI* QPC_t)(LARGE_INTEGER*);
static QPC_t g_origQPC = nullptr;
static SRWLOCK g_lock = SRWLOCK_INIT;
static LONGLONG g_realBase = 0, g_virtBase = 0;
static double g_scale = 1.0;

static BOOL WINAPI HookQPC(LARGE_INTEGER* out) {
    if (!out) return g_origQPC(out);
    AcquireSRWLockShared(&g_lock);
    LARGE_INTEGER real;
    BOOL ok = g_origQPC(&real);
    if (ok) out->QuadPart = g_virtBase + (LONGLONG)((double)(real.QuadPart - g_realBase) * g_scale);
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

static void SetScale(double s) {
    s = std::min(g_max, std::max(g_min, std::round(s * 100.0) / 100.0));
    AcquireSRWLockExclusive(&g_lock);
    LARGE_INTEGER real;
    g_origQPC(&real);
    g_virtBase += (LONGLONG)((double)(real.QuadPart - g_realBase) * g_scale); // keep clock continuous
    g_realBase = real.QuadPart;
    g_scale = s;
    ReleaseSRWLockExclusive(&g_lock);
}

// ---------------- helpers ----------------
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    std::wstring p = g_dir + L"ScrollSpeed.log";
    FILE* f = _wfopen(p.c_str(), L"a");
    if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

static BOOL CALLBACK FindWnd(HWND h, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) {
        *(HWND*)lp = h; return FALSE;
    }
    return TRUE;
}

static void ShowSpeed() {
    Log("speed = %.2fx", g_scale);
    if (!g_showTitle) return;
    static HWND wnd = nullptr; static wchar_t orig[256] = L"";
    if (!wnd || !IsWindow(wnd)) { wnd = nullptr; EnumWindows(FindWnd, (LPARAM)&wnd); if (wnd) GetWindowTextW(wnd, orig, 256); }
    if (!wnd) return;
    wchar_t buf[320];
    if (std::fabs(g_scale - 1.0) < 0.001) swprintf(buf, 320, L"%ls", orig);
    else swprintf(buf, 320, L"%ls   [Speed %.2fx]", orig, g_scale);
    SetWindowTextW(wnd, buf);
}

static bool GameFocused() {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static bool ModDown() { return g_modVk == 0 || (GetAsyncKeyState((int)g_modVk) & 0x8000); }

// ---------------- mouse hook ----------------
static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && GameFocused() && ModDown()) {
        auto* ms = (MSLLHOOKSTRUCT*)lp;
        if (wp == WM_MOUSEWHEEL) {
            short d = (short)((ms->mouseData >> 16) & 0xFFFF);
            SetScale(g_scale + (d / (double)WHEEL_DELTA) * g_step);
            ShowSpeed();
            return 1;                       // swallow so the game doesn't also zoom/switch
        }
        if (wp == WM_MBUTTONDOWN) { SetScale(1.0); ShowSpeed(); return 1; }  // modifier + middle click = reset
        if (wp == WM_MBUTTONUP) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void LoadConfig() {
    std::wstring ini = g_dir + L"ScrollSpeed.ini";
    wchar_t b[64];
    auto rd = [&](const wchar_t* k, const wchar_t* d) { GetPrivateProfileStringW(L"Settings", k, d, b, 64, ini.c_str()); return std::wstring(b); };
    g_modVk = wcstoul(rd(L"Modifier", L"11").c_str(), nullptr, 16);
    g_step = _wtof(rd(L"Step", L"0.25").c_str());
    g_min = _wtof(rd(L"Min", L"0.25").c_str());
    g_max = _wtof(rd(L"Max", L"8.0").c_str());
    g_start = _wtof(rd(L"Start", L"1.0").c_str());
    g_showTitle = _wtoi(rd(L"ShowInTitle", L"1").c_str()) != 0;
    g_log = _wtoi(rd(L"DebugLog", L"0").c_str()) != 0;
}

static DWORD WINAPI Init(LPVOID hMod) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW((HMODULE)hMod, path, MAX_PATH);
    g_dir = path; g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    LoadConfig();
    Log("ScrollSpeed starting");

    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return 0; }
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    g_realBase = g_virtBase = now.QuadPart;
    if (MH_CreateHookApi(L"kernel32", "QueryPerformanceCounter", (LPVOID)HookQPC, (LPVOID*)&g_origQPC) != MH_OK ||
        MH_EnableHook(MH_ALL_HOOKS) != MH_OK) { Log("QPC hook failed"); return 0; }
    Log("QPC hook installed");
    if (g_start != 1.0) SetScale(g_start);

    HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    Log(h ? "mouse hook installed" : "mouse hook FAILED");
    MSG m; while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Init, h, 0, nullptr);
    }
    return TRUE;
}
