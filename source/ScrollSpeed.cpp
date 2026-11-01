// ScrollSpeed v3 (EXPERIMENT) - Crimson Desert ASI plugin
// Idea: the game's own walk/run speed gains come from the static data record "MoveSpeedRate"
// (statusinfo). Community data-patch mods edit that record on disk. This plugin looks for that
// record in the running game's memory and rewrites its 16 level entries live; the scroll wheel
// changes the multiplier. WHETHER THE GAME RE-READS THE TABLE AT RUNTIME IS UNVERIFIED - the log
// tells us. Nothing here is guaranteed to work; check ScrollSpeed.log.
//
// Record bytes below are taken from the "original" bytes in the Movement Speed Gains mod's patch
// files (Steam build 25477059). Table layout (verified by the mod's own patched values being exact
// multiples): record offset +89, 16 x int64, fixed-point 32.32.
//
// Keys: Modifier(Ctrl)+Wheel = change multiplier, Modifier+MiddleClick = reset (restores originals)
//       F6 = rescan memory, F8 = restore originals.  Single-player only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const uint8_t kRec[246] = {
    0x4B,0x42,0x0F,0x00,0x0D,0x00,0x00,0x00,0x4D,0x6F,0x76,0x65,0x53,0x70,0x65,0x65,
    0x64,0x52,0x61,0x74,0x65,0x00,0x00,0x08,0x00,0x00,0x00,0x01,0x01,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03,0x0A,0x02,0x63,0x44,0x0F,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x23,0xB2,0x25,0xAD,0xEF,0xCF,0x99,
    0xDC,0x9D,0xB7,0xD3,0x61,0x00,0x01,0x00,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x2D,0x31,0x01,0x00,0x00,0x00,0x00,0x00,0x5A,0x62,0x02,
    0x00,0x00,0x00,0x00,0x00,0x87,0x93,0x03,0x00,0x00,0x00,0x00,0x00,0xB4,0xC4,0x04,
    0x00,0x00,0x00,0x00,0x00,0xE1,0xF5,0x05,0x00,0x00,0x00,0x00,0x00,0x0E,0x27,0x07,
    0x00,0x00,0x00,0x00,0xC0,0xF8,0x48,0x08,0x00,0x00,0x00,0x00,0x40,0xA1,0x5B,0x09,
    0x00,0x00,0x00,0x00,0x80,0x07,0x5F,0x0A,0x00,0x00,0x00,0x00,0x80,0x2B,0x53,0x0B,
    0x00,0x00,0x00,0x00,0x00,0xCB,0x28,0x0C,0x00,0x00,0x00,0x00,0x40,0x28,0xEF,0x0C,
    0x00,0x00,0x00,0x00,0x40,0x43,0xA6,0x0D,0x00,0x00,0x00,0x00,0x00,0x1C,0x4E,0x0E,
    0x00,0x00,0x00,0x00,0x80,0xB2,0xE6,0x0E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x41,0xDC,0x11,0xF3,0x23,0x87,0x8D,0x49,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,
};
static const size_t kTblOff = 89, kHdrLen = 21;   // header = key(4) + nameLen(4) + "MoveSpeedRate"

// ---------------- config ----------------
static unsigned g_modVk = 0x11;
static double g_step = 0.25, g_min = 0.25, g_max = 8.0, g_start = 1.0;
static int g_flat = 1;                  // 1: set ALL 16 levels to (level-15 value * mult); 0: scale each level
static DWORD g_initDelay = 45000;       // auto-scan this long after launch (also F6)
static bool g_showTitle = true, g_log = true;
static std::wstring g_dir;

// ---------------- state ----------------
struct Hit { uint8_t* rec; };
static std::vector<Hit> g_hits;
static double g_mult = 1.0;
static CRITICAL_SECTION g_cs;

static void Log(const char* fmt, ...) {
    if (!g_log) return;
    std::wstring p = g_dir + L"ScrollSpeed.log";
    FILE* f = _wfopen(p.c_str(), L"a");
    if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

// ---------------- memory scan ----------------
static bool Readable(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    DWORD p = m.Protect & 0xFF;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static void Scan() {
    EnterCriticalSection(&g_cs);
    g_hits.clear();
    uint8_t* a = nullptr; size_t regions = 0, bytes = 0, anchors = 0;
    MEMORY_BASIC_INFORMATION m;
    while (VirtualQuery(a, &m, sizeof(m)) == sizeof(m)) {
        uint8_t* base = (uint8_t*)m.BaseAddress; size_t sz = m.RegionSize;
        a = base + sz;
        if (!Readable(m) || sz < sizeof(kRec)) continue;
        regions++; bytes += sz;
        const uint8_t* p = base; const uint8_t* end = base + sz - kHdrLen;
        while (p < end) {
            p = (const uint8_t*)memchr(p, 0x4B, end - p);
            if (!p) break;
            if (memcmp(p, kRec, kHdrLen) == 0) {
                anchors++;
                size_t same = 0, n = std::min<size_t>(sizeof(kRec), (size_t)(base + sz - p));
                for (size_t i = 0; i < n; i++) same += (p[i] == kRec[i]);
                bool tbl = (size_t)(base + sz - p) >= kTblOff + 128 && memcmp(p + kTblOff, kRec + kTblOff, 128) == 0;
                Log("anchor at %p: %zu/%zu record bytes identical, table %s, region type=0x%lX prot=0x%lX",
                    (void*)p, same, sizeof(kRec), tbl ? "ORIGINAL" : "differs", m.Type, m.Protect);
                if (tbl) g_hits.push_back({ (uint8_t*)p });
            }
            p++;
        }
    }
    Log("scan done: %zu regions, %.1f MB, %zu anchor(s), %zu usable table(s)", regions, bytes / 1048576.0, anchors, g_hits.size());
    LeaveCriticalSection(&g_cs);
}

// ---------------- apply ----------------
static void Apply() {
    EnterCriticalSection(&g_cs);
    for (auto& h : g_hits) {
        int64_t out[16]; const uint8_t* orig = kRec + kTblOff; int64_t o15; memcpy(&o15, orig + 15 * 8, 8);
        for (int k = 0; k < 16; k++) {
            int64_t o; memcpy(&o, orig + k * 8, 8);
            double base = (g_flat && g_mult != 1.0) ? (double)o15 : (double)o;
            out[k] = (g_mult == 1.0) ? o : (int64_t)(base * g_mult);
        }
        DWORD old;
        if (VirtualProtect(h.rec + kTblOff, 128, PAGE_READWRITE, &old)) {
            memcpy(h.rec + kTblOff, out, 128);
            VirtualProtect(h.rec + kTblOff, 128, old, &old);
        } else Log("VirtualProtect failed at %p", (void*)h.rec);
    }
    Log("applied multiplier %.2f (%s) to %zu table(s)", g_mult, g_flat ? "flat" : "scale", g_hits.size());
    LeaveCriticalSection(&g_cs);
}

// ---------------- title / input ----------------
static BOOL CALLBACK FindWnd(HWND h, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}
static void ShowSpeed() {
    if (!g_showTitle) return;
    static HWND wnd = nullptr; static wchar_t orig[256] = L"";
    if (!wnd || !IsWindow(wnd)) { wnd = nullptr; EnumWindows(FindWnd, (LPARAM)&wnd); if (wnd) GetWindowTextW(wnd, orig, 256); }
    if (!wnd) return;
    wchar_t buf[320];
    if (g_mult == 1.0) swprintf(buf, 320, L"%ls", orig);
    else swprintf(buf, 320, L"%ls   [MoveSpeedRate x%.2f, %zu table(s)]", orig, g_mult, g_hits.size());
    SetWindowTextW(wnd, buf);
}
static bool GameFocused() {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static bool ModDown() { return g_modVk == 0 || (GetAsyncKeyState((int)g_modVk) & 0x8000); }
static void SetMult(double v) { g_mult = std::min(g_max, std::max(g_min, std::round(v * 100.0) / 100.0)); Apply(); ShowSpeed(); }

static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && GameFocused() && ModDown()) {
        auto* ms = (MSLLHOOKSTRUCT*)lp;
        if (wp == WM_MOUSEWHEEL) {
            short d = (short)((ms->mouseData >> 16) & 0xFFFF);
            SetMult(g_mult + (d / (double)WHEEL_DELTA) * g_step);
            return 1;
        }
        if (wp == WM_MBUTTONDOWN) { SetMult(1.0); return 1; }
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
    g_flat = _wtoi(rd(L"Flat", L"1").c_str());
    g_initDelay = (DWORD)_wtoi(rd(L"ScanDelayMs", L"45000").c_str());
    g_showTitle = _wtoi(rd(L"ShowInTitle", L"1").c_str()) != 0;
    g_log = _wtoi(rd(L"DebugLog", L"1").c_str()) != 0;
}

static DWORD WINAPI Init(LPVOID hMod) {
    InitializeCriticalSection(&g_cs);
    wchar_t path[MAX_PATH]; GetModuleFileNameW((HMODULE)hMod, path, MAX_PATH);
    g_dir = path; g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    LoadConfig();
    Log("ScrollSpeed v3 experiment starting");
    HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    Log(h ? "mouse hook installed" : "mouse hook FAILED");

    ULONGLONG t0 = GetTickCount64(); bool scanned = false, f6 = false, f8 = false;
    for (;;) {
        MSG m; while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        if (!scanned && GetTickCount64() - t0 > g_initDelay) {
            scanned = true; Scan(); if (g_start != 1.0) { g_mult = g_start; Apply(); ShowSpeed(); }
        }
        bool k6 = GameFocused() && (GetAsyncKeyState(VK_F6) & 0x8000), k8 = GameFocused() && (GetAsyncKeyState(VK_F8) & 0x8000);
        if (k6 && !f6) { Scan(); Apply(); ShowSpeed(); }
        if (k8 && !f8) { g_mult = 1.0; Apply(); ShowSpeed(); }
        f6 = k6; f8 = k8;
        Sleep(15);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(nullptr, 0, Init, h, 0, nullptr); }
    return TRUE;
}
