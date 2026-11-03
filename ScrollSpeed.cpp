// ScrollSpeed v6 - Crimson Desert ASI plugin
// Mouse wheel (Ctrl+wheel by default) sets a "stick limit": the left-stick values the game reads through
// XInput are scaled down before the game sees them, exactly like pushing the stick less far.
//
// v6 adds KEYBOARD support: when no controller is connected, the plugin presents a virtual XInput pad
// to the game and drives its left stick from W/A/S/D (WASD are hidden from the game while the virtual pad
// is live, so the game only sees one movement source). The wheel limit then acts as "how far the stick is pushed".
// A real controller, when connected, always takes priority and keeps working exactly as in v5.
//
//   - Default limit (Start) is 0.80 = 20% below a fully pushed stick, which also keeps the stick under the
//     game's jog/auto-run threshold, so entering combat does not push you into a run.
//   - Cannot go FASTER than a fully pushed stick (that is the game's own maximum for this input path).
//   - Does not touch game memory or game speed data.
// Controller alternative: hold Back/View and press D-Pad Up/Down (those presses are hidden from the game).
// Ctrl + middle mouse click = reset to full range. Scroll Lock = turn keyboard-stick emulation on/off
// (use it when you need to type in a text box).
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
struct XVib { WORD wLeftMotorSpeed, wRightMotorSpeed; };
struct XCaps { BYTE Type, SubType; WORD Flags; XPad Gamepad; XVib Vibration; };
typedef DWORD(WINAPI* GetState_t)(DWORD, XState*);
typedef DWORD(WINAPI* GetCaps_t)(DWORD, DWORD, XCaps*);
typedef DWORD(WINAPI* SetState_t)(DWORD, XVib*);
static const WORD BTN_UP = 0x0001, BTN_DOWN = 0x0002, BTN_BACK = 0x0020;
#ifndef ERROR_DEVICE_NOT_CONNECTED
#define ERROR_DEVICE_NOT_CONNECTED 1167L
#endif

// ---------------- config ----------------
static unsigned g_modVk = 0x11;               // Ctrl; 0 = wheel always adjusts
static unsigned g_toggleVk = 0x91;            // Scroll Lock toggles keyboard-stick emulation; 0 = none
static float g_step = 0.05f, g_min = 0.10f, g_start = 0.80f;
static float g_deadZone = 7849.0f;            // XInput default left-stick dead zone
static bool g_padAdjust = true, g_showTitle = true, g_log = true, g_kbMode = true;
static std::wstring g_dir;

static std::atomic<float> g_limit{1.0f};      // 1.0 = full stick range, g_min = barely moving
static std::atomic<bool> g_dirty{false};
static std::atomic<bool> g_kbEnabled{true};
// Combat handling: the game remaps stick tilt to a faster gait once the weapon is out, so a separate limit is
// applied while "in combat". Combat is inferred from attack/guard input (LMB, RMB, pad X/Y/RB/RT) and stays
// active for CombatHoldMs after the last such input; CombatKey force-toggles it (weapon drawn, no fighting).
static std::atomic<float> g_combatLimit{0.60f};
static std::atomic<ULONGLONG> g_lastCombat{0};
static bool g_combatDetect = true;
static DWORD g_combatHoldMs = 10000;
static unsigned g_combatKeyVk = 0x13;         // Pause/Break
static float g_combatStart = 0.60f;
// v6.5: "in combat" = weapon out. T toggles it (it is a draw/stow toggle), an attack click draws it if it was away,
// Pause/Break fixes the mod's idea of it by hand, and CombatHoldMs>0 optionally expires it after that long without attacks.
static std::atomic<bool> g_weaponOut{false};
static std::atomic<bool> g_combatRun{false};       // combat has only two gaits: false = walk, true = run
static bool g_combatBinary = true;
static bool InCombat() {
    if (g_weaponOut.load() && g_combatHoldMs && GetTickCount64() - g_lastCombat.load() > g_combatHoldMs) g_weaponOut.store(false);
    return g_weaponOut.load();
}
// Gait lock: if the stick is held while the weapon comes out, the game flips a walk into a run, whereas a
// fresh stick press after unsheathing walks. So on the transition we release the stick for RepressMs (after
// RepressDelayMs) so the game re-reads it as a fresh press. Skipped while a run is latched (Shift / pad A),
// since a run should stay a run.
// v6.3: the release is now counted in game polls (default 1, ~25 ms) instead of milliseconds, so it is too short
// to be felt as a pause but still gives the game one "stick at rest" sample.
static std::atomic<ULONGLONG> g_repressAt{0};
static std::atomic<int> g_repressLeft{0};
static std::atomic<bool> g_runLatch{false};
static std::atomic<ULONGLONG> g_lastShift{0};
static bool g_repress = true; static DWORD g_repressDelayMs = 0; static int g_repressPolls = 1;
// feature switches (ScrollSpeed.ini)
static bool g_enabled = true, g_wheelEnabled = true, g_baseSpeedEnabled = true, g_jogToWalk = true;
static unsigned g_unsheathVk[8]; static int g_unsheathN = 0; static bool g_debugKeys = false; static unsigned g_stowVk = 0x54;
static void ArmRepress() {
    if (!g_repress || g_runLatch.load()) return;
    g_repressAt.store(GetTickCount64() + g_repressDelayMs); g_repressLeft.store(g_repressPolls);
}
// Called once per served stick sample; true = send a centred stick for this sample.
static bool Repressing() {
    if (GetTickCount64() < g_repressAt.load()) return false;
    int l = g_repressLeft.load(); if (l <= 0) return false;
    g_repressLeft.store(l - 1); return true;
}
static DWORD g_drawDelayMs = 80;
static int g_gaitMethod = 2; static DWORD g_tapDelayMs = 250;    // GaitMethod bitmask: 1 = centre stick, 2 = Shift tap
static std::atomic<ULONGLONG> g_lastTransition{0};
static DWORD WINAPI DelayedTapThread(LPVOID);
static void Log(const char* fmt, ...);
// Weapon came out (drawing=true) or went away. Keeps a WALK a walk; a latched run is left alone.
static void OnWeaponTransition(bool drawing) {
    if (!g_repress) return;
    ULONGLONG n = GetTickCount64();
    if (n - g_lastTransition.load() < 400) return;                // one event can reach us via several triggers
    g_lastTransition.store(n);
    Log("weapon %s: runLatch=%d method=%d", drawing ? "DRAWN" : "STOWED", (int)g_runLatch.load(), g_gaitMethod);
    if (g_runLatch.load()) return;
    if (g_gaitMethod & 1) ArmRepress();
    if ((g_gaitMethod & 2) && drawing) CreateThread(nullptr, 0, DelayedTapThread, nullptr, 0, nullptr);
}
static void DrawWeapon(const char* why) {
    g_lastCombat.store(GetTickCount64());
    if (g_weaponOut.exchange(true)) return;
    if (g_combatBinary) {
        g_combatRun.store(false);                              // weapon out always starts at the LOW stick; one scroll up = MAX
        Log("weapon DRAWN (%s): combat mode, starting LOW (%.0f%% stick)", why, g_combatLimit.load() * 100.0f);
    } else OnWeaponTransition(true);
    g_dirty.store(true);
}
static void StowWeapon(const char* why) {
    if (!g_weaponOut.exchange(false)) return;
    g_lastCombat.store(0);
    if (g_combatBinary) Log("weapon STOWED (%s): back to analogue speed", why); else OnWeaponTransition(false);
    g_dirty.store(true);
}
static void MarkCombat() {
    if (!g_combatDetect) return;
    g_lastCombat.store(GetTickCount64());
    if (!g_weaponOut.load()) DrawWeapon("attack input");
}
// Combat gets its own limit only when CombatLimitEnabled=1; otherwise the same limit is used everywhere so the
// stick the game sees does not change when the weapon comes out.
static bool UseCombatBinary() { return g_combatBinary && InCombat(); }
// Stick fraction (0..1 of full deflection) the game should see. Combat = two raw values (LOW / MAX); exploring = analogue limit.
static float StickFactor(float lim);
static float ActiveFactor() {
    if (UseCombatBinary()) return g_combatRun.load() ? 1.0f : g_combatLimit.load(std::memory_order_relaxed);
    return StickFactor(g_limit.load(std::memory_order_relaxed));
}
static std::atomic<long> g_calls{0}, g_scaled{0}, g_virtual{0};
static std::atomic<ULONGLONG> g_realPadTick{0};     // last time the game got a real controller state
static std::atomic<ULONGLONG> g_virtServedTick{0};  // last time we served the virtual pad to the game
static std::atomic<bool> g_key[4];                  // W A S D as seen by the keyboard hook
static bool g_swallowed[4] = {false, false, false, false};
static GetState_t g_origA = nullptr, g_origEx = nullptr;
static GetCaps_t g_origCaps = nullptr;
static SetState_t g_origSet = nullptr;

static FILE* g_f = nullptr;
static void Log(const char* fmt, ...) {
    if (!g_log || !g_f) return;
    va_list a; va_start(a, fmt); vfprintf(g_f, fmt, a); va_end(a);
    fputc('\n', g_f); fflush(g_f);
}

// Scrolling adjusts whichever limit is currently active (normal or combat), so you can tune combat in place.
static void AdjustLimit(float delta) {
    if (UseCombatBinary()) {
        if (GetAsyncKeyState(VK_MENU) & 0x8000) {                 // Alt + modifier + wheel: tune the combat walk stick
            float lo = g_deadZone / 32767.0f + 0.005f;
            float v = std::min(1.0f, std::max(lo, std::round((g_combatLimit.load() + (delta > 0 ? 0.01f : -0.01f)) * 100.0f) / 100.0f));
            g_combatLimit.store(v); Log("combat LOW stick = %.2f", v);
        } else {                                                  // wheel up = run, wheel down = walk (no in-between)
            bool run = delta > 0; if (g_combatRun.exchange(run) != run) Log("combat -> %s", run ? "MAX" : "LOW");
        }
        g_dirty.store(true); return;
    }
    std::atomic<float>& L = g_limit;
    float v = L.load() + delta;
    v = std::min(1.0f, std::max(g_min, std::round(v * 100.0f) / 100.0f));
    L.store(v); g_dirty.store(true);
}

// Factor applied to a full-range stick so that "limit" behaves like pushing the stick that far
// (measured above the dead zone, which the game subtracts).
static float StickFactor(float lim) {
    return (g_deadZone + lim * (32767.0f - g_deadZone)) / 32767.0f;
}

// ---- v6.6: keyboard prompts ----
// The game picks button prompts (keyboard vs pad glyphs) from whichever device it last saw input on. The virtual pad
// makes the keyboard look like a pad, so these switches keep the game on KEYBOARD prompts:
//   KeyboardPulse   - while you hold WASD, send a harmless key event (F24) every PulseMs so the game keeps seeing
//                     keyboard activity alongside the virtual stick.
//   IdleDisconnect  - when you are NOT moving, report "no controller" to the game (it only sees the virtual pad while
//                     WASD is held, plus DisconnectDelayMs after). Off by default: it can make the game briefly
//                     treat the pad as unplugged, and the first step after standing still may be read from the
//                     keyboard directly.
static bool g_pulse = true, g_idleDisc = false; static DWORD g_pulseMs = 100, g_lingerMs = 700;
static std::atomic<ULONGLONG> g_lastMoveTick{0}, g_pollTick{0};
static std::atomic<long> g_pulses{0};
static const ULONG_PTR PULSE_TAG = 0x53535046;      // marks our own injected F24 events so the key hook ignores them
static bool AnyKey() { return g_key[0].load() || g_key[1].load() || g_key[2].load() || g_key[3].load(); }
static bool Moving() { return AnyKey() || GetTickCount64() - g_lastMoveTick.load() < g_lingerMs; }

static bool RealPadRecently() { return GetTickCount64() - g_realPadTick.load() < 1500; }
static bool VirtualLive() {
    if (!g_kbMode || !g_kbEnabled.load()) return false;
    ULONGLONG n = GetTickCount64();
    if (n - g_virtServedTick.load() < 500) return true;
    // idle-disconnect: the game is not being served while you stand still, so go by "the game is still polling slot 0"
    return g_idleDisc && n - g_pollTick.load() < 1500 && !RealPadRecently();
}
static bool VirtualAllowed() { return g_kbMode && g_kbEnabled.load() && !RealPadRecently(); }
static bool VirtualPresented() { return VirtualAllowed() && (!g_idleDisc || Moving()); }

// ---------------- XInput detours ----------------
static void ProcessReal(XState* s) {
    static WORD prev = 0;
    WORD b = s->Gamepad.wButtons;
    if (g_padAdjust && (b & BTN_BACK)) {                        // Back + DPad Up/Down adjusts the limit
        WORD pressed = b & ~prev;
        if (pressed & BTN_UP) AdjustLimit(+g_step);
        if (pressed & BTN_DOWN) AdjustLimit(-g_step);
        s->Gamepad.wButtons &= ~(BTN_UP | BTN_DOWN);            // hide from the game
    }
    if ((b & (0x4000 | 0x8000 | 0x0200)) || s->Gamepad.bRightTrigger > 40 || s->Gamepad.bLeftTrigger > 40) MarkCombat();   // X, Y, RB, triggers
    prev = b;

    {
        bool cb = UseCombatBinary(); float k = ActiveFactor();
        float x = (float)s->Gamepad.sThumbLX, y = (float)s->Gamepad.sThumbLY;
        float m = std::sqrt(x * x + y * y);
        if (m > g_deadZone && (cb || k < 0.999f)) {
            float t = cb ? k * 32767.0f / m : k;                // combat: fixed magnitude along the pushed direction
            s->Gamepad.sThumbLX = (SHORT)(x * t);
            s->Gamepad.sThumbLY = (SHORT)(y * t);
            g_scaled++;
        }
    }
    {   // run latch for pad: A held while moving = running; cleared after 200 ms without stick input
        static ULONGLONG lastMove = 0; float x = (float)s->Gamepad.sThumbLX, y = (float)s->Gamepad.sThumbLY;
        ULONGLONG n = GetTickCount64();
        if (std::sqrt(x * x + y * y) > g_deadZone) { lastMove = n; if (b & 0x1000) g_runLatch.store(true); }
        else if (n - lastMove > 200 && n - g_lastShift.load() > 1000) g_runLatch.store(false);
    }
    if (Repressing()) { s->Gamepad.sThumbLX = 0; s->Gamepad.sThumbLY = 0; }
}

// Fill a virtual pad state from WASD.
static void FillVirtual(XState* s) {
    static DWORD packet = 1; static SHORT lastX = 0, lastY = 0;
    float dx = (g_key[3].load() ? 1.0f : 0.0f) - (g_key[1].load() ? 1.0f : 0.0f);   // D - A
    float dy = (g_key[0].load() ? 1.0f : 0.0f) - (g_key[2].load() ? 1.0f : 0.0f);   // W - S
    float m = std::sqrt(dx * dx + dy * dy);
    SHORT X = 0, Y = 0;
    if (m > 0.0f) {
        float k = ActiveFactor() * 32767.0f / m;
        X = (SHORT)(dx * k); Y = (SHORT)(dy * k);
        g_scaled++;
    }
    {   static ULONGLONG lastMove = 0; ULONGLONG n = GetTickCount64();
        if (m > 0.0f) lastMove = n; else if (n - lastMove > 200 && n - g_lastShift.load() > 1000) g_runLatch.store(false); }   // stopped -> run ends
    if (Repressing()) { X = 0; Y = 0; }
    if (X != lastX || Y != lastY) { packet++; lastX = X; lastY = Y; }
    ZeroMemory(s, sizeof(*s));
    s->dwPacketNumber = packet;
    s->Gamepad.sThumbLX = X; s->Gamepad.sThumbLY = Y;
    g_virtServedTick.store(GetTickCount64());
    g_virtual++;
}

static DWORD DoGetState(GetState_t orig, DWORD i, XState* s) {
    g_calls++;
    DWORD r = orig(i, s);
    if (!s) return r;
    if (r == ERROR_SUCCESS) { g_realPadTick.store(GetTickCount64()); ProcessReal(s); return r; }
    if (i == 0) g_pollTick.store(GetTickCount64());
    if (i == 0 && VirtualPresented()) { FillVirtual(s); return ERROR_SUCCESS; }
    return r;
}
static DWORD WINAPI HookA(DWORD i, XState* s) { return DoGetState(g_origA, i, s); }
static DWORD WINAPI HookEx(DWORD i, XState* s) { return DoGetState(g_origEx, i, s); }

static DWORD WINAPI HookCaps(DWORD i, DWORD flags, XCaps* c) {
    DWORD r = g_origCaps(i, flags, c);
    if (r != ERROR_SUCCESS && i == 0 && c && VirtualPresented()) {
        ZeroMemory(c, sizeof(*c));
        c->Type = 0x00; c->SubType = 0x01;                      // XINPUT_DEVTYPE_GAMEPAD / SUBTYPE_GAMEPAD
        c->Gamepad.wButtons = 0xF3FF; c->Gamepad.bLeftTrigger = 255; c->Gamepad.bRightTrigger = 255;
        c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = (SHORT)0xFFC0;
        c->Vibration.wLeftMotorSpeed = c->Vibration.wRightMotorSpeed = 255;
        return ERROR_SUCCESS;
    }
    return r;
}
static DWORD WINAPI HookSet(DWORD i, XVib* v) {
    DWORD r = g_origSet(i, v);
    if (r != ERROR_SUCCESS && i == 0 && VirtualPresented()) return ERROR_SUCCESS;   // rumble to a virtual pad: ignore
    return r;
}

// ---------------- mouse + keyboard hooks (own thread, blocking message loop = no lag) ----------------
static bool GameFocused() {
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}
// One brief Shift tap (own thread so the hook returns immediately). The game's Shift is a run TOGGLE ("press again
// to slow down"), so a tap while running drops you back to a walk instead of waiting for a full stop.
static DWORD WINAPI ShiftTapThread(LPVOID) {
    INPUT in[1] = {}; in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_LSHIFT; in[0].ki.wScan = 0x2A; in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    SendInput(1, in, sizeof(INPUT));
    Sleep(45);
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, in, sizeof(INPUT));
    return 0;
}
// Re-sends the held-back weapon key after DrawDelayMs (as a short tap).
static DWORD WINAPI DelayedKeyThread(LPVOID) {
    Sleep(g_drawDelayMs);
    INPUT in[1] = {}; in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = (WORD)g_stowVk;
    in[0].ki.wScan = (WORD)MapVirtualKeyW(g_stowVk, MAPVK_VK_TO_VSC); in[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    SendInput(1, in, sizeof(INPUT));
    Sleep(45);
    in[0].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    SendInput(1, in, sizeof(INPUT));
    return 0;
}
static DWORD WINAPI DelayedTapThread(LPVOID) {
    Sleep(g_tapDelayMs);
    bool moving = g_key[0].load() || g_key[1].load() || g_key[2].load() || g_key[3].load();
    if (!moving) { Log("gait tap skipped: not moving"); return 0; }
    if (g_runLatch.load() || GetTickCount64() - g_lastShift.load() < g_tapDelayMs + 400) { Log("gait tap skipped: you pressed Shift / already running"); return 0; }
    Log("gait tap: Shift tap to cancel the run the game started on draw");
    ShiftTapThread(nullptr);
    return 0;
}
static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN) && GameFocused()) MarkCombat();
    if (g_wheelEnabled && code == HC_ACTION && (wp == WM_MOUSEWHEEL || wp == WM_MBUTTONDOWN || wp == WM_MBUTTONUP) &&
        GameFocused() && (g_modVk == 0 || (GetAsyncKeyState((int)g_modVk) & 0x8000))) {
        auto* ms = (MSLLHOOKSTRUCT*)lp;
        if (wp == WM_MOUSEWHEEL) {
            short d = (short)((ms->mouseData >> 16) & 0xFFFF);
            // scrolling down while jogging: end the run so the slower stick takes effect as a natural walk
            if (d < 0 && g_jogToWalk && !UseCombatBinary() && g_runLatch.load() && GetTickCount64() - g_lastShift.load() > 400) {
                g_runLatch.store(false);
                CreateThread(nullptr, 0, ShiftTapThread, nullptr, 0, nullptr);
                Log("scroll down while running -> Shift tap (run -> walk)");
            }
            AdjustLimit((d / (float)WHEEL_DELTA) * g_step);
            return 1;
        }
        if (wp == WM_MBUTTONDOWN) { if (UseCombatBinary()) g_combatRun.store(false); else g_limit.store(1.0f); g_dirty.store(true); }
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static bool g_tHeld = false, g_tSwallowed = false;
static int WasdIndex(DWORD vk) { return vk == 'W' ? 0 : vk == 'A' ? 1 : vk == 'S' ? 2 : vk == 'D' ? 3 : -1; }
static void ClearKeys() { for (auto& k : g_key) k.store(false); }

static LRESULT CALLBACK KeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        if (k->dwExtraInfo == PULSE_TAG) return CallNextHookEx(nullptr, code, wp, lp);   // our own prompt-pulse key
        bool down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN), up = (wp == WM_KEYUP || wp == WM_SYSKEYUP);
        if (down && g_toggleVk && k->vkCode == g_toggleVk && GameFocused()) {
            bool on = !g_kbEnabled.load(); g_kbEnabled.store(on); ClearKeys(); g_dirty.store(true);
            Log("keyboard-stick emulation %s", on ? "ON" : "OFF");
        }
        if (down && GameFocused()) {
            if (g_debugKeys && WasdIndex(k->vkCode) < 0) Log("key down vk=0x%02X", k->vkCode);
            for (int u = 0; u < g_unsheathN; u++)
                if (k->vkCode == g_unsheathVk[u]) { Log("unsheath key vk=0x%02X", k->vkCode); DrawWeapon("key"); }
        }
        if (down && g_combatKeyVk && k->vkCode == g_combatKeyVk && GameFocused()) {
            if (g_weaponOut.load()) StowWeapon("manual key"); else DrawWeapon("manual key");
        }
        // Track the game's run toggle from REAL Shift presses (our own injected tap is ignored):
        // first press = run, press again (not a quick double-press = sprint) = back to walk.
        if (!(k->flags & LLKHF_INJECTED) && (k->vkCode == VK_SHIFT || k->vkCode == VK_LSHIFT || k->vkCode == VK_RSHIFT) && GameFocused()) {
            static bool held = false; static ULONGLONG last = 0;
            if (down && !held) {
                held = true; ULONGLONG n = GetTickCount64();
                if (g_runLatch.load() && n - last > 400) g_runLatch.store(false); else g_runLatch.store(true);
                last = n; g_lastShift.store(n);
            } else if (up) held = false;
        }
        // T = draw/stow toggle. On DRAW the stick drops to LOW first and the key press is held back DrawDelayMs, so the
        // game never sees the weapon come out while the stick is still at the exploring strength.
        if (g_stowVk && k->vkCode == g_stowVk && !(k->flags & LLKHF_INJECTED) && GameFocused()) {
            if (down) {
                if (g_tHeld) { if (g_tSwallowed) return 1; }        // auto-repeat
                else {
                    g_tHeld = true;
                    if (g_weaponOut.load()) StowWeapon("key");
                    else {
                        DrawWeapon("key");
                        if (g_drawDelayMs && g_combatBinary) { g_tSwallowed = true; CreateThread(nullptr, 0, DelayedKeyThread, nullptr, 0, nullptr); return 1; }
                    }
                }
            } else if (up) { g_tHeld = false; if (g_tSwallowed) { g_tSwallowed = false; return 1; } }
        }
        int idx = WasdIndex(k->vkCode);
        if (idx >= 0 && (down || up)) {
            if (!GameFocused()) { ClearKeys(); }
            else if (down) {
                g_key[idx].store(true); g_lastMoveTick.store(GetTickCount64());
                if (g_swallowed[idx] || VirtualLive()) { g_swallowed[idx] = true; return 1; }
            } else {
                g_key[idx].store(false); g_lastMoveTick.store(GetTickCount64());
                if (g_swallowed[idx]) { g_swallowed[idx] = false; return 1; }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
// Keeps the game on keyboard prompts: while WASD is held (and the virtual pad is live) send an unbound key (F24) tap.
static DWORD WINAPI PulseThread(LPVOID) {
    for (;;) {
        Sleep(std::max<DWORD>(20, g_pulseMs));
        if (!g_pulse || !g_kbMode || !g_kbEnabled.load() || !AnyKey() || !VirtualLive() || !GameFocused()) continue;
        INPUT in[2] = {};
        for (auto& e : in) { e.type = INPUT_KEYBOARD; e.ki.wVk = VK_F24; e.ki.wScan = 0x76; e.ki.dwExtraInfo = PULSE_TAG; }
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT)); g_pulses++;
    }
}
static DWORD WINAPI InputThread(LPVOID) {
    HHOOK m = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    Log(m ? "mouse hook installed" : "mouse hook FAILED");
    {   // keyboard hook is always installed (run tracking, stow key); WASD capture itself needs KeyboardStick=1
        HHOOK k = SetWindowsHookExW(WH_KEYBOARD_LL, KeyProc, GetModuleHandleW(nullptr), 0);
        Log(k ? "keyboard hook installed" : "keyboard hook FAILED");
    }
    MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
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
    wchar_t buf[320];
    if (g_combatBinary && InCombat())
        swprintf(buf, 320, L"%ls   [COMBAT: %ls  |  low stick %d%%]", orig, g_combatRun.load() ? L"MAX" : L"LOW", (int)std::round(g_combatLimit.load() * 100.0f));
    else
        swprintf(buf, 320, L"%ls   [Stick %d%%]", orig, (int)std::round(g_limit.load() * 100.0f));
    SetWindowTextW(wnd, buf);
}

static void LoadConfig() {
    std::wstring ini = g_dir + L"ScrollSpeed.ini";
    wchar_t b[64];
    auto rd = [&](const wchar_t* k, const wchar_t* d) { GetPrivateProfileStringW(L"Settings", k, d, b, 64, ini.c_str()); return std::wstring(b); };
    g_modVk = wcstoul(rd(L"Modifier", L"11").c_str(), nullptr, 16);
    g_toggleVk = wcstoul(rd(L"KeyboardToggle", L"91").c_str(), nullptr, 16);
    g_combatStart = (float)_wtof(rd(L"CombatLowStick", L"0.25").c_str());
    g_drawDelayMs = (DWORD)_wtoi(rd(L"DrawDelayMs", L"80").c_str());
    g_combatHoldMs = (DWORD)_wtoi(rd(L"CombatHoldMs", L"0").c_str());
    g_combatDetect = _wtoi(rd(L"CombatDetect", L"1").c_str()) != 0;
    g_combatKeyVk = wcstoul(rd(L"CombatKey", L"13").c_str(), nullptr, 16);
    g_repress = _wtoi(rd(L"GaitLock", L"1").c_str()) != 0;
    g_gaitMethod = _wtoi(rd(L"GaitMethod", L"2").c_str());
    g_tapDelayMs = (DWORD)_wtoi(rd(L"GaitTapDelayMs", L"250").c_str());
    g_repressPolls = std::max(1, _wtoi(rd(L"RepressPolls", L"1").c_str()));
    g_enabled = _wtoi(rd(L"Enabled", L"1").c_str()) != 0;
    g_wheelEnabled = _wtoi(rd(L"WheelAdjust", L"1").c_str()) != 0;
    g_baseSpeedEnabled = _wtoi(rd(L"BaseSpeed", L"1").c_str()) != 0;
    g_combatBinary = _wtoi(rd(L"CombatBinary", L"1").c_str()) != 0;
    g_jogToWalk = _wtoi(rd(L"JogToWalk", L"1").c_str()) != 0;
    g_repressDelayMs = (DWORD)_wtoi(rd(L"RepressDelayMs", L"0").c_str());
    g_debugKeys = _wtoi(rd(L"DebugKeys", L"0").c_str()) != 0;
    g_stowVk = wcstoul(rd(L"WeaponKey", L"54").c_str(), nullptr, 16);
    {   wchar_t lb[128]; GetPrivateProfileStringW(L"Settings", L"UnsheathKeys", L"", lb, 128, (g_dir + L"ScrollSpeed.ini").c_str());
        g_unsheathN = 0; for (wchar_t* p = lb; *p && g_unsheathN < 8;) { g_unsheathVk[g_unsheathN++] = wcstoul(p, &p, 16); while (*p == L',' || *p == L' ') p++; } }
    g_step = (float)_wtof(rd(L"Step", L"0.05").c_str());
    g_min = (float)_wtof(rd(L"Min", L"0.10").c_str());
    g_start = (float)_wtof(rd(L"Start", L"0.80").c_str());
    g_deadZone = (float)_wtof(rd(L"DeadZone", L"7849").c_str());
    g_padAdjust = _wtoi(rd(L"PadAdjust", L"1").c_str()) != 0;
    g_kbMode = _wtoi(rd(L"KeyboardStick", L"1").c_str()) != 0;
    g_pulse = _wtoi(rd(L"KeyboardPulse", L"1").c_str()) != 0;
    g_pulseMs = (DWORD)_wtoi(rd(L"PulseMs", L"100").c_str());
    g_idleDisc = _wtoi(rd(L"IdleDisconnect", L"0").c_str()) != 0;
    g_lingerMs = (DWORD)_wtoi(rd(L"DisconnectDelayMs", L"700").c_str());
    g_showTitle = _wtoi(rd(L"ShowInTitle", L"1").c_str()) != 0;
    g_log = _wtoi(rd(L"DebugLog", L"1").c_str()) != 0;
}

static bool Hook(void* target, void* detour, void** orig, const char* name, const wchar_t* dll) {
    if (!target) return false;
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) { Log("hook %s FAILED", name); return false; }
    Log("hooked %s in %ls", name, dll); return true;
}

static DWORD WINAPI Init(LPVOID hMod) {
    CreateMutexW(nullptr, TRUE, L"Local\\ScrollSpeedV6");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;       // loaded twice -> second copy exits
    wchar_t path[MAX_PATH]; GetModuleFileNameW((HMODULE)hMod, path, MAX_PATH);
    g_dir = path; g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    LoadConfig();
    g_f = _wfopen((g_dir + L"ScrollSpeed.log").c_str(), L"w");
    Log("ScrollSpeed v6.6 starting (enabled=%d wheel=%d baseSpeed=%d keyboardStick=%d combatBinary=%d gaitLock=%d jogToWalk=%d start=%.2f)",
        (int)g_enabled, (int)g_wheelEnabled, (int)g_baseSpeedEnabled, (int)g_kbMode, (int)g_combatBinary, (int)g_repress, (int)g_jogToWalk, g_start);
    if (!g_enabled) { Log("Enabled=0 -> mod does nothing"); return 0; }          // master switch: no hooks at all
    if (!g_baseSpeedEnabled) g_start = 1.0f;                                    // BaseSpeed=0 -> start at the game's normal speed
    g_limit.store(std::min(1.0f, std::max(g_min, g_start)));
    g_combatLimit.store(std::min(1.0f, std::max(g_deadZone / 32767.0f + 0.005f, g_combatStart))); g_dirty.store(true);
    CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, PulseThread, nullptr, 0, nullptr);
    Log("prompt fix: pulse=%d (every %lu ms) idleDisconnect=%d (linger %lu ms)", (int)g_pulse, g_pulseMs, (int)g_idleDisc, g_lingerMs);

    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return 0; }
    const wchar_t* dlls[] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    bool hooked = false; ULONGLONG t0 = GetTickCount64(), lastStat = t0; long lastCalls = 0, lastScaled = 0, lastVirt = 0;
    for (;;) {
        if (!hooked && GetTickCount64() - t0 < 600000) {
            for (const wchar_t* d : dlls) {
                HMODULE xm = GetModuleHandleW(d);
                if (!xm) continue;
                void* a = (void*)GetProcAddress(xm, "XInputGetState");
                void* ex = (void*)GetProcAddress(xm, MAKEINTRESOURCEA(100));   // XInputGetStateEx (undocumented ordinal)
                if (Hook(a, (void*)HookA, (void**)&g_origA, "XInputGetState", d)) {
                    hooked = true;
                    if (ex && ex != a) Hook(ex, (void*)HookEx, (void**)&g_origEx, "XInputGetStateEx", d);
                    Hook((void*)GetProcAddress(xm, "XInputGetCapabilities"), (void*)HookCaps, (void**)&g_origCaps, "XInputGetCapabilities", d);
                    Hook((void*)GetProcAddress(xm, "XInputSetState"), (void*)HookSet, (void**)&g_origSet, "XInputSetState", d);
                    break;
                }
            }
        }
        static bool wasCombat = false; bool nowCombat = InCombat();
        if (nowCombat != wasCombat) { wasCombat = nowCombat; g_dirty.store(true); Log("combat state -> %s", nowCombat ? "COMBAT" : "exploring"); }
        if (g_dirty.exchange(false)) { Log("stick limit = %.2f, combat limit = %.2f", g_limit.load(), g_combatLimit.load()); if (g_showTitle) UpdateTitle(); }
        if (GetTickCount64() - lastStat > 10000) {          // diagnostics every 10 s
            long c = g_calls.load(), s = g_scaled.load(), v = g_virtual.load();
            Log("[10s] GetState calls=%ld, scaled=%ld, virtual-pad polls=%ld, prompt-pulses=%ld, limit=%.2f, hooked=%d, realPad=%d, kbEmu=%d, WASD=%d%d%d%d",
                c - lastCalls, s - lastScaled, v - lastVirt, g_pulses.load(), g_limit.load(), (int)hooked, (int)RealPadRecently(), (int)g_kbEnabled.load(),
                (int)g_key[0].load(), (int)g_key[1].load(), (int)g_key[2].load(), (int)g_key[3].load());
            lastCalls = c; lastScaled = s; lastVirt = v; lastStat = GetTickCount64();
        }
        Sleep(200);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); CreateThread(nullptr, 0, Init, h, 0, nullptr); }
    return TRUE;
}
