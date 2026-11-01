// ScrollSpeed v2 - Crimson Desert ASI plugin
// Hold a modifier (default Ctrl) + mouse wheel = change the PLAYER's horizontal movement speed.
//
// How it works: finds the game's controlled-character position update by byte signature
//   (addps xmm0,[r13] ; movups [r13],xmm0), redirects it through a small code cave, and scales
//   the per-update X/Z movement delta by the current multiplier. World clock is untouched,
//   so enemies/animations/physics keep normal speed. No third-party libraries.
//
// Signature research credit: Orcax-1399/CrimsonDesert-player-status-modifier (position_research.md).
// Single-player use only.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>
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

// ---------------- config ----------------
static unsigned g_modVk = 0x11;          // Ctrl. 0 = wheel always active
static float g_step = 0.25f, g_min = 0.25f, g_max = 8.0f, g_start = 1.0f;
static float g_maxDelta = 5.0f;          // ignore per-update deltas larger than this (teleports etc.)
static int g_axisA = 0, g_axisB = 2;     // horizontal axes inside the position vector (x,y,z,w = 0..3)
static DWORD g_initDelay = 3000;
static bool g_showTitle = true, g_log = false;
static std::wstring g_dir;

static std::atomic<float> g_mult{1.0f};

// ---------------- log ----------------
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    std::wstring p = g_dir + L"ScrollSpeed.log";
    FILE* f = _wfopen(p.c_str(), L"a");
    if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

// ---------------- called from the code cave ----------------
// nv  = pointer to the NEW position vector (old + delta), may be edited
// old = pointer to the CURRENT (pre-write) position vector
static void __cdecl Adjust(float* nv, const float* old) {
    float m = g_mult.load(std::memory_order_relaxed);
    if (m == 1.0f) return;
    float da = nv[g_axisA] - old[g_axisA], db = nv[g_axisB] - old[g_axisB];
    if (!(std::fabs(da) < g_maxDelta && std::fabs(db) < g_maxDelta)) return; // also rejects NaN
    nv[g_axisA] = old[g_axisA] + da * m;
    nv[g_axisB] = old[g_axisB] + db * m;
}

// ---------------- code cave (assembled & verified with GNU as/objdump) ----------------
static const uint8_t kCave[] = {
    0x55,0x48,0x89,0xE5,0x9C,0x50,0x51,0x52,0x41,0x50,0x41,0x51,0x41,0x52,0x41,0x53,
    0x48,0x81,0xEC,0x80,0x00,0x00,0x00,0x48,0x83,0xE4,0xF0,0x0F,0x11,0x44,0x24,0x20,
    0x0F,0x11,0x4C,0x24,0x30,0x0F,0x11,0x54,0x24,0x40,0x0F,0x11,0x5C,0x24,0x50,0x0F,
    0x11,0x64,0x24,0x60,0x0F,0x11,0x6C,0x24,0x70,0x48,0x8D,0x4C,0x24,0x20,0x4C,0x89,
    0xEA,0x48,0xB8,0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11,0xFF,0xD0,0x0F,0x10,0x44,
    0x24,0x20,0x0F,0x10,0x4C,0x24,0x30,0x0F,0x10,0x54,0x24,0x40,0x0F,0x10,0x5C,0x24,
    0x50,0x0F,0x10,0x64,0x24,0x60,0x0F,0x10,0x6C,0x24,0x70,0x48,0x8D,0x65,0xC0,0x41,
    0x5B,0x41,0x5A,0x41,0x59,0x41,0x58,0x5A,0x59,0x58,0x9D,0x5D,0x41,0x0F,0x11,0x45,
    0x00,0xFF,0x25,0x00,0x00,0x00,0x00,0x00,0xFF,0xEE,0xDD,0xCC,0xBB,0xAA,0x99,
};
static const size_t kFnImmOff = 67, kRetImmOff = 135;           // movabs rax,<Adjust> ; .quad <return addr>
static const uint8_t kOrig[5] = { 0x41, 0x0F, 0x11, 0x45, 0x00 }; // movups [r13],xmm0

// ---------------- signature scan ----------------
struct Sig { const char* name; const char* pat; int off; };
static const Sig kSigs[] = {
    { "primary",  "49 3B F7 0F 8C ?? ?? ?? ?? 0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00 48 8B BB F8 00 00 00 48 63 83 00 01 00 00", 0x16 },
    { "short",    "0F 28 C6 F3 45 0F 5C C8 41 0F 58 45 00 41 0F 11 45 00", 0x0D },
    { "context",  "41 0F 58 45 00 41 0F 11 45 00 48 8B BB F8 00 00 00 48 63 83 00 01 00 00 48 8D 34 C7 48 3B FE 74 20", 0x05 },
};

static void ParsePat(const char* s, std::vector<int>& out) {
    out.clear();
    while (*s) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (s[0] == '?') { out.push_back(-1); s += (s[1] == '?') ? 2 : 1; }
        else { out.push_back((int)strtoul(std::string(s, 2).c_str(), nullptr, 16)); s += 2; }
    }
}

static uint8_t* ScanModule(HMODULE mod, const std::vector<int>& pat, int& hits) {
    auto* base = (uint8_t*)mod;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    uint8_t* found = nullptr; hits = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uint8_t* s = base + sec->VirtualAddress;
        size_t n = sec->Misc.VirtualSize;
        if (n < pat.size()) continue;
        for (size_t p = 0; p + pat.size() <= n; p++) {
            size_t k = 0;
            while (k < pat.size() && (pat[k] < 0 || s[p + k] == (uint8_t)pat[k])) k++;
            if (k == pat.size()) { if (!found) found = s + p; hits++; }
        }
    }
    return found;
}

// ---------------- patching ----------------
static void SuspendOthers(std::vector<HANDLE>& hs, bool suspend) {
    if (!suspend) { for (HANDLE h : hs) { ResumeThread(h); CloseHandle(h); } hs.clear(); return; }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{ sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (h) { if (SuspendThread(h) != (DWORD)-1) hs.push_back(h); else CloseHandle(h); }
    }
    CloseHandle(snap);
}

static uint8_t* AllocNear(uint8_t* target, size_t size) {
    const uintptr_t t = (uintptr_t)target, gran = 0x10000, span = 0x70000000;
    for (uintptr_t d = gran; d < span; d += gran) {
        for (int dir = 0; dir < 2; dir++) {
            uintptr_t a = dir ? t + d : (t > d ? t - d : 0);
            if (!a) continue;
            a &= ~(gran - 1);
            void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return (uint8_t*)p;
        }
    }
    return nullptr;
}

static bool InstallHook(uint8_t* H) {
    if (memcmp(H, kOrig, 5) != 0) { Log("target bytes mismatch, aborting"); return false; }
    uint8_t* cave = AllocNear(H, 256);
    if (!cave) { Log("no cave memory near target"); return false; }
    memcpy(cave, kCave, sizeof(kCave));
    uint64_t fn = (uint64_t)(uintptr_t)&Adjust, ret = (uint64_t)(uintptr_t)(H + 5);
    memcpy(cave + kFnImmOff, &fn, 8);
    memcpy(cave + kRetImmOff, &ret, 8);
    FlushInstructionCache(GetCurrentProcess(), cave, sizeof(kCave));

    int64_t rel = (int64_t)(cave - (H + 5));
    if (rel > INT32_MAX || rel < INT32_MIN) { Log("cave out of rel32 range"); return false; }
    uint8_t jmp[5] = { 0xE9 }; int32_t r32 = (int32_t)rel; memcpy(jmp + 1, &r32, 4);

    std::vector<HANDLE> hs; SuspendOthers(hs, true);
    DWORD old;
    bool ok = VirtualProtect(H, 5, PAGE_EXECUTE_READWRITE, &old) != 0;
    if (ok) { memcpy(H, jmp, 5); VirtualProtect(H, 5, old, &old); FlushInstructionCache(GetCurrentProcess(), H, 5); }
    SuspendOthers(hs, false);
    Log(ok ? "hook installed at %p (cave %p)" : "VirtualProtect failed", (void*)H, (void*)cave);
    return ok;
}

// ---------------- UI feedback / input ----------------
static BOOL CALLBACK FindWnd(HWND h, LPARAM lp) {
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { *(HWND*)lp = h; return FALSE; }
    return TRUE;
}
static void ShowSpeed() {
    float m = g_mult.load();
    Log("move speed multiplier = %.2f", m);
    if (!g_showTitle) return;
    static HWND wnd = nullptr; static wchar_t orig[256] = L"";
    if (!wnd || !IsWindow(wnd)) { wnd = nullptr; EnumWindows(FindWnd, (LPARAM)&wnd); if (wnd) GetWindowTextW(wnd, orig, 256); }
    if (!wnd) return;
    wchar_t buf[320];
    if (std::fabs(m - 1.0f) < 0.001f) swprintf(buf, 320, L"%ls", orig);
    else swprintf(buf, 320, L"%ls   [Move speed %.2fx]", orig, m);
    SetWindowTextW(wnd, buf);
}
static bool GameFocused() {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
static bool ModDown() { return g_modVk == 0 || (GetAsyncKeyState((int)g_modVk) & 0x8000); }
static void SetMult(float v) { g_mult.store(std::min(g_max, std::max(g_min, std::round(v * 100.0f) / 100.0f))); }

static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && GameFocused() && ModDown()) {
        auto* ms = (MSLLHOOKSTRUCT*)lp;
        if (wp == WM_MOUSEWHEEL) {
            short d = (short)((ms->mouseData >> 16) & 0xFFFF);
            SetMult(g_mult.load() + (d / (float)WHEEL_DELTA) * g_step);
            ShowSpeed();
            return 1;
        }
        if (wp == WM_MBUTTONDOWN) { SetMult(1.0f); ShowSpeed(); return 1; }
        if (wp == WM_MBUTTONUP) return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void LoadConfig() {
    std::wstring ini = g_dir + L"ScrollSpeed.ini";
    wchar_t b[64];
    auto rd = [&](const wchar_t* k, const wchar_t* d) { GetPrivateProfileStringW(L"Settings", k, d, b, 64, ini.c_str()); return std::wstring(b); };
    g_modVk = wcstoul(rd(L"Modifier", L"11").c_str(), nullptr, 16);
    g_step = (float)_wtof(rd(L"Step", L"0.25").c_str());
    g_min = (float)_wtof(rd(L"Min", L"0.25").c_str());
    g_max = (float)_wtof(rd(L"Max", L"8.0").c_str());
    g_start = (float)_wtof(rd(L"Start", L"1.0").c_str());
    g_maxDelta = (float)_wtof(rd(L"MaxDeltaPerUpdate", L"5.0").c_str());
    g_axisA = std::clamp(_wtoi(rd(L"AxisA", L"0").c_str()), 0, 3);
    g_axisB = std::clamp(_wtoi(rd(L"AxisB", L"2").c_str()), 0, 3);
    g_initDelay = (DWORD)_wtoi(rd(L"InitDelayMs", L"3000").c_str());
    g_showTitle = _wtoi(rd(L"ShowInTitle", L"1").c_str()) != 0;
    g_log = _wtoi(rd(L"DebugLog", L"0").c_str()) != 0;
}

static DWORD WINAPI Init(LPVOID hMod) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW((HMODULE)hMod, path, MAX_PATH);
    g_dir = path; g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    LoadConfig();
    Log("ScrollSpeed v2 starting");
    SetMult(g_start);
    Sleep(g_initDelay);

    HMODULE exe = GetModuleHandleW(nullptr);
    bool installed = false;
    for (const Sig& s : kSigs) {
        std::vector<int> pat; ParsePat(s.pat, pat);
        int hits = 0; uint8_t* m = ScanModule(exe, pat, hits);
        Log("signature '%s': %d match(es)", s.name, hits);
        if (m && hits == 1) { installed = InstallHook(m + s.off); if (installed) break; }
    }
    if (!installed) Log("NO HOOK INSTALLED - game version likely changed; signature needs updating");

    HHOOK h = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    Log(h ? "mouse hook installed" : "mouse hook FAILED");
    MSG m; while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(nullptr, 0, Init, h, 0, nullptr); }
    return TRUE;
}
