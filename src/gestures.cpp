#include "shell.h"
#include <cstdlib>

// Right-drag anywhere (desktop, menu bar, dock background or any normal app window):
//   drag left / right  -> next / previous desktop (Spaces)
//   drag up            -> Mission Control (Task View)
//   drag down          -> show desktop
// A right-click without a drag is replayed, so normal context menus still work.

enum { G_NEXT = 1, G_PREV, G_MISSION, G_DESKTOP, G_REPLAY };

namespace {
HHOOK s_hook = nullptr, s_kbdHook = nullptr;
DWORD s_mainThread = 0, s_hookThread = 0;
HANDLE s_thread = nullptr;
bool s_track = false, s_fired = false;
bool s_winDown = false, s_winOther = false, s_switching = false;
bool s_winSent = false;          // real Win key: we played a Win down to Windows for a Win+<key> shortcut
DWORD s_winVk = VK_LWIN;
bool s_fakeWin = false, s_fakeOther = false, s_fakeSent = false;   // remapped Win key (F24)
volatile DWORD s_lastHookEvent = 0;   // last time either hook saw any input (hook health check)
POINT s_start{};

// Where a right-drag may become a gesture. Games and other borderless full-screen
// windows are left alone so right-click aiming keeps working there.
bool DesktopUnder(POINT pt) {
    HWND h = WindowFromPoint(pt);
    HWND root = h ? GetAncestor(h, GA_ROOT) : nullptr;
    wchar_t cls[64] = {};
    if (root) GetClassNameW(root, cls, 64);
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW");
}

bool GestureSurface(POINT pt) {
    HWND h = WindowFromPoint(pt);
    HWND root = h ? GetAncestor(h, GA_ROOT) : nullptr;
    if (!root) return false;
    wchar_t cls[64];
    GetClassNameW(root, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return true;
    for (auto& mb : menubars) if (root == mb->ov.hwnd) return true;
    if (root == dock.ov.hwnd) return dock.hoverIdx < 0;
    if (cfg.gesturesEverywhere) {
        LONG st = GetWindowLongW(root, GWL_STYLE);
        LONG ex = GetWindowLongW(root, GWL_EXSTYLE);
        if ((st & WS_CAPTION) == WS_CAPTION && !(ex & WS_EX_TOOLWINDOW)) return true;   // normal titled app window
    }
    return false;
}

LRESULT CALLBACK Hook(int code, WPARAM wp, LPARAM lp) {
    s_lastHookEvent = GetTickCount();
    if (code == HC_ACTION) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        if (!(m->flags & LLMHF_INJECTED)) {
            if (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN) s_winOther = s_fakeOther = true;
            // a right-click on the desktop while a Liquid menu is open just closes that menu;
            // it must never fall through to Windows' own desktop menu
            if ((wp == WM_RBUTTONDOWN || wp == WM_RBUTTONUP) && GlassMenuOpen() && DesktopUnder(m->pt)) return 1;
            if (wp == WM_RBUTTONDOWN && !GlassMenuOpen() && GestureSurface(m->pt)) {
                s_track = true;
                s_fired = false;
                s_start = m->pt;
                return 1;
            }
            if (wp == WM_MOUSEMOVE && s_track && !s_fired) {
                int dx = m->pt.x - s_start.x, dy = m->pt.y - s_start.y, th = 110;
                int g = 0;
                if (std::abs(dx) > th && std::abs(dx) > std::abs(dy)) g = dx < 0 ? G_NEXT : G_PREV;
                else if (std::abs(dy) > th && std::abs(dy) > std::abs(dx)) g = dy < 0 ? G_MISSION : G_DESKTOP;
                if (g) { s_fired = true; PostThreadMessageW(s_mainThread, WM_LIQUID_GESTURE, g, MAKELPARAM(s_start.x, s_start.y)); }
            }
            if (wp == WM_RBUTTONUP && s_track) {
                s_track = false;
                if (!s_fired) PostThreadMessageW(s_mainThread, WM_LIQUID_GESTURE, G_REPLAY, MAKELPARAM(s_start.x, s_start.y));
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
// volume keys -> our HUD (swallowed, so the Windows volume flyout never shows)
bool s_swallowSpaceUp = false;

LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    s_lastHookEvent = GetTickCount();
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        bool injected = (k->flags & LLKHF_INJECTED) != 0;
        // Remapped Windows key (boot\remap-win-key.ps1 turns both Win keys into F24 at the driver
        // level, because this Windows build no longer lets any app stop Win+L). Liquid plays the
        // Win key back for every shortcut except Win+L, which opens the Liquid lock screen.
        if (!injected && k->vkCode == VK_F24) {
            if (down) {
                if (!s_fakeWin) { s_fakeWin = true; s_fakeOther = false; s_fakeSent = false; }
            } else {
                bool alone = s_fakeWin && !s_fakeOther;
                s_fakeWin = false;
                if (s_fakeSent) {
                    INPUT in{}; in.type = INPUT_KEYBOARD;
                    in.ki.wVk = VK_LWIN; in.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
                    SendInput(1, &in, sizeof(INPUT));
                    s_fakeSent = false;
                } else if (alone && !lock::IsLocked()) {
                    if (cfg.winKeySpotlight) PostThreadMessageW(s_mainThread, WM_LIQUID_SPOTKEY, 0, 0);
                    else {   // tap the real Win key: Start
                        INPUT in[2] = {};
                        for (auto& i : in) { i.type = INPUT_KEYBOARD; i.ki.wVk = VK_LWIN; i.ki.dwFlags = KEYEVENTF_EXTENDEDKEY; }
                        in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
                        SendInput(2, in, sizeof(INPUT));
                    }
                }
            }
            return 1;
        }
        if (!injected && down && s_fakeWin && !lock::IsLocked()) {
            s_fakeOther = true;
            DWORD vk = k->vkCode;
            bool modifier = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL ||
                            vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
            if (vk == 'L') {
                if (cfg.lockScreen) PostThreadMessageW(s_mainThread, WM_LIQUID_LOCKKEY, 0, 0);
                return 1;
            }
            if (vk == 'F' && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
                PostThreadMessageW(s_mainThread, WM_LIQUID_BORDERLESS, 0, 0);
                return 1;
            }
            if (!modifier && !s_fakeSent) {
                // first real key of a Win+<key> shortcut: press Win, then replay this key after it
                INPUT in[2] = {};
                in[0].type = in[1].type = INPUT_KEYBOARD;
                in[0].ki.wVk = VK_LWIN; in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
                in[1].ki.wVk = (WORD)vk; in[1].ki.wScan = (WORD)k->scanCode;
                in[1].ki.dwFlags = (k->flags & LLKHF_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0;
                SendInput(2, in, sizeof(INPUT));
                s_fakeSent = true;
                return 1;
            }
        }
        // locked: every key goes to the lock screen (Ctrl+Alt+Del can't be stopped by any app)
        if (lock::IsLocked() && !injected) {
            if (down) {
                BYTE ks[256] = {};
                if (GetAsyncKeyState(VK_SHIFT) & 0x8000) ks[VK_SHIFT] = 0x80;
                wchar_t buf[4] = {}, ch = 0;
                HKL layout = GetKeyboardLayout(0);
                if (ToUnicodeEx(k->vkCode, k->scanCode, ks, buf, 4, 4, layout) == 1) ch = buf[0];
                PostThreadMessageW(s_mainThread, WM_LIQUID_LOCKKEY, k->vkCode, ch);
            }
            return 1;
        }
        // Alt+Tab: Liquid's Mac-style app switcher instead of Windows'
        if (cfg.appSwitcher && !injected) {
            DWORD vk = k->vkCode;
            bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            if (vk == VK_TAB && alt && !(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
                if (down) {
                    PostThreadMessageW(s_mainThread, WM_LIQUID_SWITCH, (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 2 : 1, 0);
                    s_switching = true;
                }
                return 1;
            }
            if (s_switching) {
                if (down && vk == VK_ESCAPE) { s_switching = false; PostThreadMessageW(s_mainThread, WM_LIQUID_SWITCH, 4, 0); return 1; }
                if (down && (vk == VK_LEFT || vk == VK_RIGHT)) { PostThreadMessageW(s_mainThread, WM_LIQUID_SWITCH, vk == VK_LEFT ? 2 : 1, 0); return 1; }
                if (down && vk == 'Q') { PostThreadMessageW(s_mainThread, WM_LIQUID_SWITCH, 5, 0); return 1; }
                if (!down && (vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU)) {
                    s_switching = false;
                    PostThreadMessageW(s_mainThread, WM_LIQUID_SWITCH, 3, 0);
                    // a dummy key before the Alt release stops the app's menu bar from activating
                    INPUT in[3] = {};
                    for (auto& i : in) i.type = INPUT_KEYBOARD;
                    in[0].ki.wVk = 0xE8;
                    in[1].ki.wVk = 0xE8; in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                    in[2].ki.wVk = (WORD)vk; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
                    SendInput(3, in, sizeof(INPUT));
                    return 1;
                }
            }
        }
        // Windows key on its own opens Spotlight instead of Start; Win+<key> shortcuts still work.
        // (Used until boot\remap-win-key.ps1's F24 remap takes effect at the next restart; then
        // the F24 path above handles the key the same way.)
        // Windows never sees the real Win key: masking it with a dummy key while it was held used
        // to stop Start opening, but on this Windows build Start opened anyway, alongside
        // Spotlight. So the Win key is swallowed, and for a Win+<key> shortcut we play a Win down
        // to Windows just before that key, and the Win up when the real one comes up. (Injected
        // keys can't reach an admin window in front, so Win shortcuts don't work over one.)
        if (cfg.winKeySpotlight && !injected) {
            DWORD vk = k->vkCode;
            bool isWin = vk == VK_LWIN || vk == VK_RWIN;
            if (isWin && down) {
                if (!s_winDown) { s_winDown = true; s_winOther = false; s_winSent = false; s_winVk = vk; }
                return 1;
            }
            if (isWin && !down) {
                bool alone = s_winDown && !s_winOther;
                s_winDown = false;
                if (s_winSent) {
                    INPUT in{}; in.type = INPUT_KEYBOARD;
                    in.ki.wVk = (WORD)s_winVk; in.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_EXTENDEDKEY;
                    SendInput(1, &in, sizeof(INPUT));
                    s_winSent = false;
                } else if (alone && !lock::IsLocked()) {
                    PostThreadMessageW(s_mainThread, WM_LIQUID_SPOTKEY, 0, 0);
                }
                return 1;
            }
            if (down && s_winDown) {
                s_winOther = true;
                // Ctrl+Win+F: borderless full screen on / off (macOS full screen shortcut)
                if (vk == 'F' && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
                    PostThreadMessageW(s_mainThread, WM_LIQUID_BORDERLESS, 0, 0);
                    return 1;
                }
                // Win+L: the Liquid lock screen
                if (vk == 'L' && cfg.lockScreen) {
                    PostThreadMessageW(s_mainThread, WM_LIQUID_LOCKKEY, 0, 0);
                    return 1;
                }
                bool modifier = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL ||
                                vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU;
                if (!modifier && !s_winSent) {
                    INPUT in[2] = {};
                    in[0].type = in[1].type = INPUT_KEYBOARD;
                    in[0].ki.wVk = (WORD)s_winVk; in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
                    in[1].ki.wVk = (WORD)vk; in[1].ki.wScan = (WORD)k->scanCode;
                    in[1].ki.dwFlags = (k->flags & LLKHF_EXTENDED) ? KEYEVENTF_EXTENDEDKEY : 0;
                    SendInput(2, in, sizeof(INPUT));
                    s_winSent = true;
                    return 1;
                }
            }
        }
        // Spotlight: Alt+Space toggles (like Cmd+Space on a Mac)
        if (cfg.spotlight && !injected && k->vkCode == VK_SPACE) {
            if (down && (GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                !(GetAsyncKeyState(VK_LWIN) & 0x8000) && !(GetAsyncKeyState(VK_RWIN) & 0x8000)) {
                PostThreadMessageW(s_mainThread, WM_LIQUID_SPOTKEY, 0, 0);
                // a dummy key so letting go of Alt doesn't open the app's menu bar
                INPUT in[2] = {};
                in[0].type = in[1].type = INPUT_KEYBOARD;
                in[0].ki.wVk = in[1].ki.wVk = 0xE8;
                in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
                s_swallowSpaceUp = true;
                return 1;
            }
            if (!down && s_swallowSpaceUp) { s_swallowSpaceUp = false; return 1; }
        }
        // while Spotlight is open, typing goes to it
        if (spotlight::IsOpen() && !injected) {
            DWORD vk = k->vkCode;
            bool modifier = vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL || vk == VK_LCONTROL ||
                            vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN ||
                            vk == VK_RWIN || vk == VK_CAPITAL;
            bool winHeld = (GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000);
            bool altHeld = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            bool media = vk >= VK_VOLUME_MUTE && vk <= VK_LAUNCH_APP2;
            if (!modifier && !winHeld && !altHeld && !media) {
                if (down) {
                    BYTE ks[256] = {};
                    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) ks[VK_SHIFT] = 0x80;
                    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
                    wchar_t buf[4] = {};
                    wchar_t ch = 0;
                    if (!(GetAsyncKeyState(VK_CONTROL) & 0x8000)) {
                        HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
                        int n = ToUnicodeEx(vk, k->scanCode, ks, buf, 4, 4, layout);
                        if (n == 1) ch = buf[0];
                    }
                    PostThreadMessageW(s_mainThread, WM_LIQUID_SPOTKEY, vk, ch);
                }
                return 1;
            }
        }
        // volume shortcuts for keyboards without media keys: Ctrl+Alt+Up / Down, Ctrl+Alt+M mutes
        if (cfg.volumeHud && !injected && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
            (k->vkCode == VK_UP || k->vkCode == VK_DOWN || k->vkCode == 'M')) {
            if (down) PostThreadMessageW(s_mainThread, WM_LIQUID_VOLKEY,
                                         k->vkCode == VK_UP ? VK_VOLUME_UP : k->vkCode == VK_DOWN ? VK_VOLUME_DOWN : VK_VOLUME_MUTE, 0);
            return 1;
        }
        if (cfg.volumeHud && (k->vkCode == VK_VOLUME_UP || k->vkCode == VK_VOLUME_DOWN || k->vkCode == VK_VOLUME_MUTE)) {
            if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) PostThreadMessageW(s_mainThread, WM_LIQUID_VOLKEY, k->vkCode, 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
}  // namespace

namespace gestures {
// The hook lives on its own thread with its own message loop: if it shared the busy
// render thread, Windows would silently unhook it whenever a frame ran long.
static void InstallHooks() {
    if (s_hook) UnhookWindowsHookEx(s_hook);
    if (s_kbdHook) UnhookWindowsHookEx(s_kbdHook);
    s_hook = s_kbdHook = nullptr;
    if (cfg.gestures) s_hook = SetWindowsHookExW(WH_MOUSE_LL, Hook, GetModuleHandleW(nullptr), 0);
    if (cfg.volumeHud || cfg.spotlight || cfg.winKeySpotlight || cfg.lockScreen || cfg.appSwitcher) s_kbdHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    s_lastHookEvent = GetTickCount();
}

static DWORD WINAPI HookThread(void*) {
    InstallHooks();
    Log(L"gestures: right-drag hook %s, volume keys %s", s_hook ? L"on" : L"off", s_kbdHook ? L"on" : L"off");
    // Windows silently removes a low-level hook that ever answers too slowly, and then every
    // Win key press goes straight to Start. Every 2 s: if Windows saw input the hooks never
    // saw, they were dropped, so put them back.
    SetTimer(nullptr, 0, 2000, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message != WM_TIMER) continue;
        LASTINPUTINFO li{sizeof(li)};
        if (!GetLastInputInfo(&li)) continue;
        DWORD seen = s_lastHookEvent;
        if ((int)(li.dwTime - seen) > 1500 && (int)(GetTickCount() - li.dwTime) < 4000) {
            InstallHooks();
            Log(L"gestures: hooks had been dropped by Windows, reinstalled");
        }
    }
    if (s_hook) UnhookWindowsHookEx(s_hook);
    if (s_kbdHook) UnhookWindowsHookEx(s_kbdHook);
    s_hook = s_kbdHook = nullptr;
    return 0;
}

void Init() {
    if (!cfg.gestures && !cfg.volumeHud && !cfg.spotlight && !cfg.winKeySpotlight && !cfg.lockScreen) return;
    s_mainThread = GetCurrentThreadId();
    s_thread = CreateThread(nullptr, 0, HookThread, nullptr, 0, &s_hookThread);
    if (s_thread) SetThreadPriority(s_thread, THREAD_PRIORITY_TIME_CRITICAL);
}

void Shutdown() {
    if (!s_thread) return;
    PostThreadMessageW(s_hookThread, WM_QUIT, 0, 0);
    WaitForSingleObject(s_thread, 1000);
    CloseHandle(s_thread);
    s_thread = nullptr;
}

void Handle(WPARAM g, LPARAM lp) {
    POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    switch (g) {
        case G_NEXT: if (!spaces::Switch(mon, +1)) TapKeys({VK_CONTROL, VK_LWIN, VK_RIGHT}); break;
        case G_PREV: if (!spaces::Switch(mon, -1)) TapKeys({VK_CONTROL, VK_LWIN, VK_LEFT}); break;
        case G_MISSION: TapKeys({VK_LWIN, VK_TAB}); break;
        case G_DESKTOP: TapKeys({VK_LWIN, 'D'}); break;
        case G_REPLAY: {
            if (cfg.desktopMenu) {
                HWND h = WindowFromPoint(pt);
                HWND root = h ? GetAncestor(h, GA_ROOT) : nullptr;
                wchar_t cls[64] = {};
                if (root) GetClassNameW(root, cls, 64);
                if ((!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) && DesktopMenu(pt)) break;
            }
            INPUT in[2] = {};
            in[0].type = in[1].type = INPUT_MOUSE;
            in[0].mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
            in[1].mi.dwFlags = MOUSEEVENTF_RIGHTUP;
            SendInput(2, in, sizeof(INPUT));
            break;
        }
    }
}
}  // namespace gestures
