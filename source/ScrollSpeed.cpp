// ScrollSpeed keyboard/mouse-only - Crimson Desert ASI plugin
// Keyboard/mouse-only build: native keyboard movement + game-side movement-vector scaling.
// All XInput interception and physical-controller processing have been removed.
//
// The keyboard movement hook is based on the user's Cheat Engine table, entry 45 ("walk key"):
//   CrimsonDesert.exe+81EAF9
//   mov rcx,rdi
//   call CrimsonDesert.exe+81D1E0
//   mov [rdi+40],rbx
//   movaps xmm0,xmm11
// The table scales the two float components at [rdi+40] and [rdi+44]. This plugin applies
// the current wheel-selected multiplier there, but only while native WASD movement is held.
//
// Weapon state and active combat are deliberately separate:
//   sheathed   -> g_weaponOut=false, g_combatActive=false
//   unsheathed -> g_weaponOut=true,  g_combatActive=false unless attack/guard input occurs
//   combat    -> g_weaponOut=true,  g_combatActive=true until CombatHoldMs expires or stowed
//
// This plugin does NOT try to cancel the game's auto-jog-on-unsheathe behavior. The supplied
// Cheat Engine table does not expose the game's jog-state variable; the vector multiplier is
// not a substitute for identifying/overriding that internal state.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>

// ---------------- config ----------------
static unsigned g_modVk = 0x11;               // Ctrl; 0 = wheel always adjusts
static unsigned g_toggleVk = 0x91;            // Scroll Lock toggles keyboard vector scaling; 0 = none
static float g_step = 0.05f, g_min = 0.10f, g_start = 0.80f;
static bool g_showTitle = true, g_log = true;
static bool g_keyboardVectorEnabled = true;
static std::wstring g_dir;

static std::atomic<float> g_limit{1.0f};       // native WASD movement-vector multiplier
static std::atomic<float> g_combatLimit{0.30f};
static std::atomic<bool> g_dirty{false};
static std::atomic<bool> g_keyboardScaleEnabled{true};
static std::atomic<bool> g_key[4]{};            // W, A, S, D
// The injected x64 trampoline reads this aligned 32-bit float bit-pattern directly.
alignas(4) static volatile LONG g_keyboardVectorScaleBits = 0x3F800000L; // 1.0f
static std::atomic<float> g_keyboardVectorScale{1.0f};

// State model: unsheathed is not combat.
static std::atomic<bool> g_weaponOut{false};
static std::atomic<bool> g_combatActive{false};
static std::atomic<bool> g_combatRun{false};     // combat binary gait: false=walk, true=run
static std::atomic<ULONGLONG> g_lastCombat{0};
static bool g_combatDetect = true;
static DWORD g_combatHoldMs = 5000;               // active-combat inactivity timeout; 0 = manual until stowed
static unsigned g_weaponStateOverrideVk = 0x13; // Pause/Break manually corrects the mod's weapon-state belief
static unsigned g_stowVk = 0x54;                  // T draw/stow toggle
static unsigned g_unsheathVk[8];
static int g_unsheathN = 0;
static bool g_combatBinary = true;
static float g_combatStart = 0.30f;

// Other features
static bool g_enabled = true, g_wheelEnabled = true, g_baseSpeedEnabled = true, g_jogToWalk = true;
static bool g_debugKeys = false;
static std::atomic<bool> g_runLatch{false};
static std::atomic<ULONGLONG> g_lastShift{0};
static FILE* g_f = nullptr;
static bool g_movementVectorHookInstalled = false;
static BYTE* g_movementVectorSite = nullptr;
static BYTE* g_movementVectorTrampoline = nullptr;
static BYTE g_movementVectorOriginal[16]{};

static void Log(const char* fmt, ...);
static bool InCombat();
static bool UseCombatBinary();
static float ActiveLimit();
static void RefreshKeyboardVectorScale();

static void Log(const char* fmt, ...) {
    if (!g_log || !g_f) return;
    va_list a; va_start(a, fmt); vfprintf(g_f, fmt, a); va_end(a);
    fputc('\n', g_f); fflush(g_f);
}

// ---------------- weapon / combat state ----------------
static void DrawWeapon(const char* why) {
    if (g_weaponOut.exchange(true)) return;
    Log("weapon UNSHEATHED (%s); combat remains %s", why, g_combatActive.load() ? "active" : "inactive");
    g_dirty.store(true);
    RefreshKeyboardVectorScale();
}

static void StowWeapon(const char* why) {
    const bool wasOut = g_weaponOut.exchange(false);
    const bool wasCombat = g_combatActive.exchange(false);
    g_lastCombat.store(0);
    if (wasOut || wasCombat) {
        Log("weapon SHEATHED (%s); combat cleared", why);
        g_dirty.store(true);
        RefreshKeyboardVectorScale();
    }
}

static bool InCombat() {
    if (g_combatActive.load(std::memory_order_relaxed) && g_combatHoldMs != 0) {
        const ULONGLONG last = g_lastCombat.load(std::memory_order_relaxed);
        if (last && GetTickCount64() - last > g_combatHoldMs) {
            if (g_combatActive.exchange(false)) {
                Log("combat activity expired after %lu ms; weapon remains %s",
                    g_combatHoldMs, g_weaponOut.load() ? "UNSHEATHED" : "SHEATHED");
                g_dirty.store(true);
            }
        }
    }
    return g_combatActive.load(std::memory_order_relaxed);
}

static void MarkCombat() {
    if (!g_combatDetect) return;
    if (!g_weaponOut.load()) DrawWeapon("attack/guard input");
    g_lastCombat.store(GetTickCount64(), std::memory_order_relaxed);
    const bool wasActive = g_combatActive.exchange(true);
    if (!wasActive) {
        g_combatRun.store(g_runLatch.load());
        Log("active COMBAT detected; initial gait=%s", g_combatRun.load() ? "RUN" : "WALK");
    }
    g_dirty.store(true);
    RefreshKeyboardVectorScale();
}

static bool UseCombatBinary() { return g_combatBinary && InCombat(); }
static float ActiveLimit() {
    return UseCombatBinary()
        ? (g_combatRun.load(std::memory_order_relaxed) ? 1.0f : g_combatLimit.load(std::memory_order_relaxed))
        : g_limit.load(std::memory_order_relaxed);
}

// ---------------- keyboard movement-vector scale ----------------
static void SetKeyboardVectorScale(float value) {
    value = std::max(0.0f, std::min(1.0f, value));
    g_keyboardVectorScale.store(value, std::memory_order_relaxed);
    LONG bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "32-bit float required");
    std::memcpy(&bits, &value, sizeof(bits));
    InterlockedExchange(&g_keyboardVectorScaleBits, bits);
}

static bool KeyboardMovementHeld() {
    return g_key[0].load(std::memory_order_relaxed) || g_key[1].load(std::memory_order_relaxed) ||
           g_key[2].load(std::memory_order_relaxed) || g_key[3].load(std::memory_order_relaxed);
}

static void RefreshKeyboardVectorScale() {
    const float factor = (g_keyboardVectorEnabled && g_keyboardScaleEnabled.load() && KeyboardMovementHeld())
        ? ActiveLimit() : 1.0f;
    SetKeyboardVectorScale(factor);
}

// ---------------- speed adjustment ----------------
static void AdjustLimit(float delta) {
    if (UseCombatBinary()) {
        if (GetAsyncKeyState(VK_MENU) & 0x8000) { // Alt + modifier + wheel: tune combat walk multiplier
            float v = std::min(1.0f, std::max(g_min,
                std::round((g_combatLimit.load() + delta) * 100.0f) / 100.0f));
            g_combatLimit.store(v);
            Log("combat walk multiplier = %.2f", v);
        } else {
            const bool run = delta > 0;
            if (g_combatRun.exchange(run) != run)
                Log("combat gait -> %s", run ? "RUN" : "WALK");
        }
    } else {
        float v = std::min(1.0f, std::max(g_min,
            std::round((g_limit.load() + delta) * 100.0f) / 100.0f));
        g_limit.store(v);
        Log("movement multiplier = %.2f", v);
    }
    g_dirty.store(true);
    RefreshKeyboardVectorScale();
}

// ---------------- game-side movement vector hook ----------------
static BYTE* FindUniqueMovementVectorSite(HMODULE module, int* matchCount) {
    if (matchCount) *matchCount = 0;
    if (!module) return nullptr;
    BYTE* base = reinterpret_cast<BYTE*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return nullptr;

    // Exact fixed bytes from CT entry 45; only the relative CALL displacement is wildcarded.
    const BYTE signature[16] = {
        0x48, 0x8B, 0xCF, 0xE8, 0, 0, 0, 0,
        0x48, 0x89, 0x5F, 0x40, 0x41, 0x0F, 0x28, 0xC3
    };
    const char mask[17] = "xxxx????xxxxxxxx";
    BYTE* found = nullptr;
    int count = 0;
    IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
    for (WORD sectionIndex = 0; sectionIndex < nt->FileHeader.NumberOfSections; ++sectionIndex) {
        const auto& section = sections[sectionIndex];
        if (!(section.Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const DWORD rva = section.VirtualAddress;
        if (rva >= nt->OptionalHeader.SizeOfImage) continue;
        size_t span = (size_t)std::max(section.Misc.VirtualSize, section.SizeOfRawData);
        span = std::min(span, (size_t)nt->OptionalHeader.SizeOfImage - rva);
        if (span < sizeof(signature)) continue;
        BYTE* begin = base + rva;
        for (size_t offset = 0; offset <= span - sizeof(signature); ++offset) {
            bool match = true;
            for (size_t j = 0; j < sizeof(signature); ++j) {
                if (mask[j] == 'x' && begin[offset + j] != signature[j]) { match = false; break; }
            }
            if (match) { found = begin + offset; ++count; }
        }
    }
    if (matchCount) *matchCount = count;
    return count == 1 ? found : nullptr;
}

static bool InstallMovementVectorHook() {
    if (!g_keyboardVectorEnabled) {
        Log("native keyboard movement-vector scaling disabled by INI");
        return false;
    }
    HMODULE game = GetModuleHandleW(L"CrimsonDesert.exe");
    if (!game) {
        Log("movement-vector hook FAILED: CrimsonDesert.exe module not found");
        return false;
    }
    int matches = 0;
    BYTE* site = FindUniqueMovementVectorSite(game, &matches);
    if (!site) {
        Log("movement-vector hook FAILED: CT entry 45 signature found %d times (need exactly 1)", matches);
        return false;
    }

    // Resolve the game's original relative CALL target.
    std::int32_t relativeCall = 0;
    std::memcpy(&relativeCall, site + 4, sizeof(relativeCall));
    BYTE* base = reinterpret_cast<BYTE*>(game);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const std::uintptr_t callAddress = (std::uintptr_t)site + 8 + (std::intptr_t)relativeCall;
    const std::uintptr_t imageStart = (std::uintptr_t)base;
    if (callAddress < imageStart || callAddress >= imageStart + nt->OptionalHeader.SizeOfImage) {
        Log("movement-vector hook FAILED: resolved movement routine is outside the game image");
        return false;
    }

    BYTE* trampoline = (BYTE*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!trampoline) {
        Log("movement-vector hook FAILED: VirtualAlloc error=%lu", GetLastError());
        return false;
    }

    BYTE code[160]{};
    size_t used = 0;
    auto emit = [&](const BYTE* bytes, size_t length) {
        std::memcpy(code + used, bytes, length);
        used += length;
    };
    auto emit64 = [&](std::uint64_t value) {
        std::memcpy(code + used, &value, sizeof(value));
        used += sizeof(value);
    };

    // Replay original: mov rcx,rdi; call original movement calculation.
    const BYTE movRcxRdi[] = { 0x48, 0x8B, 0xCF };
    const BYTE absoluteCall[] = { 0xFF, 0x15, 0x02, 0x00, 0x00, 0x00, 0xEB, 0x08 };
    emit(movRcxRdi, sizeof(movRcxRdi));
    emit(absoluteCall, sizeof(absoluteCall));
    emit64((std::uint64_t)callAddress);

    // Replay original vector store, then multiply both float components by the current keyboard scale.
    // RAX is preserved; the original following instruction overwrites XMM0 with XMM11.
    const BYTE storeVector[] = { 0x48, 0x89, 0x5F, 0x40, 0x50, 0x48, 0xB8 };
    emit(storeVector, sizeof(storeVector));
    emit64((std::uint64_t)(std::uintptr_t)&g_keyboardVectorScaleBits);
    const BYTE scaleX[] = {
        0xF3, 0x0F, 0x10, 0x47, 0x40, // movss xmm0,[rdi+40]
        0xF3, 0x0F, 0x59, 0x00,       // mulss xmm0,[rax]
        0xF3, 0x0F, 0x11, 0x47, 0x40  // movss [rdi+40],xmm0
    };
    const BYTE scaleY[] = {
        0xF3, 0x0F, 0x10, 0x47, 0x44, // movss xmm0,[rdi+44]
        0xF3, 0x0F, 0x59, 0x00,       // mulss xmm0,[rax]
        0xF3, 0x0F, 0x11, 0x47, 0x44  // movss [rdi+44],xmm0
    };
    const BYTE restoreAndMove[] = { 0x58, 0x41, 0x0F, 0x28, 0xC3 }; // pop rax; movaps xmm0,xmm11
    emit(scaleX, sizeof(scaleX));
    emit(scaleY, sizeof(scaleY));
    emit(restoreAndMove, sizeof(restoreAndMove));

    // Absolute indirect jump to the instruction immediately after the 16-byte overwritten sequence.
    const BYTE absoluteJump[] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
    emit(absoluteJump, sizeof(absoluteJump));
    emit64((std::uint64_t)(std::uintptr_t)(site + 16));

    std::memcpy(trampoline, code, used);
    DWORD oldTrampolineProtect = 0;
    VirtualProtect(trampoline, 4096, PAGE_EXECUTE_READ, &oldTrampolineProtect);
    FlushInstructionCache(GetCurrentProcess(), trampoline, used);

    // The overwritten sequence is exactly 16 bytes: 3 + 5 + 4 + 4.
    BYTE patch[16] = { 0xFF, 0x25, 0, 0, 0, 0 };
    const std::uint64_t trampolineAddress = (std::uint64_t)(std::uintptr_t)trampoline;
    std::memcpy(patch + 6, &trampolineAddress, sizeof(trampolineAddress));
    patch[14] = 0x90;
    patch[15] = 0x90;

    DWORD oldProtect = 0;
    if (!VirtualProtect(site, sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtect)) {
        Log("movement-vector hook FAILED: VirtualProtect error=%lu", GetLastError());
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(g_movementVectorOriginal, site, sizeof(g_movementVectorOriginal));
    std::memcpy(site, patch, sizeof(patch));
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(patch));
    DWORD ignored = 0;
    VirtualProtect(site, sizeof(patch), oldProtect, &ignored);

    g_movementVectorSite = site;
    g_movementVectorTrampoline = trampoline;
    g_movementVectorHookInstalled = true;
    Log("movement-vector hook installed at CrimsonDesert.exe+%llX; original calculator +%llX",
        (unsigned long long)(site - base), (unsigned long long)(callAddress - imageStart));
    return true;
}

// ---------------- mouse + keyboard hooks ----------------
static bool GameFocused() {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
}

static DWORD WINAPI ShiftTapThread(LPVOID) {
    INPUT input[1]{};
    input[0].type = INPUT_KEYBOARD;
    input[0].ki.wVk = VK_LSHIFT;
    input[0].ki.wScan = 0x2A;
    input[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    SendInput(1, input, sizeof(INPUT));
    Sleep(45);
    input[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, input, sizeof(INPUT));
    return 0;
}

static LRESULT CALLBACK MouseProc(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION && (message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN) && GameFocused())
        MarkCombat();

    if (g_wheelEnabled && code == HC_ACTION &&
        (message == WM_MOUSEWHEEL || message == WM_MBUTTONDOWN || message == WM_MBUTTONUP) &&
        GameFocused() && (g_modVk == 0 || (GetAsyncKeyState((int)g_modVk) & 0x8000))) {
        auto* mouse = (MSLLHOOKSTRUCT*)data;
        if (message == WM_MOUSEWHEEL) {
            const short delta = (short)((mouse->mouseData >> 16) & 0xFFFF);
            if (delta < 0 && g_jogToWalk && !UseCombatBinary() && g_runLatch.load() &&
                GetTickCount64() - g_lastShift.load() > 400) {
                g_runLatch.store(false);
                CreateThread(nullptr, 0, ShiftTapThread, nullptr, 0, nullptr);
                Log("scroll down while running -> Shift tap (run -> walk)");
            }
            AdjustLimit((delta / (float)WHEEL_DELTA) * g_step);
            return 1;
        }
        if (message == WM_MBUTTONDOWN) {
            if (UseCombatBinary()) g_combatRun.store(false);
            else g_limit.store(1.0f);
            g_dirty.store(true);
            RefreshKeyboardVectorScale();
        }
        return 1;
    }
    return CallNextHookEx(nullptr, code, message, data);
}

static bool g_tHeld = false;
static int WasdIndex(DWORD vk) {
    return vk == 'W' ? 0 : vk == 'A' ? 1 : vk == 'S' ? 2 : vk == 'D' ? 3 : -1;
}
static void ClearKeys() {
    for (auto& key : g_key) key.store(false);
    RefreshKeyboardVectorScale();
}

static LRESULT CALLBACK KeyProc(int code, WPARAM message, LPARAM data) {
    if (code == HC_ACTION) {
        auto* key = (KBDLLHOOKSTRUCT*)data;
        const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
        const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;

        if (down && g_toggleVk && key->vkCode == g_toggleVk && GameFocused()) {
            const bool enabled = !g_keyboardScaleEnabled.load();
            g_keyboardScaleEnabled.store(enabled);
            ClearKeys();
            Log("native keyboard vector scaling %s (WASD remains native)", enabled ? "ON" : "OFF");
        }

        if (down && GameFocused()) {
            if (g_debugKeys && WasdIndex(key->vkCode) < 0)
                Log("key down vk=0x%02X", key->vkCode);

            if (g_stowVk && key->vkCode == g_stowVk) {
                if (!g_tHeld) {
                    if (g_weaponOut.load()) StowWeapon("WeaponKey");
                    else DrawWeapon("WeaponKey");
                }
                g_tHeld = true;
            }
            for (int index = 0; index < g_unsheathN; ++index) {
                if (key->vkCode == g_unsheathVk[index]) {
                    Log("unsheathe key vk=0x%02X", key->vkCode);
                    DrawWeapon("UnsheathKeys");
                }
            }
            if (g_weaponStateOverrideVk && key->vkCode == g_weaponStateOverrideVk) {
                if (g_weaponOut.load()) StowWeapon("manual weapon-state override");
                else DrawWeapon("manual weapon-state override");
            }
        }

        // Track only real user Shift presses. No Shift is injected on weapon draw.
        if (!(key->flags & LLKHF_INJECTED) &&
            (key->vkCode == VK_SHIFT || key->vkCode == VK_LSHIFT || key->vkCode == VK_RSHIFT) && GameFocused()) {
            static bool shiftHeld = false;
            static ULONGLONG lastShiftPress = 0;
            if (down && !shiftHeld) {
                shiftHeld = true;
                const ULONGLONG now = GetTickCount64();
                if (g_runLatch.load() && now - lastShiftPress > 400) g_runLatch.store(false);
                else g_runLatch.store(true);
                lastShiftPress = now;
                g_lastShift.store(now);
            } else if (up) {
                shiftHeld = false;
            }
        }

        if (up && g_stowVk && key->vkCode == g_stowVk) g_tHeld = false;

        const int index = WasdIndex(key->vkCode);
        if (index >= 0 && (down || up)) {
            if (!GameFocused()) {
                ClearKeys();
            } else {
                g_key[index].store(down);
                RefreshKeyboardVectorScale();
            }
            // Intentionally do NOT swallow WASD: the game receives native keyboard input,
            // while the movement-vector hook applies the selected speed multiplier.
        }
    }
    return CallNextHookEx(nullptr, code, message, data);
}

static DWORD WINAPI InputThread(LPVOID) {
    HHOOK mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    Log(mouseHook ? "mouse hook installed" : "mouse hook FAILED");
    HHOOK keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, GetModuleHandleW(nullptr), 0);
    Log(keyboardHook ? "keyboard hook installed" : "keyboard hook FAILED");
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

// ---------------- title / config / hook setup ----------------
static BOOL CALLBACK FindWnd(HWND window, LPARAM parameter) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(window) && !GetWindow(window, GW_OWNER)) {
        *(HWND*)parameter = window;
        return FALSE;
    }
    return TRUE;
}

static void UpdateTitle() {
    static HWND window = nullptr;
    static wchar_t original[256] = L"";
    if (!window || !IsWindow(window)) {
        window = nullptr;
        EnumWindows(FindWnd, (LPARAM)&window);
        if (window) GetWindowTextW(window, original, 256);
    }
    if (!window) return;

    wchar_t buffer[384]{};
    if (InCombat() && g_combatBinary) {
        swprintf(buffer, 384, L"%ls   [COMBAT: %ls | walk %d%%]", original,
            g_combatRun.load() ? L"RUN" : L"WALK", (int)std::round(g_combatLimit.load() * 100.0f));
    } else {
        const wchar_t* weaponState = g_weaponOut.load() ? L"UNSHEATHED" : L"SHEATHED";
        swprintf(buffer, 384, L"%ls   [%ls | speed %d%%]", original, weaponState,
            (int)std::round(g_limit.load() * 100.0f));
    }
    SetWindowTextW(window, buffer);
}

static void LoadConfig() {
    const std::wstring ini = g_dir + L"ScrollSpeed.ini";
    wchar_t buffer[64];
    auto read = [&](const wchar_t* key, const wchar_t* fallback) {
        GetPrivateProfileStringW(L"Settings", key, fallback, buffer, 64, ini.c_str());
        return std::wstring(buffer);
    };
    g_modVk = wcstoul(read(L"Modifier", L"11").c_str(), nullptr, 16);
    g_toggleVk = wcstoul(read(L"KeyboardToggle", L"91").c_str(), nullptr, 16);
    g_combatStart = (float)_wtof(read(L"CombatWalkStick", L"0.30").c_str());
    g_combatHoldMs = (DWORD)_wtoi(read(L"CombatHoldMs", L"5000").c_str());
    g_combatDetect = _wtoi(read(L"CombatDetect", L"1").c_str()) != 0;
    g_weaponStateOverrideVk = wcstoul(read(L"WeaponStateOverrideKey", L"13").c_str(), nullptr, 16);
    g_stowVk = wcstoul(read(L"WeaponKey", L"54").c_str(), nullptr, 16);
    {
        wchar_t list[128];
        GetPrivateProfileStringW(L"Settings", L"UnsheathKeys", L"", list, 128, ini.c_str());
        g_unsheathN = 0;
        for (wchar_t* p = list; *p && g_unsheathN < 8;) {
            g_unsheathVk[g_unsheathN++] = wcstoul(p, &p, 16);
            while (*p == L',' || *p == L' ') ++p;
        }
    }
    g_step = (float)_wtof(read(L"Step", L"0.05").c_str());
    g_min = (float)_wtof(read(L"Min", L"0.10").c_str());
    g_start = (float)_wtof(read(L"Start", L"0.80").c_str());
    g_keyboardVectorEnabled = _wtoi(read(L"KeyboardVector", L"1").c_str()) != 0;
    g_showTitle = _wtoi(read(L"ShowInTitle", L"1").c_str()) != 0;
    g_log = _wtoi(read(L"DebugLog", L"1").c_str()) != 0;
    g_enabled = _wtoi(read(L"Enabled", L"1").c_str()) != 0;
    g_wheelEnabled = _wtoi(read(L"WheelAdjust", L"1").c_str()) != 0;
    g_baseSpeedEnabled = _wtoi(read(L"BaseSpeed", L"1").c_str()) != 0;
    g_combatBinary = _wtoi(read(L"CombatBinary", L"1").c_str()) != 0;
    g_jogToWalk = _wtoi(read(L"JogToWalk", L"1").c_str()) != 0;
    g_debugKeys = _wtoi(read(L"DebugKeys", L"0").c_str()) != 0;
}

static DWORD WINAPI Init(LPVOID moduleHandle) {
    CreateMutexW(nullptr, TRUE, L"Local\\ScrollSpeedKeyboardMouseOnly");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    wchar_t path[MAX_PATH];
    GetModuleFileNameW((HMODULE)moduleHandle, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    LoadConfig();
    g_f = _wfopen((g_dir + L"ScrollSpeed.log").c_str(), L"w");
    Log("ScrollSpeed keyboard/mouse-only starting (enabled=%d wheel=%d keyboardVector=%d combatBinary=%d start=%.2f)",
        (int)g_enabled, (int)g_wheelEnabled, (int)g_keyboardVectorEnabled, (int)g_combatBinary, g_start);
    if (!g_enabled) {
        Log("Enabled=0 -> mod does nothing");
        return 0;
    }

    if (!g_baseSpeedEnabled) g_start = 1.0f;
    g_limit.store(std::min(1.0f, std::max(g_min, g_start)));
    g_combatLimit.store(std::min(1.0f, std::max(g_min, g_combatStart)));
    RefreshKeyboardVectorScale();
    CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);

    // Install only the game-side movement-vector hook; this build has no controller hooks.
    InstallMovementVectorHook();

    ULONGLONG lastStats = GetTickCount64();
    bool previousCombat = false;
    bool previousWeaponOut = false;

    for (;;) {
        const bool currentCombat = InCombat();
        const bool currentWeaponOut = g_weaponOut.load();
        if (currentCombat != previousCombat || currentWeaponOut != previousWeaponOut) {
            Log("state -> %s / %s", currentWeaponOut ? "UNSHEATHED" : "SHEATHED",
                currentCombat ? "COMBAT" : "NOT-COMBAT");
            previousCombat = currentCombat;
            previousWeaponOut = currentWeaponOut;
            RefreshKeyboardVectorScale();
            g_dirty.store(true);
        }

        if (g_dirty.exchange(false) && g_showTitle) UpdateTitle();

        if (GetTickCount64() - lastStats > 10000) {
            Log("[10s] vectorHook=%d vectorScale=%.2f WASD=%d%d%d%d weaponOut=%d combat=%d",
                (int)g_movementVectorHookInstalled, g_keyboardVectorScale.load(),
                (int)g_key[0].load(), (int)g_key[1].load(), (int)g_key[2].load(), (int)g_key[3].load(),
                (int)g_weaponOut.load(), (int)InCombat());
            lastStats = GetTickCount64();
        }
        Sleep(200);
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, Init, module, 0, nullptr);
    }
    return TRUE;
}
