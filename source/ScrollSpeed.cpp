// ScrollSpeed PROBE v4 - READ-ONLY diagnostic. Writes nothing to the game, installs no hooks.
// Purpose: find out whether/how the game keeps the "MoveSpeedRate" speed table in memory.
// It searches process memory (excluding this DLL itself) for:
//   - the ASCII name "MoveSpeedRate"
//   - the 16-entry table in several encodings (file format 32.32 fixed, int64, int32, float, double)
// and logs every hit with module / region type / surrounding bytes into ScrollSpeed.log.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static std::wstring g_dir;
static uint8_t* g_selfBase = nullptr; static size_t g_selfSize = 0;
static FILE* g_f = nullptr;

static void Log(const char* fmt, ...) {
    if (!g_f) return;
    va_list a; va_start(a, fmt); vfprintf(g_f, fmt, a); va_end(a);
    fputc('\n', g_f); fflush(g_f);
}

struct Pat { std::string name; std::vector<uint8_t> b; };

static void PushI64(std::vector<uint8_t>& v, int64_t x) { uint8_t t[8]; memcpy(t, &x, 8); v.insert(v.end(), t, t + 8); }
static void PushI32(std::vector<uint8_t>& v, int32_t x) { uint8_t t[4]; memcpy(t, &x, 4); v.insert(v.end(), t, t + 4); }
static void PushF32(std::vector<uint8_t>& v, float x) { uint8_t t[4]; memcpy(t, &x, 4); v.insert(v.end(), t, t + 4); }
static void PushF64(std::vector<uint8_t>& v, double x) { uint8_t t[8]; memcpy(t, &x, 8); v.insert(v.end(), t, t + 8); }

static std::vector<Pat> MakePats() {
    // Original table entries (file format = value * 2^32 where value = 78125 * k for k=0..5, then flattening)
    const int64_t fx[16] = { 0, 335544320000000LL, 671088640000000LL, 1006632960000000LL, 1342177280000000LL,
        1677721600000000LL, 2013265920000000LL, 2332033024000000LL, 2634022912000000LL, 2919235584000000LL,
        3187671040000000LL, 3422552064000000LL, 3640655872000000LL, 3841982464000000LL, 4026531840000000LL, 4194304000000000LL };
    std::vector<Pat> P;
    { Pat p{ "ascii MoveSpeedRate" }; const char* s = "MoveSpeedRate"; p.b.assign(s, s + 13); P.push_back(p); }
    { Pat p{ "ascii MoveSpeedLevel" }; const char* s = "MoveSpeedLevel"; p.b.assign(s, s + 14); P.push_back(p); }
    { Pat p{ "table 16x fixed32.32 (file format)" }; for (int i = 0; i < 16; i++) PushI64(p.b, fx[i]); P.push_back(p); }
    { Pat p{ "first 5x fixed32.32" }; for (int i = 0; i < 5; i++) PushI64(p.b, fx[i]); P.push_back(p); }
    { Pat p{ "first 6x int64 plain (0,78125,..)" }; for (int i = 0; i < 6; i++) PushI64(p.b, 78125LL * i); P.push_back(p); }
    { Pat p{ "first 6x int32 plain" }; for (int i = 0; i < 6; i++) PushI32(p.b, (int32_t)(78125 * i)); P.push_back(p); }
    { Pat p{ "first 6x float (0.078125*k)" }; for (int i = 0; i < 6; i++) PushF32(p.b, 0.078125f * i); P.push_back(p); }
    { Pat p{ "first 6x double (0.078125*k)" }; for (int i = 0; i < 6; i++) PushF64(p.b, 0.078125 * i); P.push_back(p); }
    { Pat p{ "first 6x float (78125*k)" }; for (int i = 0; i < 6; i++) PushF32(p.b, 78125.0f * i); P.push_back(p); }
    { Pat p{ "first 6x double (78125*k)" }; for (int i = 0; i < 6; i++) PushF64(p.b, 78125.0 * i); P.push_back(p); }
    return P;
}

static bool Readable(const MEMORY_BASIC_INFORMATION& m) {
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    DWORD p = m.Protect & 0xFF;
    return p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READ ||
           p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static void DescribeAndDump(const uint8_t* hit, const MEMORY_BASIC_INFORMATION& m, const uint8_t* regBase, size_t regSize) {
    char mod[MAX_PATH] = "-";
    HMODULE hm = nullptr;
    if (m.Type == MEM_IMAGE && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)hit, &hm))
        GetModuleFileNameA(hm, mod, MAX_PATH);
    const uint8_t* s = std::max(hit - 16, regBase);
    const uint8_t* e = std::min(hit + 80, regBase + regSize);
    std::string hex; char t[4];
    for (const uint8_t* p = s; p < e; p++) { snprintf(t, 4, "%02x", *p); hex += t; if (p == hit - 1) hex += " | "; }
    Log("    at %p  type=0x%lX prot=0x%lX module=%s", (void*)hit, m.Type, m.Protect, mod);
    Log("    bytes(-16..+80): %s", hex.c_str());
}

static void Scan() {
    std::vector<Pat> P = MakePats();
    std::vector<int> counts(P.size(), 0);
    Log("=== scan start ===");
    uint8_t* a = nullptr; size_t regions = 0, bytes = 0;
    MEMORY_BASIC_INFORMATION m;
    while (VirtualQuery(a, &m, sizeof(m)) == sizeof(m)) {
        uint8_t* base = (uint8_t*)m.BaseAddress; size_t sz = m.RegionSize;
        a = base + sz;
        if (!Readable(m)) continue;
        if (base < g_selfBase + g_selfSize && base + sz > g_selfBase) continue;   // skip our own DLL
        regions++; bytes += sz;
        for (size_t pi = 0; pi < P.size(); pi++) {
            const auto& pat = P[pi].b;
            if (sz < pat.size()) continue;
            const uint8_t* p = base; const uint8_t* end = base + sz - pat.size() + 1;
            while (p < end) {
                p = (const uint8_t*)memchr(p, pat[0], end - p);
                if (!p) break;
                if (memcmp(p, pat.data(), pat.size()) == 0) {
                    if (++counts[pi] <= 12) { Log("HIT [%s] #%d", P[pi].name.c_str(), counts[pi]); DescribeAndDump(p, m, base, sz); }
                }
                p++;
            }
        }
    }
    Log("=== scan done: %zu regions, %.1f MB (own DLL excluded) ===", regions, bytes / 1048576.0);
    for (size_t i = 0; i < P.size(); i++) Log("  total hits [%s]: %d", P[i].name.c_str(), counts[i]);
}

static DWORD WINAPI Init(LPVOID hMod) {
    HANDLE mtx = CreateMutexW(nullptr, TRUE, L"Local\\ScrollSpeedProbeV4");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;       // loaded twice -> second copy exits
    wchar_t path[MAX_PATH]; GetModuleFileNameW((HMODULE)hMod, path, MAX_PATH);
    g_dir = path; g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    g_selfBase = (uint8_t*)hMod;
    auto* nt = (IMAGE_NT_HEADERS*)(g_selfBase + ((IMAGE_DOS_HEADER*)g_selfBase)->e_lfanew);
    g_selfSize = nt->OptionalHeader.SizeOfImage;
    g_f = _wfopen((g_dir + L"ScrollSpeedProbe.log").c_str(), L"w");
    Log("ScrollSpeed PROBE v4 (read-only). own DLL %p size %zu", (void*)g_selfBase, g_selfSize);

    std::wstring ini = g_dir + L"ScrollSpeed.ini";
    DWORD delay = GetPrivateProfileIntW(L"Settings", L"ScanDelayMs", 60000, ini.c_str());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    Sleep(delay);                   // load a save before this elapses
    Scan();                         // automatic scan once
    bool f6 = false;
    for (;;) {                      // press F6 (game focused) to scan again, e.g. after changing something
        HWND fg = GetForegroundWindow(); DWORD pid = 0; if (fg) GetWindowThreadProcessId(fg, &pid);
        bool k = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F6) & 0x8000);
        if (k && !f6) Scan();
        f6 = k; Sleep(50);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(nullptr, 0, Init, h, 0, nullptr); }
    return TRUE;
}
