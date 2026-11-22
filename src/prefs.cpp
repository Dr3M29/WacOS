#define SECURITY_WIN32
#include "shell.h"
#include <shellscalingapi.h>
#include <shlobj.h>
#include <security.h>
#include <secext.h>
#include <sddl.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <share.h>
#include <algorithm>
#include <cwctype>
#include <map>

// System Settings (Apple menu > System Settings...): one glass sheet with macOS's sidebar of panes.
//   Liquid's own settings are real controls here: Desktop & Dock, Menu Bar, Appearance, Lock
//   Screen, Face ID & Password, Air Gestures, Notifications... (config.ini, written as you click).
//   Windows' settings live in the same panes as rows that open the matching Windows Settings page.
//   Face ID & Password and Air Gestures are the hand-built panes from before; every other
//   pane is a list of sections built by Build() and drawn by one renderer.
// Air Ring (Python, D:\air-ring) owns the webcam. It shares its status and, while this window
// shows the Air Gestures pane, its camera picture through Local\AirRingShared, and takes
// commands through the same memory (layout: LiquidLink in air_ring.py).
//
// Keyboard: Tab / Shift+Tab move between controls, Enter or Space presses, Esc closes.
// Mouse wheel scrolls the sidebar or the pane under the pointer. A click outside closes it.

namespace {
enum {
    H_CLOSE = 1, H_BACK, H_RELOAD, H_ACCOUNT,
    H_FACE_SETUP = 10, H_FACE_FORGET, H_AWAY_LOCK, H_AWAY_PAUSE, H_WAKE_LOCK, H_PIN, H_LOCK_NOW,
    H_AIR_START = 30, H_AIR_CAMERA, H_AIR_PINCH, H_AIR_SWIPES, H_AIR_TRAIN, H_AIR_CONFIG, H_AIR_LOG,
    H_SIDE0 = 200,                    // + index into the sidebar
    H_GEN0 = 1000,                    // + index into s_actions (rows of the built panes)
};

const float kW = 920, kH = 640, kMargin = 40, kSide = 232, kPadX = 28;

// ---------------------------------------------------------------- Air Ring link
const DWORD kAirSize = 512 * 1024, kAirStatus = 64, kAirCmd = 1088, kAirJpeg = 1600;

struct AirLink {
    HANDLE map = nullptr;
    BYTE* view = nullptr;
    HANDLE evPreview = nullptr, evCmd = nullptr;
    DWORD seq = 0;
    ULONGLONG beat = 0, lastTry = 0;
    std::map<std::wstring, std::wstring> st;
    ComPtr<ID2D1Bitmap1> frame;

    void Events() {
        if (!evPreview) evPreview = CreateEventW(nullptr, TRUE, FALSE, L"Local\\AirRingPreview");   // manual reset
        if (!evCmd) evCmd = CreateEventW(nullptr, FALSE, FALSE, L"Local\\AirRingCmd");
    }
    bool Connect() {
        if (view) return true;
        ULONGLONG now = GetTickCount64();
        if (now - lastTry < 1000) return false;
        lastTry = now;
        map = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, L"Local\\AirRingShared");
        if (!map) return false;
        view = (BYTE*)MapViewOfFile(map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, kAirSize);
        if (!view) { CloseHandle(map); map = nullptr; return false; }
        return true;
    }
    void Disconnect() {
        if (view) UnmapViewOfFile(view);
        if (map) CloseHandle(map);
        view = nullptr; map = nullptr;
        frame.Reset();
        st.clear();
    }
    // Air Ring is open (its single-instance mutex exists), whether or not it answers.
    static bool Process() {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"AirRingSingleInstance");
        if (m) CloseHandle(m);
        return m != nullptr;
    }
    bool Answering() const { return view && beat && GetTickCount64() - beat < 3000; }
    std::wstring Get(const wchar_t* k) const { auto it = st.find(k); return it == st.end() ? L"" : it->second; }
    bool On(const wchar_t* k) const { return Get(k) == L"1"; }

    // Copies a consistent snapshot: Air Ring makes seq odd while it writes. True if it changed.
    bool Poll(ID2D1DeviceContext* dc, bool wantFrame) {
        if (!Connect()) return false;
        volatile DWORD* sp = (volatile DWORD*)(view + 4);
        DWORD s1 = *sp;
        if (s1 == seq || (s1 & 1)) return false;
        MemoryBarrier();
        DWORD slen = *(volatile DWORD*)(view + 8);
        if (slen > 1000) slen = 1000;
        DWORD jlen = *(volatile DWORD*)(view + 12);
        std::string text((const char*)view + kAirStatus, slen);
        std::vector<BYTE> jpeg;
        if (wantFrame && jlen > 0 && jlen < kAirSize - kAirJpeg) jpeg.assign(view + kAirJpeg, view + kAirJpeg + jlen);
        MemoryBarrier();
        if (*sp != s1) return false;   // written meanwhile: take the next one
        seq = s1;
        beat = GetTickCount64();
        st.clear();
        int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), w.data(), n);
        for (size_t p = 0; p < w.size();) {
            size_t e = w.find(L'\n', p);
            if (e == std::wstring::npos) e = w.size();
            size_t eq = w.find(L'=', p);
            if (eq != std::wstring::npos && eq < e) st[w.substr(p, eq - p)] = w.substr(eq + 1, e - eq - 1);
            p = e + 1;
        }
        if (!jpeg.empty()) {
            if (auto b = DecodeImage(dc, jpeg, 480)) frame = b;
        } else if (!wantFrame) frame.Reset();
        return true;
    }
    void Send(const char* cmd) {
        if (!view || !evCmd) return;
        DWORD n = (DWORD)strlen(cmd);
        memcpy(view + kAirCmd + 8, cmd, n);
        *(volatile DWORD*)(view + kAirCmd + 4) = n;
        MemoryBarrier();
        volatile DWORD* cs = (volatile DWORD*)(view + kAirCmd);
        *cs = *cs + 1;                 // Air Ring runs a command once per new number
        SetEvent(evCmd);
        Log(L"prefs: sent Air Ring \"%S\"", cmd);
    }
    void Preview(bool on) {
        if (evPreview) { if (on) SetEvent(evPreview); else ResetEvent(evPreview); }
        if (!on) frame.Reset();
    }
} s_air;

// ---------------------------------------------------------------- window state
Overlay s_ov;
float s_s = 1;
bool s_open = false, s_modal = false, s_dirty = true, s_needReload = false;
int s_pane = prefs::General;
float s_anim = 0, s_animV = 0;
struct Hit { D2D1_RECT_F r; int id; bool enabled; };
std::vector<Hit> s_hits;
int s_hover = -1, s_pressed = -1, s_focus = -1;   // control ids; focus shows only after Tab
HHOOK s_mouse = nullptr, s_kbd = nullptr;
ComPtr<ID2D1SolidColorBrush> s_br;
ComPtr<IDWriteTextFormat> fTitle, fHead, fBody, fBodyC, fBodyR, fSmall, fWrap, fWrapSmall, fIcon, fSideIcon, fChevron, fSideName;
ULONGLONG s_lastPoll = 0;
float s_sideScroll = 0, s_sideMax = 0, s_scroll = 0, s_scrollMax = 0;
std::vector<std::function<void()>> s_actions;    // H_GEN0 + i
struct GSlider { int id; float l, r; std::function<void(float)> set; };
std::vector<GSlider> s_gsliders;
int s_gdrag = -1;
float s_gdragVal = 0;
bool s_rebuild = true;                // the pane's rows are read again (not every frame: some reads are slow)
ComPtr<ID2D1Bitmap1> s_avatar;
std::wstring s_userName;
bool s_userLoaded = false;

D2D1_RECT_F Box() {
    float m = kMargin * s_s, yoff = (1 - s_anim) * 12 * s_s;
    return D2D1::RectF(m, m + yoff, m + kW * s_s, m + yoff + kH * s_s);
}

bool Enabled(int id) {
    for (auto& h : s_hits) if (h.id == id) return h.enabled;
    return false;
}

void Activate(int id);
void SetPane(int pane);

void MoveFocus(bool back) {
    std::vector<int> ids;
    for (auto& h : s_hits) if (h.enabled && std::find(ids.begin(), ids.end(), h.id) == ids.end()) ids.push_back(h.id);
    if (ids.empty()) return;
    auto it = std::find(ids.begin(), ids.end(), s_focus);
    int i = it == ids.end() ? (back ? (int)ids.size() - 1 : 0)
                            : (int)((it - ids.begin() + (back ? ids.size() - 1 : 1)) % ids.size());
    s_focus = ids[i];
    s_dirty = true;
}

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_open && !s_modal && !GlassMenuOpen() && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        D2D1_RECT_F b = Box();
        RECT box{s_ov.x + (int)b.left, s_ov.y + (int)b.top, s_ov.x + (int)b.right, s_ov.y + (int)b.bottom};
        if (!PtInRect(&box, m->pt)) prefs::Close();
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_open && !s_modal) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (!alt && (k->vkCode == VK_ESCAPE || k->vkCode == VK_TAB || k->vkCode == VK_RETURN || k->vkCode == VK_SPACE)) {
            if (down) {
                if (k->vkCode == VK_ESCAPE) prefs::Close();
                else if (k->vkCode == VK_TAB) MoveFocus((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
                else if (s_focus >= 0 && Enabled(s_focus)) Activate(s_focus);
            }
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

std::wstring PoseName(const std::wstring& p) {
    if (p == L"pinch") return L"a pinch";
    if (p == L"open") return L"an open hand";
    if (p == L"fist") return L"a fist";
    if (p == L"point") return L"a pointing finger";
    if (p == L"peace") return L"a peace sign";
    if (p == L"three") return L"three fingers";
    if (p == L"rock") return L"a rock sign";
    if (p == L"thumbs_up") return L"a thumbs up";
    return L"a hand";
}

std::wstring EventName(const std::wstring& e) {
    if (e == L"swipe_right") return L"next song";
    if (e == L"swipe_left") return L"previous song";
    if (e == L"volume_on") return L"volume drag started";
    if (e == L"volume_off") return L"volume drag ended";
    std::wstring r = e;
    for (auto& c : r) if (c == L'_') c = L' ';
    return r;
}

// ---------------------------------------------------------------- the sidebar
struct SideItem { int pane; const wchar_t* name; const wchar_t* glyph; D2D1_COLOR_F color; bool gap; };
const D2D1_COLOR_F kBlue{0.04f, 0.52f, 1.0f, 1}, kGrey{0.53f, 0.53f, 0.56f, 1}, kGreen{0.2f, 0.74f, 0.33f, 1},
                   kRed{1.0f, 0.23f, 0.19f, 1}, kPink{1.0f, 0.18f, 0.33f, 1}, kIndigo{0.35f, 0.34f, 0.84f, 1},
                   kBlack{0.17f, 0.17f, 0.18f, 1}, kCyan{0.2f, 0.67f, 0.9f, 1},
                   kPurple{0.69f, 0.32f, 0.87f, 1};
// the order and grouping of macOS Tahoe's System Settings, with Windows' and Liquid's own extras
std::vector<SideItem> SideItems() {
    std::vector<SideItem> v = {
        {prefs::WiFi, L"Wi-Fi", L"\uE701", kBlue, false},
        {prefs::Bluetooth, L"Bluetooth", L"\uE702", kBlue, false},
        {prefs::Network, L"Network", L"\uE968", kBlue, false},
        {prefs::Energy, L"Energy", L"\uE945", kGreen, false},
        {prefs::General, L"General", L"\uE713", kGrey, true},
        {prefs::Accessibility, L"Accessibility", L"\uE776", kBlue, false},
        {prefs::Appearance, L"Appearance", L"\uE790", kBlack, false},
        {prefs::CtrlCenter, L"Control Center", L"\uE9E9", kGrey, false},
        {prefs::DesktopDock, L"Desktop & Dock", L"\uE8A9", kBlack, false},
        {prefs::Displays, L"Displays", L"\uE7F4", kBlue, false},
        {prefs::MenuBar, L"Menu Bar", L"\uE700", kBlack, false},
        {prefs::Spotlight, L"Spotlight", L"\uE721", kGrey, false},
        {prefs::Wallpaper, L"Wallpaper", L"\uE91B", kCyan, false},
        {prefs::Notifications, L"Notifications", L"\uEA8F", kRed, true},
        {prefs::Sound, L"Sound", L"\uE767", kPink, false},
        {prefs::Focus, L"Focus", L"\uE708", kIndigo, false},
        {prefs::LockScreen, L"Lock Screen", L"\uE72E", kBlack, true},
        {prefs::Privacy, L"Privacy & Security", L"\uEA18", kBlue, false},
        {prefs::LockFace, L"Face ID & Password", nullptr, kGreen, false},   // drawn with the Face ID glyph
        {prefs::Users, L"Users & Groups", L"\uE716", kBlue, false},
        {prefs::AirGestures, L"Air Gestures", L"\uE714", kPurple, true},
    };
    v.push_back({prefs::Gaming, L"Gaming", L"\uE7FC", kGreen, false});
    v.push_back({prefs::Apps, L"Apps", L"\uE71D", kBlue, true});
    v.push_back({prefs::Keyboard, L"Keyboard", L"\uE765", kGrey, false});
    v.push_back({prefs::Mouse, L"Mouse", L"\uE962", kGrey, false});
    v.push_back({prefs::Printers, L"Printers & Scanners", L"\uE749", kGrey, false});
    return v;
}

// The account at the top of the sidebar: your Windows name and account picture.
void LoadUser() {
    s_userLoaded = true;
    wchar_t name[256];
    ULONG n = 256;
    if (GetUserNameExW(NameDisplay, name, &n) && name[0]) s_userName = name;
    else { DWORD m = 256; if (GetUserNameW(name, &m)) s_userName = name; }
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
    BYTE buf[256];
    DWORD len = 0;
    LPWSTR sid = nullptr;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &len)) ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &sid);
    CloseHandle(tok);
    if (!sid) return;
    std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\") + sid;
    LocalFree(sid);
    wchar_t path[MAX_PATH];
    DWORD sz = sizeof path;
    for (const wchar_t* v : {L"Image192", L"Image96", L"Image64"}) {
        sz = sizeof path;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), v, RRF_RT_REG_SZ, nullptr, path, &sz) != ERROR_SUCCESS) continue;
        HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER fs{};
        GetFileSizeEx(f, &fs);
        std::vector<BYTE> bytes((size_t)std::min<LONGLONG>(fs.QuadPart, 8 << 20));
        DWORD got = 0;
        ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr);
        CloseHandle(f);
        if (got == bytes.size() && !bytes.empty()) { s_avatar = DecodeImage(s_ov.dc.Get(), bytes, (int)(72 * s_s)); if (s_avatar) return; }
    }
}

// ---------------------------------------------------------------- the built panes
struct GRow {
    enum Kind { Toggle, Link, Choice, Info, Slider, Check, Button } kind = Info;
    std::wstring label, sub, value;
    bool on = false, enabled = true;
    std::vector<std::wstring> options;
    int sel = -1;
    std::function<void(int)> act;     // Toggle/Link/Check/Button: act(0); Choice: act(option)
    float sv = 0;                     // Slider: 0..1
    std::function<void(float)> slide;
};
struct GSection { std::wstring caption; std::vector<GRow> rows; std::wstring note; };

GRow Toggle(const std::wstring& label, bool on, std::function<void()> f, const std::wstring& sub = L"") {
    GRow r; r.kind = GRow::Toggle; r.label = label; r.on = on; r.sub = sub; r.act = [f](int) { f(); }; return r;
}
GRow Link(const std::wstring& label, std::function<void()> f, const std::wstring& value = L"", const std::wstring& sub = L"") {
    GRow r; r.kind = GRow::Link; r.label = label; r.value = value; r.sub = sub; r.act = [f](int) { f(); }; return r;
}
GRow Choice(const std::wstring& label, std::vector<std::wstring> opts, int sel, std::function<void(int)> f, const std::wstring& sub = L"") {
    GRow r; r.kind = GRow::Choice; r.label = label; r.options = std::move(opts); r.sel = sel; r.act = std::move(f); r.sub = sub; return r;
}
GRow Info(const std::wstring& label, const std::wstring& value) {
    GRow r; r.kind = GRow::Info; r.label = label; r.value = value; return r;
}
GRow Check(const std::wstring& label, bool on, std::function<void()> f) {
    GRow r; r.kind = GRow::Check; r.label = label; r.on = on; r.act = [f](int) { f(); }; return r;
}
GRow SliderRow(const std::wstring& label, float v, std::function<void(float)> f) {
    GRow r; r.kind = GRow::Slider; r.label = label; r.sv = v; r.slide = std::move(f); return r;
}

// Windows Settings opens in front, so this sheet steps aside.
std::function<void()> Win(const wchar_t* uri) {
    std::wstring u = uri;
    return [u] { Launch(u); prefs::Close(); };
}
std::function<void()> Go(int pane) { return [pane] { SetPane(pane); }; }

// config.ini switches. reload: the part of Liquid that reads it only does so at start.
void Flag(bool& field, const wchar_t* key, bool v, bool reload) {
    field = v;
    SetConfigValue(key, v ? L"on" : L"off");
    if (reload) s_needReload = true;
}
std::function<void()> FlagFn(bool& field, const wchar_t* key, bool reload) {
    bool* p = &field;
    std::wstring k = key;
    return [p, k, reload] { Flag(*p, k.c_str(), !*p, reload); };
}

bool WinLightMode() {
    DWORD v = 1, sz = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                 RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v != 0;
}
void SetWinLightMode(bool light) {
    DWORD v = light ? 1 : 0;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    RegSetKeyValueW(HKEY_CURRENT_USER, key, L"AppsUseLightTheme", REG_DWORD, &v, sizeof v);
    RegSetKeyValueW(HKEY_CURRENT_USER, key, L"SystemUsesLightTheme", REG_DWORD, &v, sizeof v);
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 300, &r);
    Log(L"prefs: Windows %s mode", light ? L"light" : L"dark");
}

std::vector<GRow> DisplayRows() {
    std::vector<GRow> rows;
    struct Ctx { std::vector<GRow>* rows; int n; } ctx{&rows, 0};
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM lp) -> BOOL {
        auto* c = (Ctx*)lp;
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof mi;
        if (!GetMonitorInfoW(m, &mi)) return TRUE;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm);
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof dd;
        std::wstring model;
        if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) model = dd.DeviceString;
        c->n++;
        bool main = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        wchar_t v[96];
        swprintf(v, 96, L"%lu \u00D7 %lu, %lu Hz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
        GRow r = Info(main ? L"Main display" : L"Display " + std::to_wstring(c->n), v);
        if (!model.empty() && Lower(model).find(L"generic") == std::wstring::npos) r.sub = model;
        c->rows->push_back(r);
        return TRUE;
    }, (LPARAM)&ctx);
    return rows;
}

std::wstring FileName(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

// Every pane except the three hand-built ones. parent >= 0: a page inside another (back arrow).
std::vector<GSection> Build(int pane, std::wstring& title, int& parent) {
    std::vector<GSection> S;
    parent = -1;
    switch (pane) {
    case prefs::WiFi: {
        title = L"Wi-Fi";
        int radio = sys::WifiRadio();
        std::wstring ssid = sys::WifiConnectedSsid();
        GSection a;
        if (sys::WifiAvailable()) {
            a.rows.push_back(Toggle(L"Wi-Fi", radio == 1, [] { sys::SetWifiRadio(sys::WifiRadio() != 1); }));
            a.rows.push_back(Info(L"Network", radio != 1 ? L"Off" : ssid.empty() ? L"Not connected" : ssid));
        } else a.rows.push_back(Info(L"Wi-Fi", L"No Wi-Fi adapter"));
        if (sys::EthernetUp()) a.note = L"This PC is online through Ethernet.";
        S.push_back(a);
        S.push_back({L"Windows", {Link(L"Wi-Fi settings", Win(L"ms-settings:network-wifi")),
                                  Link(L"Known networks", Win(L"ms-settings:network-wifisettings")),
                                  Link(L"Mobile hotspot", Win(L"ms-settings:network-mobilehotspot"))}});
        break;
    }
    case prefs::Bluetooth: {
        title = L"Bluetooth";
        int bt = sys::BluetoothRadio();
        GSection a;
        if (bt < 0) a.rows.push_back(Info(L"Bluetooth", L"Unavailable"));
        else a.rows.push_back(Toggle(L"Bluetooth", bt == 1, [] { sys::SetBluetoothRadio(sys::BluetoothRadio() != 1); }));
        S.push_back(a);
        S.push_back({L"Devices", {Link(L"My devices", Win(L"ms-settings:bluetooth")),
                                  Link(L"Add a device", Win(L"ms-settings:connecteddevices")),
                                  Link(L"Printers & scanners", Go(prefs::Printers))}});
        break;
    }
    case prefs::Network: {
        title = L"Network";
        S.push_back({L"", {Info(L"Ethernet", sys::EthernetUp() ? L"Connected" : L"Not connected"),
                           Info(L"Wi-Fi", sys::WifiConnectedSsid().empty() ? L"Not connected" : sys::WifiConnectedSsid())}});
        S.push_back({L"Windows", {Link(L"Ethernet", Win(L"ms-settings:network-ethernet")),
                                  Link(L"VPN", Win(L"ms-settings:network-vpn")),
                                  Link(L"Proxy", Win(L"ms-settings:network-proxy")),
                                  Link(L"Data usage", Win(L"ms-settings:datausage")),
                                  Link(L"Advanced network settings", Win(L"ms-settings:network-advancedsettings")),
                                  Link(L"Firewall", Win(L"windowsdefender://network"))}});
        break;
    }
    case prefs::Energy: {
        title = L"Energy";
        SYSTEM_POWER_STATUS ps{};
        GetSystemPowerStatus(&ps);
        bool battery = ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
        GSection a;
        if (battery) {
            a.rows.push_back(Info(L"Battery", ps.BatteryLifePercent <= 100 ? std::to_wstring(ps.BatteryLifePercent) + L"%" : L"Unknown"));
            a.rows.push_back(Info(L"Power source", ps.ACLineStatus == 1 ? L"Power adapter" : L"Battery"));
        } else a.rows.push_back(Info(L"Power source", L"Plugged in (no battery)"));
        S.push_back(a);
        GSection w{L"Windows", {Link(L"Power mode, sleep and screen", Win(L"ms-settings:powersleep"))}};
        if (battery) w.rows.push_back(Link(L"Battery saver", Win(L"ms-settings:batterysaver")));
        S.push_back(w);
        break;
    }
    case prefs::General: {
        title = L"General";
        S.push_back({L"", {Link(L"About", Go(prefs::About)),
                           Link(L"Software Update", Win(L"ms-settings:windowsupdate")),
                           Link(L"Storage", Win(L"ms-settings:storagesense"))}});
        S.push_back({L"", {Link(L"Login Items", Go(prefs::LoginItems)),
                           Link(L"Language & Region", Win(L"ms-settings:regionlanguage")),
                           Link(L"Date & Time", Win(L"ms-settings:dateandtime")),
                           Link(L"Sharing", Win(L"ms-settings:crossdevice"))}});
        S.push_back({L"", {Link(L"Backup", Win(L"ms-settings:backup")),
                           Link(L"Transfer or Reset", Win(L"ms-settings:recovery"))}});
        break;
    }
    case prefs::About: {
        title = L"About";
        parent = prefs::General;
        std::wstring t, sub;
        std::vector<std::pair<std::wstring, std::wstring>> specs;
        about::Specs(t, sub, specs);
        wchar_t comp[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
        GSection a;
        a.rows.push_back(Info(L"Name", GetComputerNameW(comp, &n) ? comp : L""));
        a.rows.push_back(Info(L"Model", sub.empty() ? t : t + L"\n" + sub));
        for (auto& [k, v] : specs) a.rows.push_back(Info(k, v));
        S.push_back(a);
        S.push_back({L"", {Link(L"Rename this PC", Win(L"ms-settings:about")),
                           Link(L"Device specifications", Win(L"ms-settings:about")),
                           Link(L"Show About This PC", [] { prefs::Close(); about::Open(); })}});
        break;
    }
    case prefs::LoginItems: {
        title = L"Login Items";
        parent = prefs::General;
        GSection a{L"Open at login", {Toggle(L"Liquid", IsAutostart(), [] { SetAutostart(!IsAutostart()); })}};
        S.push_back(a);
        S.push_back({L"", {Link(L"Startup apps", Win(L"ms-settings:startupapps"))}});
        break;
    }
    case prefs::Accessibility: {
        title = L"Accessibility";
        S.push_back({L"Vision", {Link(L"Text size", Win(L"ms-settings:easeofaccess-display")),
                                 Link(L"Visual effects", Win(L"ms-settings:easeofaccess-visualeffects")),
                                 Link(L"Mouse pointer and touch", Win(L"ms-settings:easeofaccess-mousepointer")),
                                 Link(L"Magnifier", Win(L"ms-settings:easeofaccess-magnifier")),
                                 Link(L"Color filters", Win(L"ms-settings:easeofaccess-colorfilter")),
                                 Link(L"Contrast themes", Win(L"ms-settings:easeofaccess-highcontrast")),
                                 Link(L"Narrator", Win(L"ms-settings:easeofaccess-narrator"))}});
        S.push_back({L"Hearing", {Link(L"Captions", Win(L"ms-settings:easeofaccess-closedcaptioning")),
                                  Link(L"Audio", Win(L"ms-settings:easeofaccess-audio"))}});
        S.push_back({L"Interaction", {Link(L"Keyboard", Win(L"ms-settings:easeofaccess-keyboard")),
                                      Link(L"Mouse", Win(L"ms-settings:easeofaccess-mouse")),
                                      Link(L"Speech", Win(L"ms-settings:easeofaccess-speechrecognition"))}});
        break;
    }
    case prefs::Appearance: {
        title = L"Appearance";
        bool light = WinLightMode();
        S.push_back({L"", {Choice(L"Appearance", {L"Light", L"Dark"}, light ? 0 : 1, [](int i) { SetWinLightMode(i == 0); },
                                  L"Windows and its apps"),
                           Link(L"Accent colour", Win(L"ms-settings:colors")),
                           Link(L"Themes", Win(L"ms-settings:themes"))}});
        // Tahoe's "Icon & widget style": Default, Dark, Clear, Tinted
        const std::wstring& st = cfg.iconStyle;
        int sel = st == L"color" ? 0 : st == L"dark" ? 1 : st == L"clear" ? 2 : 3;
        S.push_back({L"", {Choice(L"Icon style", {L"Default", L"Dark", L"Clear", L"Tinted"}, sel, [](int i) {
                               cfg.iconStyle = i == 0 ? L"color" : i == 1 ? L"dark" : i == 2 ? L"clear" : L"tahoe";
                               SetConfigValue(L"icon_style", cfg.iconStyle);
                           }, L"Dock icons"),
                           Choice(L"Menu bar", {L"Glass", L"Clear"}, cfg.menubarGlass ? 0 : 1, [](int i) {
                               cfg.menubarGlass = i == 0;
                               SetConfigValue(L"menubar", i == 0 ? L"glass" : L"clear");
                               for (auto& mb : menubars) mb->dirty = true;
                           })}});
        break;
    }
    case prefs::CtrlCenter: {
        title = L"Control Center";
        GSection a{L"Control Center modules", {Info(L"Wi-Fi, Bluetooth and Ethernet", L"Always shown"),
                                               Info(L"Sound", L"Always shown"), Info(L"Now Playing", L"Always shown")}};
        S.push_back(a);
        S.push_back({L"", {Toggle(L"Show the volume when it changes", cfg.volumeHud, FlagFn(cfg.volumeHud, L"volume_hud", false),
                                  L"The volume keys show Liquid's slider instead of Windows' one")}});
        S.push_back({L"Windows", {Link(L"Quick settings and taskbar", Win(L"ms-settings:taskbar"))}});
        break;
    }
    case prefs::DesktopDock: {
        title = L"Desktop & Dock";
        int size = cfg.iconSize <= 34 ? 0 : cfg.iconSize <= 42 ? 1 : 2;
        S.push_back({L"Dock", {
            Choice(L"Size", {L"Small", L"Medium", L"Large"}, size, [](int i) {
                cfg.iconSize = i == 0 ? 32.f : i == 1 ? 38.f : 48.f;
                SetConfigValue(L"icon_size", i == 0 ? L"32" : i == 1 ? L"38" : L"48");
                s_needReload = true;
            }),
            Toggle(L"Magnification", cfg.magnify > 1.01f, [] {
                cfg.magnify = cfg.magnify > 1.01f ? 1.f : 1.6f;
                dock.M = cfg.magnify;
                SetConfigValue(L"magnify", cfg.magnify > 1.01f ? L"1.6" : L"1");
            }),
            Toggle(L"Automatically hide and show the Dock", cfg.autohide, [] {
                cfg.autohide = !cfg.autohide;
                SetConfigValue(L"autohide", cfg.autohide ? L"on" : L"off");
                dock.leftAt = GetTickCount64();
                dock.Reserve();
            }),
            Toggle(L"Windows slide under the Dock", cfg.dockOverlap, FlagFn(cfg.dockOverlap, L"dock_overlap", true)),
            Toggle(L"Show File Explorer first", cfg.finder, FlagFn(cfg.finder, L"finder", true)),
            Toggle(L"Show Trash last", cfg.trash, FlagFn(cfg.trash, L"trash", true))}});
        S.push_back({L"Desktop", {
            Toggle(L"Show widgets", cfg.widgets, FlagFn(cfg.widgets, L"widgets", true)),
            Toggle(L"Show desktop icons", cfg.desktopIcons, FlagFn(cfg.desktopIcons, L"desktop_icons", true)),
            Toggle(L"Liquid right-click menu", cfg.desktopMenu, FlagFn(cfg.desktopMenu, L"desktop_menu", false))}});
        S.push_back({L"Windows", {
            Toggle(L"Close, minimise and zoom buttons", cfg.trafficLights, FlagFn(cfg.trafficLights, L"traffic_lights", true),
                   L"Mac traffic lights on the active window"),
            Toggle(L"Hide Windows' caption buttons", cfg.hideCaptionButtons, FlagFn(cfg.hideCaptionButtons, L"hide_caption_buttons", true)),
            Toggle(L"Frosted windows", cfg.frostWindows, FlagFn(cfg.frostWindows, L"frost_windows", true)),
            Toggle(L"Rounded corners", cfg.roundCorners, FlagFn(cfg.roundCorners, L"round_corners", true)),
            Toggle(L"Minimise into the Dock", cfg.windowAnimations, FlagFn(cfg.windowAnimations, L"window_animations", true))}});
        S.push_back({L"Mission Control", {
            Toggle(L"Full-screen apps get their own space", cfg.fullscreenSpaces, FlagFn(cfg.fullscreenSpaces, L"fullscreen_spaces", false)),
            Toggle(L"App switcher on Alt+Tab", cfg.appSwitcher, FlagFn(cfg.appSwitcher, L"app_switcher", false)),
            Toggle(L"Right-drag to switch spaces", cfg.gestures, FlagFn(cfg.gestures, L"gestures", true))}});
        break;
    }
    case prefs::Displays: {
        title = L"Displays";
        S.push_back({L"", DisplayRows()});
        S.push_back({L"Windows", {Link(L"Display settings", Win(L"ms-settings:display")),
                                  Link(L"Night light", Win(L"ms-settings:nightlight")),
                                  Link(L"Graphics", Win(L"ms-settings:display-advancedgraphics"))}});
        break;
    }
    case prefs::MenuBar: {
        title = L"Menu Bar";
        S.push_back({L"", {Choice(L"Style", {L"Glass", L"Clear"}, cfg.menubarGlass ? 0 : 1, [](int i) {
                               cfg.menubarGlass = i == 0;
                               SetConfigValue(L"menubar", i == 0 ? L"glass" : L"clear");
                               for (auto& mb : menubars) mb->dirty = true;
                           }),
                           Toggle(L"Dynamic Island", cfg.dynamicIsland, FlagFn(cfg.dynamicIsland, L"dynamic_island", true),
                                  L"Music, Claude and notifications in the middle of the bar"),
                           Info(L"Menu", cfg.logo)}});
        break;
    }
    case prefs::Spotlight: {
        title = L"Spotlight";
        S.push_back({L"", {Toggle(L"Alt+Space opens Spotlight", cfg.spotlight, FlagFn(cfg.spotlight, L"spotlight", false)),
                           Toggle(L"The Windows key opens Spotlight", cfg.winKeySpotlight, FlagFn(cfg.winKeySpotlight, L"windows_key_spotlight", false),
                                  L"Instead of the Start menu")}});
        S.push_back({L"Windows", {Link(L"Search permissions", Win(L"ms-settings:search-permissions")),
                                  Link(L"Searching Windows", Win(L"ms-settings:cortana-windowssearch"))}});
        break;
    }
    case prefs::Wallpaper: {
        title = L"Wallpaper";
        HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(hm, &mi);
        wchar_t fb[MAX_PATH] = {};
        SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, fb, 0);
        int pos = 0;
        std::wstring wp = ScreenWallpaper(mi.rcMonitor, fb, pos);
        std::wstring lockPath = cfg.lockWallpaper.empty() ? ExeDir() + L"\\wallpapers\\lock.png" : cfg.lockWallpaper;
        bool own = Lower(cfg.lockWallpaper) != L"desktop" && GetFileAttributesW(lockPath.c_str()) != INVALID_FILE_ATTRIBUTES;
        std::wstring lockWp = own ? FileName(lockPath) : L"Same as the desktop";
        S.push_back({L"", {Info(L"Desktop", wp.empty() ? L"None" : FileName(wp)), Info(L"Lock screen", lockWp)}});
        S.push_back({L"Windows", {Link(L"Choose a wallpaper", Win(L"ms-settings:personalization-background")),
                                  Link(L"Lock screen picture", Win(L"ms-settings:lockscreen"))}});
        break;
    }
    case prefs::Notifications: {
        title = L"Notifications";
        bool access = sys::NotificationAccess() == 1;
        GSection a{L"", {Toggle(L"Show banners", cfg.macNotifications, FlagFn(cfg.macNotifications, L"mac_notifications", true),
                                L"In the top right corner, like macOS"),
                         Toggle(L"Show in the Dynamic Island", cfg.islandNotifications, FlagFn(cfg.islandNotifications, L"island_notifications", false))}};
        if (!access) a.note = L"Liquid can't read notifications yet. Allow it in Privacy & Security > Notifications.";
        S.push_back(a);
        S.push_back({L"Windows", {Link(L"Notification settings and Do Not Disturb", Win(L"ms-settings:notifications")),
                                  Link(L"Notification access", Win(L"ms-settings:privacy-notifications"))}});
        break;
    }
    case prefs::Sound: {
        title = L"Sound";
        GSection a{L"Output", {SliderRow(L"Output volume", sys::Muted() ? 0.f : sys::Volume(), [](float v) {
                                   if (sys::Muted() && v > 0) sys::SetMuted(false);
                                   sys::SetVolume(v);
                               }),
                               Toggle(L"Mute", sys::Muted(), [] { sys::SetMuted(!sys::Muted()); })}};
        S.push_back(a);
        GSection d{L"Play sound through", {}};
        for (auto& dev : sys::OutputDevices()) {
            std::wstring id = dev.id;
            d.rows.push_back(Check(dev.name, dev.isDefault, [id] { sys::SetDefaultOutput(id); }));
        }
        S.push_back(d);
        S.push_back({L"Windows", {Link(L"Sound settings", Win(L"ms-settings:sound")),
                                  Link(L"Volume mixer", Win(L"ms-settings:apps-volume"))}});
        break;
    }
    case prefs::Focus: {
        title = L"Focus";
        S.push_back({L"", {Link(L"Do Not Disturb", Win(L"ms-settings:notifications"), L"", L"Turn it on, or set when it turns on by itself")}});
        break;
    }
    case prefs::LockScreen: {
        title = L"Lock Screen";
        float m = cfg.lockIdleMinutes;
        int sel = m <= 0 ? 0 : m <= 2 ? 1 : m <= 5 ? 2 : m <= 10 ? 3 : 4;
        S.push_back({L"", {Choice(L"Lock when idle for", {L"Never", L"2 min", L"5 min", L"10 min", L"30 min"}, sel, [](int i) {
                               const float mins[] = {0, 2, 5, 10, 30};
                               cfg.lockIdleMinutes = mins[i];
                               SetConfigValue(L"lock_idle_minutes", std::to_wstring((int)mins[i]));
                           }, L"No keyboard or mouse use, and no music playing"),
                           Toggle(L"Lock when Liquid starts", cfg.lockOnStart, FlagFn(cfg.lockOnStart, L"lock_on_start", false)),
                           Toggle(L"Lock when the PC wakes up", cfg.lockOnWake, FlagFn(cfg.lockOnWake, L"lock_on_wake", false)),
                           Link(L"Face ID, PIN and walking away", Go(prefs::LockFace))}});
        S.push_back({L"Windows", {Link(L"Windows lock screen", Win(L"ms-settings:lockscreen")),
                                  Link(L"Screen saver and sleep", Win(L"ms-settings:powersleep"))}});
        break;
    }
    case prefs::Privacy: {
        title = L"Privacy & Security";
        S.push_back({L"Privacy", {Link(L"Location", Win(L"ms-settings:privacy-location")),
                                  Link(L"Camera", Win(L"ms-settings:privacy-webcam")),
                                  Link(L"Microphone", Win(L"ms-settings:privacy-microphone")),
                                  Link(L"Notifications", Win(L"ms-settings:privacy-notifications")),
                                  Link(L"App permissions", Win(L"ms-settings:privacy"))}});
        S.push_back({L"Security", {Link(L"Windows Security", Win(L"windowsdefender:")),
                                   Link(L"Find my device", Win(L"ms-settings:findmydevice")),
                                   Link(L"For developers", Win(L"ms-settings:developers"))}});
        break;
    }
    case prefs::Users: {
        title = L"Users & Groups";
        S.push_back({L"", {Info(L"You", s_userName)}});
        S.push_back({L"Windows", {Link(L"Your info", Win(L"ms-settings:yourinfo")),
                                  Link(L"Sign-in options", Win(L"ms-settings:signinoptions")),
                                  Link(L"Other users", Win(L"ms-settings:otherusers")),
                                  Link(L"Family", Win(L"ms-settings:family-group")),
                                  Link(L"Email and accounts", Win(L"ms-settings:emailandaccounts"))}});
        break;
    }
    case prefs::Gaming: {
        title = L"Gaming";
        std::wstring apps = cfg.borderlessApps;
        for (size_t p; (p = apps.find(L',')) != std::wstring::npos;) apps.replace(p, 1, L", ");
        S.push_back({L"Liquid", {Info(L"Borderless full screen apps", apps.empty() ? L"None" : apps)}, L"Ctrl+Win+F makes the active window borderless full screen, or back."});
        S.push_back({L"Windows", {Link(L"Game Bar", Win(L"ms-settings:gaming-gamebar")),
                                  Link(L"Captures", Win(L"ms-settings:gaming-gamedvr")),
                                  Link(L"Game Mode", Win(L"ms-settings:gaming-gamemode"))}});
        break;
    }
    case prefs::Apps: {
        title = L"Apps";
        S.push_back({L"", {Link(L"Installed apps", Win(L"ms-settings:appsfeatures")),
                           Link(L"Default apps", Win(L"ms-settings:defaultapps")),
                           Link(L"Startup apps", Win(L"ms-settings:startupapps")),
                           Link(L"Optional features", Win(L"ms-settings:optionalfeatures")),
                           Link(L"Apps for websites", Win(L"ms-settings:appsforwebsites"))}});
        break;
    }
    case prefs::Keyboard: {
        title = L"Keyboard";
        S.push_back({L"Liquid shortcuts", {Info(L"Spotlight", L"Alt+Space"),
                                           Info(L"Lock screen", L"Win+L"),
                                           Info(L"Borderless full screen", L"Ctrl+Win+F"),
                                           Info(L"Snapshot of the screen with Liquid", L"Ctrl+Alt+Shift+S"),
                                           Info(L"Reload Liquid", L"Ctrl+Alt+Shift+R"),
                                           Info(L"Quit Liquid", L"Ctrl+Alt+Shift+Q")}});
        S.push_back({L"Windows", {Link(L"Typing", Win(L"ms-settings:typing")),
                                  Link(L"Keyboard layouts and languages", Win(L"ms-settings:regionlanguage")),
                                  Link(L"Accessibility keyboard", Win(L"ms-settings:easeofaccess-keyboard"))}});
        break;
    }
    case prefs::Mouse: {
        title = L"Mouse";
        S.push_back({L"", {Toggle(L"Right-drag to switch spaces", cfg.gestures, FlagFn(cfg.gestures, L"gestures", true),
                                  L"Hold the right button and drag left or right")}});
        S.push_back({L"Windows", {Link(L"Mouse", Win(L"ms-settings:mousetouchpad")),
                                  Link(L"Pointer size and colour", Win(L"ms-settings:easeofaccess-mousepointer"))}});
        break;
    }
    case prefs::Printers: {
        title = L"Printers & Scanners";
        S.push_back({L"", {Link(L"Printers & scanners", Win(L"ms-settings:printers"))}});
        break;
    }
    }
    return S;
}

// ---------------------------------------------------------------- drawing
void Render() {
    s_hits.clear();
    s_actions.clear();
    s_gsliders.clear();
    float s = s_s, a = s_anim;
    D2D1_RECT_F box = Box();
    float x0 = box.left + (kSide + kPadX) * s, x1 = box.right - kPadX * s, top = box.top;

    std::vector<GlassShape> sh;
    GlassShape gs{box.left, box.top, kW * s, kH * s};
    gs.radius = 26 * s; gs.blur = 2.6f; gs.tint = -0.4f; gs.bezel = 18 * s; gs.thick = 2.2f;
    gs.shadow = 0.32f; gs.highlight = 0.0f; gs.opacity = a;
    sh.push_back(gs);
    float tint[3] = {0.1f, 0.1f, 0.12f};

    const D2D1_COLOR_F fg = D2D1::ColorF(1, 1, 1, 0.95f * a), fg2 = D2D1::ColorF(1, 1, 1, 0.68f * a),
                       dim = D2D1::ColorF(1, 1, 1, 0.38f * a), accent = D2D1::ColorF(0.04f, 0.52f, 1.0f, a);
    bool airUp = s_air.Answering(), airProc = AirLink::Process();
    bool built = s_pane != prefs::LockFace && s_pane != prefs::AirGestures;
    static std::wstring builtTitle;
    static int parent = -1, cachedPane = -1;
    static std::vector<GSection> secs;
    if (built && (s_rebuild || cachedPane != s_pane)) {
        secs = Build(s_pane, builtTitle, parent);
        cachedPane = s_pane;
        s_rebuild = false;
    }
    if (!built) parent = -1;

    s_ov.Render(s, sh, tint, [&](ID2D1DeviceContext* dc) {
        // The sheet sits over whatever window is behind it: darken it enough that text always
        // clears 4.5:1, even over a white page.
        ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        g.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(box, 26 * s, 26 * s), &clipGeo);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clipGeo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), a), nullptr);
        s_br->SetColor(D2D1::ColorF(0, 0, 0, 0.5f));
        dc->FillRectangle(box, s_br.Get());
        s_br->SetColor(D2D1::ColorF(0, 0, 0, 0.22f));   // sidebar a shade darker
        dc->FillRectangle(D2D1::RectF(box.left, box.top, box.left + kSide * s, box.bottom), s_br.Get());
        s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.08f));
        dc->FillRectangle(D2D1::RectF(box.left + kSide * s, box.top, box.left + kSide * s + 1, box.bottom), s_br.Get());
        dc->PopLayer();

        auto text = [&](IDWriteTextFormat* f, const std::wstring& t, D2D1_RECT_F r, D2D1_COLOR_F c) {
            s_br->SetColor(c);
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        auto focusRing = [&](D2D1_RECT_F r, float rad, int id) {
            if (s_focus != id) return;
            s_br->SetColor(accent);
            D2D1_RECT_F f = D2D1::RectF(r.left - 3 * s, r.top - 3 * s, r.right + 3 * s, r.bottom + 3 * s);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(f, rad + 3 * s, rad + 3 * s), s_br.Get(), 2 * s);
        };
        auto button = [&](D2D1_RECT_F r, const std::wstring& label, int id, bool enabled = true) {
            s_hits.push_back({r, id, enabled});
            float alpha = !enabled ? 0.05f : s_pressed == id && s_hover == id ? 0.08f : s_hover == id ? 0.22f : 0.14f;
            s_br->SetColor(D2D1::ColorF(1, 1, 1, alpha * a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 7 * s, 7 * s), s_br.Get());
            text(fBodyC.Get(), label, r, enabled ? fg : dim);
            focusRing(r, 7 * s, id);
        };
        auto bw = [&](const std::wstring& label) { return TextWidth(fBodyC.Get(), label) + 28 * s; };
        auto toggle = [&](float right, float cy, bool on, bool enabled) {
            D2D1_RECT_F t = D2D1::RectF(right - 40 * s, cy - 12 * s, right, cy + 12 * s);
            float ea = enabled ? 1.f : 0.35f;
            s_br->SetColor(on ? D2D1::ColorF(accent.r, accent.g, accent.b, a * ea) : D2D1::ColorF(1, 1, 1, 0.22f * a * ea));
            dc->FillRoundedRectangle(D2D1::RoundedRect(t, 12 * s, 12 * s), s_br.Get());
            s_br->SetColor(D2D1::ColorF(1, 1, 1, a * ea));
            float kx = on ? t.right - 12 * s : t.left + 12 * s;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, cy), 10 * s, 10 * s), s_br.Get());
        };
        auto group = [&](float y, int rows, float rowH) {
            D2D1_RECT_F g2 = D2D1::RectF(x0, y, x1, y + rows * rowH * s);
            s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(g2, 12 * s, 12 * s), s_br.Get());
            s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * a));
            for (int i = 1; i < rows; i++) {
                float ly = y + i * rowH * s;
                dc->FillRectangle(D2D1::RectF(x0 + 14 * s, ly, x1 - 14 * s, ly + 1), s_br.Get());
            }
            return g2;
        };
        // a whole row is the click target for its toggle (bigger than the switch itself)
        auto toggleRow = [&](float gx0, float gx1, float y, float rowH, const std::wstring& label, bool on, int id, bool enabled) {
            D2D1_RECT_F r = D2D1::RectF(gx0, y, gx1, y + rowH * s);
            s_hits.push_back({r, id, enabled});
            if (enabled && s_hover == id) {
                s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.05f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 10 * s, 10 * s), s_br.Get());
            }
            text(fBody.Get(), label, D2D1::RectF(gx0 + 14 * s, y, gx1 - 60 * s, y + rowH * s), enabled ? fg : dim);
            toggle(gx1 - 14 * s, y + rowH * s / 2, on, enabled);
            if (s_focus == id) focusRing(D2D1::RectF(gx1 - 54 * s, y + rowH * s / 2 - 12 * s, gx1 - 14 * s, y + rowH * s / 2 + 12 * s), 12 * s, id);
        };

        // ---- window controls: close works; minimise and zoom are greyed out
        D2D1_POINT_2F cd = D2D1::Point2F(box.left + 22 * s, box.top + 22 * s);
        s_hits.push_back({D2D1::RectF(cd.x - 11 * s, cd.y - 11 * s, cd.x + 11 * s, cd.y + 11 * s), H_CLOSE, true});
        s_br->SetColor(D2D1::ColorF(1.0f, 0.373f, 0.341f, a));
        dc->FillEllipse(D2D1::Ellipse(cd, 6.5f * s, 6.5f * s), s_br.Get());
        s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.2f * a));
        for (int i = 1; i < 3; i++) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cd.x + i * 20 * s, cd.y), 6.5f * s, 6.5f * s), s_br.Get());
        if (s_hover == H_CLOSE || s_focus == H_CLOSE) text(fIcon.Get(), L"\uE711", D2D1::RectF(cd.x - 6 * s, cd.y - 6 * s, cd.x + 6 * s, cd.y + 6 * s),
                                                          D2D1::ColorF(0.3f, 0, 0, 0.8f * a));
        focusRing(D2D1::RectF(cd.x - 7 * s, cd.y - 7 * s, cd.x + 7 * s, cd.y + 7 * s), 7 * s, H_CLOSE);

        // ---- sidebar: your account, then the panes (scrolls with the wheel)
        D2D1_RECT_F side = D2D1::RectF(box.left, box.top + 44 * s, box.left + kSide * s, box.bottom - 8 * s);
        dc->PushAxisAlignedClip(side, D2D1_ANTIALIAS_MODE_ALIASED);
        float sy = side.top + 4 * s - s_sideScroll;
        {
            D2D1_RECT_F r = D2D1::RectF(box.left + 10 * s, sy, box.left + (kSide - 10) * s, sy + 52 * s);
            if (r.bottom > side.top && r.top < side.bottom) s_hits.push_back({r, H_ACCOUNT, true});
            if (s_hover == H_ACCOUNT) {
                s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.07f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 10 * s, 10 * s), s_br.Get());
            }
            D2D1_RECT_F av = D2D1::RectF(r.left + 6 * s, r.top + 7 * s, r.left + 44 * s, r.top + 45 * s);
            if (s_avatar) {
                ComPtr<ID2D1EllipseGeometry> geo;
                g.d2dFactory->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F((av.left + av.right) / 2, (av.top + av.bottom) / 2), 19 * s, 19 * s), &geo);
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
                dc->DrawBitmap(s_avatar.Get(), &av, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                dc->PopLayer();
            } else {   // initials, as macOS shows for an account without a picture
                std::wstring ini;
                for (size_t i = 0; i < s_userName.size() && ini.size() < 2; i++)
                    if (iswalpha(s_userName[i]) && (i == 0 || s_userName[i - 1] == L' ')) ini += (wchar_t)towupper(s_userName[i]);
                s_br->SetColor(D2D1::ColorF(0.55f, 0.56f, 0.6f, a));
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((av.left + av.right) / 2, (av.top + av.bottom) / 2), 19 * s, 19 * s), s_br.Get());
                text(fBodyC.Get(), ini, av, fg);
            }
            text(fHead.Get(), s_userName, D2D1::RectF(av.right + 10 * s, r.top + 8 * s, r.right - 4 * s, r.top + 28 * s), fg);
            text(fWrapSmall.Get(), L"Windows account", D2D1::RectF(av.right + 10 * s, r.top + 28 * s, r.right - 4 * s, r.top + 46 * s), fg2);
            focusRing(r, 10 * s, H_ACCOUNT);
            sy = r.bottom + 10 * s;
        }
        auto items = SideItems();
        for (size_t i = 0; i < items.size(); i++) {
            auto& it = items[i];
            if (it.gap) sy += 10 * s;
            D2D1_RECT_F r = D2D1::RectF(box.left + 10 * s, sy, box.left + (kSide - 10) * s, sy + 30 * s);
            int id = H_SIDE0 + (int)i;
            bool vis = r.bottom > side.top && r.top < side.bottom;
            if (vis) s_hits.push_back({r, id, true});
            bool sel = s_pane == it.pane || (built && parent == it.pane);
            if (sel || s_hover == id) {
                s_br->SetColor(sel ? D2D1::ColorF(accent.r, accent.g, accent.b, 0.85f * a) : D2D1::ColorF(1, 1, 1, 0.07f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8 * s, 8 * s), s_br.Get());
            }
            D2D1_RECT_F ic = D2D1::RectF(r.left + 8 * s, r.top + 5 * s, r.left + 28 * s, r.top + 25 * s);
            s_br->SetColor(D2D1::ColorF(it.color.r, it.color.g, it.color.b, a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(ic, 5.5f * s, 5.5f * s), s_br.Get());
            if (it.glyph) text(fSideIcon.Get(), it.glyph, ic, D2D1::ColorF(1, 1, 1, a));
            else island::DrawFaceIdGlyph(dc, s_br.Get(), (ic.left + ic.right) / 2, (ic.top + ic.bottom) / 2, 6.5f * s, a);
            text(fSideName.Get(), it.name, D2D1::RectF(ic.right + 9 * s, r.top, r.right - 4 * s, r.bottom), fg);
            focusRing(r, 8 * s, id);
            sy = r.bottom + 2 * s;
        }
        s_sideMax = std::max(0.f, sy + s_sideScroll - side.bottom + 8 * s);
        dc->PopAxisAlignedClip();

        if (built) {
            // ---- title (with a back arrow on a page inside another)
            float tx = x0;
            if (parent >= 0) {
                D2D1_RECT_F br = D2D1::RectF(x0 - 6 * s, top + 16 * s, x0 + 24 * s, top + 50 * s);
                s_hits.push_back({br, H_BACK, true});
                if (s_hover == H_BACK) {
                    s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * a));
                    dc->FillRoundedRectangle(D2D1::RoundedRect(br, 7 * s, 7 * s), s_br.Get());
                }
                text(fChevron.Get(), L"\uE76B", br, fg);
                focusRing(br, 7 * s, H_BACK);
                tx = x0 + 32 * s;
            }
            text(fTitle.Get(), builtTitle, D2D1::RectF(tx, top + 18 * s, x1, top + 50 * s), fg);

            // ---- sections, scrolling under the title
            float clipBottom = box.bottom - (s_needReload ? 60 : 8) * s;
            D2D1_RECT_F clip = D2D1::RectF(x0 - 10 * s, top + 58 * s, x1 + 10 * s, clipBottom);
            dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
            float y = top + 64 * s - s_scroll;
            for (auto& sec : secs) {
                if (sec.rows.empty()) continue;
                if (!sec.caption.empty()) {
                    text(fSmall.Get(), sec.caption, D2D1::RectF(x0 + 4 * s, y, x1, y + 18 * s), fg2);
                    y += 22 * s;
                }
                // row heights first, so the group box can be drawn behind them
                std::vector<float> hs;
                for (auto& r : sec.rows) {
                    float h = r.sub.empty() ? 40.f : 52.f;
                    if (r.kind == GRow::Info && r.value.find(L'\n') != std::wstring::npos)
                        h = std::max(h, 22.f + 18.f * (float)(1 + std::count(r.value.begin(), r.value.end(), L'\n')));
                    if (r.kind == GRow::Slider) h = 56;
                    hs.push_back(h * s);
                }
                float gh = 0;
                for (float h : hs) gh += h;
                D2D1_RECT_F gr = D2D1::RectF(x0, y, x1, y + gh);
                s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(gr, 12 * s, 12 * s), s_br.Get());
                float ry = y;
                for (size_t i = 0; i < sec.rows.size(); i++) {
                    GRow& row = sec.rows[i];
                    float h = hs[i];
                    D2D1_RECT_F rr = D2D1::RectF(x0, ry, x1, ry + h);
                    bool vis = rr.bottom > clip.top && rr.top < clip.bottom;
                    if (i) {
                        s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * a));
                        dc->FillRectangle(D2D1::RectF(x0 + 14 * s, ry, x1 - 14 * s, ry + 1), s_br.Get());
                    }
                    float labelR = x1 - 60 * s;
                    float cy = ry + h / 2;
                    auto addHit = [&](D2D1_RECT_F r, std::function<void()> f, bool enabled = true) {
                        int id = H_GEN0 + (int)s_actions.size();
                        s_actions.push_back(std::move(f));
                        if (vis) s_hits.push_back({r, id, enabled});
                        return id;
                    };
                    auto hot = [&](int id, D2D1_RECT_F r) {
                        if (s_hover == id) {
                            s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.05f * a));
                            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 10 * s, 10 * s), s_br.Get());
                        }
                    };
                    if (row.kind == GRow::Toggle || row.kind == GRow::Link || row.kind == GRow::Check) {
                        auto act = row.act;
                        int id = addHit(rr, [act] { act(0); }, row.enabled);
                        hot(id, rr);
                        focusRing(rr, 10 * s, id);
                    }
                    if (row.kind == GRow::Choice) {   // segmented control: measure it, then the label gets the rest
                        float segW = 0;
                        std::vector<float> ws;
                        for (auto& o : row.options) { float w = TextWidth(fBodyC.Get(), o) + 24 * s; ws.push_back(w); segW += w; }
                        labelR = x1 - 14 * s - segW - 12 * s;
                    }
                    if (row.kind == GRow::Info) labelR = x0 + (x1 - x0) * 0.45f;
                    if (row.kind == GRow::Link && !row.value.empty()) labelR = x0 + (x1 - x0) * 0.55f;
                    // label and its grey second line
                    if (row.kind != GRow::Slider) {
                        if (row.sub.empty()) text(fBody.Get(), row.label, D2D1::RectF(x0 + 14 * s, ry, labelR, ry + h), row.enabled ? fg : dim);
                        else {
                            text(fBody.Get(), row.label, D2D1::RectF(x0 + 14 * s, ry + 7 * s, labelR, ry + 27 * s), row.enabled ? fg : dim);
                            text(fWrapSmall.Get(), row.sub, D2D1::RectF(x0 + 14 * s, ry + 27 * s, labelR, ry + h - 4 * s), fg2);
                        }
                    }
                    switch (row.kind) {
                    case GRow::Toggle: toggle(x1 - 14 * s, cy, row.on, row.enabled); break;
                    case GRow::Check:
                        if (row.on) text(fIcon.Get(), L"\uE73E", D2D1::RectF(x1 - 40 * s, ry, x1 - 14 * s, ry + h), accent);
                        break;
                    case GRow::Link:
                        if (!row.value.empty()) text(fBodyR.Get(), row.value, D2D1::RectF(labelR, ry, x1 - 34 * s, ry + h), fg2);
                        text(fChevron.Get(), L"\uE76C", D2D1::RectF(x1 - 32 * s, ry, x1 - 10 * s, ry + h), fg2);
                        break;
                    case GRow::Info: {
                        ComPtr<IDWriteTextLayout> tl;
                        float w = x1 - 14 * s - labelR;
                        g.dw->CreateTextLayout(row.value.c_str(), (UINT32)row.value.size(), fBodyR.Get(), w, h, &tl);
                        if (tl) {
                            tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                            s_br->SetColor(fg2);
                            dc->DrawTextLayout(D2D1::Point2F(labelR, ry), tl.Get(), s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        }
                        break;
                    }
                    case GRow::Choice: {
                        float segW = 0;
                        std::vector<float> ws;
                        for (auto& o : row.options) { float w = TextWidth(fBodyC.Get(), o) + 24 * s; ws.push_back(w); segW += w; }
                        float sx = x1 - 14 * s - segW;
                        D2D1_RECT_F track = D2D1::RectF(sx, cy - 13 * s, x1 - 14 * s, cy + 13 * s);
                        s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.1f * a));
                        dc->FillRoundedRectangle(D2D1::RoundedRect(track, 8 * s, 8 * s), s_br.Get());
                        for (size_t k = 0; k < row.options.size(); k++) {
                            D2D1_RECT_F seg = D2D1::RectF(sx, track.top, sx + ws[k], track.bottom);
                            auto act = row.act;
                            int opt = (int)k;
                            int id = addHit(seg, [act, opt] { act(opt); });
                            bool selk = row.sel == (int)k;
                            if (selk || s_hover == id) {
                                D2D1_RECT_F in = D2D1::RectF(seg.left + 2 * s, seg.top + 2 * s, seg.right - 2 * s, seg.bottom - 2 * s);
                                s_br->SetColor(D2D1::ColorF(1, 1, 1, (selk ? 0.28f : 0.08f) * a));
                                dc->FillRoundedRectangle(D2D1::RoundedRect(in, 6.5f * s, 6.5f * s), s_br.Get());
                            }
                            text(fBodyC.Get(), row.options[k], seg, selk ? fg : fg2);
                            focusRing(seg, 8 * s, id);
                            sx += ws[k];
                        }
                        break;
                    }
                    case GRow::Slider: {
                        text(fBody.Get(), row.label, D2D1::RectF(x0 + 14 * s, ry + 6 * s, x1 - 70 * s, ry + 26 * s), fg);
                        text(fBodyR.Get(), std::to_wstring((int)std::lround((s_gdrag == H_GEN0 + (int)s_actions.size() ? s_gdragVal : row.sv) * 100)) + L"%",
                             D2D1::RectF(x1 - 90 * s, ry + 6 * s, x1 - 14 * s, ry + 26 * s), fg2);
                        int id = H_GEN0 + (int)s_actions.size();
                        if (s_gdrag == id) row.sv = s_gdragVal;   // while dragging, the knob follows the mouse
                        float tl = x0 + 22 * s, tr = x1 - 22 * s, ty = ry + 40 * s, kx = tl + (tr - tl) * std::clamp(row.sv, 0.f, 1.f);
                        addHit(D2D1::RectF(tl - 10 * s, ty - 14 * s, tr + 10 * s, ty + 14 * s), [] {});
                        s_gsliders.push_back({id, tl, tr, row.slide});
                        s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.18f * a));
                        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, ty - 3 * s, tr, ty + 3 * s), 3 * s, 3 * s), s_br.Get());
                        s_br->SetColor(accent);
                        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, ty - 3 * s, kx, ty + 3 * s), 3 * s, 3 * s), s_br.Get());
                        float kr = (s_gdrag == id || s_hover == id) ? 10.5f * s : 9 * s;
                        s_br->SetColor(D2D1::ColorF(0, 0, 0, 0.3f * a));
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ty + 1.5f * s), kr, kr), s_br.Get());
                        s_br->SetColor(D2D1::ColorF(1, 1, 1, a));
                        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, ty), kr, kr), s_br.Get());
                        break;
                    }
                    default: break;
                    }
                    ry += h;
                }
                y = gr.bottom + 8 * s;
                if (!sec.note.empty()) {
                    text(fWrapSmall.Get(), sec.note, D2D1::RectF(x0 + 4 * s, y, x1, y + 34 * s), fg2);
                    y += 30 * s;
                }
                y += 14 * s;
            }
            s_scrollMax = std::max(0.f, y + s_scroll - clipBottom);
            dc->PopAxisAlignedClip();

            // ---- some switches only take effect when Liquid starts again
            if (s_needReload) {
                D2D1_RECT_F bar = D2D1::RectF(x0, box.bottom - 52 * s, x1, box.bottom - 14 * s);
                s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(bar, 10 * s, 10 * s), s_br.Get());
                std::wstring lbl = L"Restart Liquid";
                text(fBody.Get(), L"Some changes apply when Liquid restarts.", D2D1::RectF(bar.left + 14 * s, bar.top, bar.right - bw(lbl) - 20 * s, bar.bottom), fg);
                button(D2D1::RectF(bar.right - 8 * s - bw(lbl), bar.top + 5 * s, bar.right - 8 * s, bar.bottom - 5 * s), lbl, H_RELOAD);
            }
            return;
        }

        if (s_pane == prefs::LockFace) {
            text(fTitle.Get(), L"Face ID & Password", D2D1::RectF(x0, top + 18 * s, x1, top + 50 * s), fg);
            // ---- face unlock (the focal point)
            float y0 = top + 64 * s;
            D2D1_RECT_F sq = D2D1::RectF(x0, y0, x0 + 68 * s, y0 + 68 * s);
            s_br->SetColor(D2D1::ColorF(0, 0, 0, 0.55f * a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(sq, 16 * s, 16 * s), s_br.Get());
            int faces = airUp ? _wtoi(s_air.Get(L"face").c_str()) : -1;
            island::DrawFaceIdGlyph(dc, s_br.Get(), (sq.left + sq.right) / 2, (sq.top + sq.bottom) / 2, 22 * s, faces > 0 ? a : 0.55f * a);
            std::wstring head, sub;
            if (faces < 0) {
                head = L"Face ID needs Air Ring";
                sub = airProc ? L"Air Ring is starting or not answering. If this stays, restart Liquid from the menu."
                              : L"Air Ring owns the webcam and isn't running. Start it from Air Gestures, then come back here.";
            } else if (faces == 0) {
                head = L"Face ID is off";
                sub = L"Look at the camera for about 20 seconds while it learns your face. Your PIN stays as the backup.";
            } else {
                head = L"Face ID is on";
                sub = std::to_wstring(faces) + L" pictures of your face are saved. The lock screen opens when the camera sees you.";
            }
            float tx = sq.right + 18 * s;
            text(fHead.Get(), head, D2D1::RectF(tx, y0 - 2 * s, x1, y0 + 22 * s), fg);
            text(fWrap.Get(), sub, D2D1::RectF(tx, y0 + 24 * s, x1, y0 + 62 * s), fg2);
            std::wstring setup = faces > 0 ? L"Set Up Again\u2026" : L"Set Up Face ID\u2026";
            float by = y0 + 66 * s, bx = tx;
            button(D2D1::RectF(bx, by, bx + bw(setup), by + 28 * s), setup, H_FACE_SETUP, faces >= 0);
            bx += bw(setup) + 10 * s;
            button(D2D1::RectF(bx, by, bx + bw(L"Forget My Face"), by + 28 * s), L"Forget My Face", H_FACE_FORGET, faces > 0);

            // ---- walk away
            const float rowH = 40;
            float y = top + 174 * s;
            text(fSmall.Get(), L"When you walk away", D2D1::RectF(x0 + 4 * s, y, x1, y + 18 * s), fg2);
            y += 22 * s;
            group(y, 2, rowH);
            toggleRow(x0, x1, y, rowH, L"Lock the screen", cfg.awayLockSeconds > 0, H_AWAY_LOCK, true);
            toggleRow(x0, x1, y + rowH * s, rowH, L"Pause the music", cfg.awayPauseSeconds > 0, H_AWAY_PAUSE, true);
            y += 2 * rowH * s + 6 * s;
            float secs = cfg.awayLockSeconds > 0 ? cfg.awayLockSeconds : cfg.awayPauseSeconds;
            std::wstring note = (cfg.awayLockSeconds > 0 || cfg.awayPauseSeconds > 0)
                ? L"After " + std::to_wstring((int)std::lround(secs)) + L" seconds with nobody in front of the camera and no keyboard or mouse use. "
                  L"Face ID starts the music again."
                : L"Both are off: Liquid ignores whether you're at the desk.";
            text(fWrapSmall.Get(), note, D2D1::RectF(x0 + 4 * s, y, x1, y + 34 * s), fg2);

            // ---- password
            y = top + 330 * s;
            text(fSmall.Get(), L"Password", D2D1::RectF(x0 + 4 * s, y, x1, y + 18 * s), fg2);
            y += 22 * s;
            group(y, 3, rowH);
            toggleRow(x0, x1, y, rowH, L"Lock when the PC wakes up", cfg.lockOnWake, H_WAKE_LOCK, true);
            float ry = y + rowH * s;
            text(fBody.Get(), L"Lock PIN", D2D1::RectF(x0 + 14 * s, ry, x1, ry + rowH * s), fg);
            std::wstring pinLbl = cfg.lockPinHash.empty() ? L"Set PIN\u2026" : L"Change PIN\u2026";
            button(D2D1::RectF(x1 - 14 * s - bw(pinLbl), ry + 6 * s, x1 - 14 * s, ry + (rowH - 6) * s), pinLbl, H_PIN);
            ry += rowH * s;
            text(fBody.Get(), L"Lock right now", D2D1::RectF(x0 + 14 * s, ry, x1, ry + rowH * s), fg);
            button(D2D1::RectF(x1 - 14 * s - bw(L"Lock Screen"), ry + 6 * s, x1 - 14 * s, ry + (rowH - 6) * s), L"Lock Screen", H_LOCK_NOW);
        } else {
            text(fTitle.Get(), L"Air Gestures", D2D1::RectF(x0, top + 18 * s, x1, top + 50 * s), fg);
            // ---- status line
            std::wstring state = s_air.Get(L"state"), status;
            D2D1_COLOR_F dot;
            if (!airUp && !airProc) { status = L"Air Ring isn't running"; dot = D2D1::ColorF(0.6f, 0.6f, 0.62f, a); }
            else if (!airUp) { status = L"Air Ring is open but not answering"; dot = D2D1::ColorF(1.0f, 0.72f, 0.2f, a); }
            else if (state == L"paused") { status = L"Camera is off"; dot = D2D1::ColorF(0.6f, 0.6f, 0.62f, a); }
            else if (state == L"nocam") { status = L"No webcam found"; dot = D2D1::ColorF(1.0f, 0.37f, 0.34f, a); }
            else {
                status = L"Watching through " + s_air.Get(L"camera") + L", " + s_air.Get(L"fps") + L" frames a second";
                dot = D2D1::ColorF(0.2f, 0.82f, 0.4f, a);
            }
            float sy2 = top + 50 * s;
            s_br->SetColor(dot);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x0 + 5 * s, sy2 + 9 * s), 4 * s, 4 * s), s_br.Get());
            text(fSmall.Get(), status, D2D1::RectF(x0 + 16 * s, sy2, x1, sy2 + 18 * s), fg2);

            // ---- camera (the focal point)
            const float camW = 360, camH = 270;
            D2D1_RECT_F cam = D2D1::RectF(x0, top + 80 * s, x0 + camW * s, top + (80 + camH) * s);
            s_br->SetColor(D2D1::ColorF(0, 0, 0, 0.6f * a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(cam, 14 * s, 14 * s), s_br.Get());
            bool live = airUp && state != L"paused" && state != L"nocam";
            if (live && s_air.frame) {
                auto fsz = s_air.frame->GetSize();
                float k = std::max((cam.right - cam.left) / fsz.width, (cam.bottom - cam.top) / fsz.height);
                float dw = fsz.width * k, dh = fsz.height * k, cx = (cam.left + cam.right) / 2, cy = (cam.top + cam.bottom) / 2;
                ComPtr<ID2D1RoundedRectangleGeometry> geo;
                g.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(cam, 14 * s, 14 * s), &geo);
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(), a), nullptr);
                D2D1_RECT_F dst = D2D1::RectF(cx - dw / 2, cy - dh / 2, cx + dw / 2, cy + dh / 2);
                dc->DrawBitmap(s_air.frame.Get(), &dst, 1.f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
                dc->PopLayer();
            } else {
                std::wstring msg = !airUp && !airProc ? L"Air Ring isn't running, so the camera is off."
                                 : !airUp ? L"Waiting for Air Ring to answer\u2026"
                                 : state == L"paused" ? L"The camera is off. Turn it on with the Camera switch."
                                 : state == L"nocam" ? L"Plug in a webcam. Air Ring looks for one every 5 seconds."
                                 : L"Starting the camera\u2026";
                float my = (cam.top + cam.bottom) / 2;
                text(fBodyC.Get(), msg, D2D1::RectF(cam.left + 16 * s, my - 30 * s, cam.right - 16 * s, my - 6 * s), fg2);
                if (!airUp && !airProc) {
                    std::wstring l = L"Start Air Ring";
                    float w = bw(l), cx = (cam.left + cam.right) / 2;
                    button(D2D1::RectF(cx - w / 2, my + 4 * s, cx + w / 2, my + 34 * s), l, H_AIR_START);
                }
            }
            // ---- what it sees right now
            float ry = cam.bottom + 10 * s;
            std::wstring seeing;
            if (live) {
                std::wstring vol = s_air.Get(L"volume");
                if (!vol.empty()) seeing = L"Changing the volume: " + vol + L" steps";
                else if (s_air.On(L"hand")) seeing = L"Sees " + PoseName(s_air.Get(L"pose"));
                else seeing = L"No hand in view";
                std::wstring last = s_air.Get(L"last");
                size_t sp = last.rfind(L' ');
                if (sp != std::wstring::npos && _wtoi(last.c_str() + sp + 1) < 6)
                    seeing += L"   \u00B7   Last: " + EventName(last.substr(0, sp));
            }
            text(fBody.Get(), seeing, D2D1::RectF(x0, ry, cam.right, ry + 20 * s), fg);
            text(fWrapSmall.Get(), L"Pinch thumb and pointer, hold still a moment, then move your hand up or down for volume. "
                                   L"Swipe an open hand left or right for the previous or next song.",
                 D2D1::RectF(x0, ry + 24 * s, cam.right, ry + 80 * s), fg2);

            // ---- switches and actions
            float cx0 = cam.right + 24 * s, gy = cam.top;
            const float rowH = 40;
            D2D1_RECT_F g2 = D2D1::RectF(cx0, gy, x1, gy + 3 * rowH * s);
            s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.06f * a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(g2, 12 * s, 12 * s), s_br.Get());
            s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.08f * a));
            for (int i = 1; i < 3; i++) dc->FillRectangle(D2D1::RectF(cx0 + 14 * s, gy + i * rowH * s, x1 - 14 * s, gy + i * rowH * s + 1), s_br.Get());
            toggleRow(cx0, x1, gy, rowH, L"Camera", airUp && state != L"paused", H_AIR_CAMERA, airUp);
            toggleRow(cx0, x1, gy + rowH * s, rowH, L"Pinch for volume", airUp && s_air.On(L"pinch"), H_AIR_PINCH, airUp);
            toggleRow(cx0, x1, gy + 2 * rowH * s, rowH, L"Swipe to skip songs", airUp && s_air.On(L"swipes"), H_AIR_SWIPES, airUp);
            float by = g2.bottom + 18 * s;
            button(D2D1::RectF(cx0, by, x1, by + 30 * s), L"Retrain Gestures\u2026", H_AIR_TRAIN, airUp);
            by += 38 * s;
            button(D2D1::RectF(cx0, by, x1, by + 30 * s), L"Open Settings File", H_AIR_CONFIG);
            by += 38 * s;
            button(D2D1::RectF(cx0, by, x1, by + 30 * s), L"Open Log", H_AIR_LOG);
            by += 42 * s;
            text(fWrapSmall.Get(), s_air.Get(L"model") == L"trained" ? L"Using the gestures you trained."
                                                                   : L"Pinch and swipe work without training. Training teaches it your other poses.",
                 D2D1::RectF(cx0 + 2 * s, by, x1, by + 50 * s), fg2);
        }
    });
}

void SetPane(int pane) {
    if (pane != s_pane) s_scroll = 0;
    s_pane = pane;
    s_rebuild = true;
    s_focus = -1;
    s_air.Preview(s_open && s_pane == prefs::AirGestures);
    s_lastPoll = 0;
    s_dirty = true;
}

void SetAwaySeconds(bool lock, bool on) {
    float& v = lock ? cfg.awayLockSeconds : cfg.awayPauseSeconds;
    v = on ? 20.f : 0.f;
    SetConfigValue(lock ? L"away_lock_seconds" : L"away_pause_seconds", on ? L"20" : L"0");
}

int ParentOf(int pane) { return pane == prefs::About || pane == prefs::LoginItems ? prefs::General : -1; }

void Activate(int id) {
    std::wstring state = s_air.Get(L"state");
    if (id >= H_GEN0) {
        size_t i = (size_t)(id - H_GEN0);
        if (i < s_actions.size()) { auto f = s_actions[i]; if (f) f(); }
        s_dirty = s_rebuild = true;
        return;
    }
    if (id >= H_SIDE0 && id < H_GEN0) {
        auto items = SideItems();
        size_t i = (size_t)(id - H_SIDE0);
        if (i < items.size()) SetPane(items[i].pane);
        return;
    }
    switch (id) {
        case H_CLOSE: prefs::Close(); break;
        case H_BACK: if (ParentOf(s_pane) >= 0) SetPane(ParentOf(s_pane)); break;
        case H_RELOAD: Log(L"prefs: restarting Liquid to apply settings"); prefs::Close(); g_restart = true; g_quit = true; break;
        case H_ACCOUNT: Launch(L"ms-settings:yourinfo"); prefs::Close(); break;
        case H_FACE_SETUP: prefs::Close(); lock::FaceSetup(false); break;
        case H_FACE_FORGET:
            s_modal = true;
            if (MessageBoxW(nullptr, L"Delete your saved face? Face ID stops until you set it up again.", L"Liquid",
                            MB_OKCANCEL | MB_ICONQUESTION | MB_TOPMOST) == IDOK) lock::FaceSetup(true);
            s_modal = false;
            break;
        case H_AWAY_LOCK: SetAwaySeconds(true, cfg.awayLockSeconds <= 0); break;
        case H_AWAY_PAUSE: SetAwaySeconds(false, cfg.awayPauseSeconds <= 0); break;
        case H_WAKE_LOCK:
            cfg.lockOnWake = !cfg.lockOnWake;
            SetConfigValue(L"lock_on_wake", cfg.lockOnWake ? L"on" : L"off");
            break;
        case H_PIN: prefs::Close(); lock::Lock(true); break;
        case H_LOCK_NOW: prefs::Close(); lock::Lock(); break;
        case H_AIR_START:
            if (!cfg.airRing) { cfg.airRing = true; SetConfigValue(L"air_ring", L"on"); }
            airring::Start();
            break;
        case H_AIR_CAMERA: s_air.Send(state == L"paused" ? "resume" : "pause"); break;
        case H_AIR_PINCH: s_air.Send(s_air.On(L"pinch") ? "pinch 0" : "pinch 1"); break;
        case H_AIR_SWIPES: s_air.Send(s_air.On(L"swipes") ? "swipes 0" : "swipes 1"); break;
        case H_AIR_TRAIN: s_air.Send("train"); prefs::Close(); break;
        case H_AIR_CONFIG: Launch(L"notepad.exe", L"\"" + airring::Dir() + L"config.json\""); break;
        case H_AIR_LOG: Launch(L"notepad.exe", L"\"" + airring::Dir() + L"air_ring.log\""); break;
    }
    s_dirty = true;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, s_hover >= 0 && Enabled(s_hover) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        case WM_LBUTTONDOWN:
            s_pressed = s_hover;
            for (auto& gsl : s_gsliders) if (gsl.id == s_hover) {
                s_gdrag = gsl.id; s_focus = -1; SetCapture(h);
                POINT pt; GetCursorPos(&pt);
                s_gdragVal = std::clamp(((float)(pt.x - s_ov.x) - gsl.l) / std::max(1.f, gsl.r - gsl.l), 0.f, 1.f);
                if (gsl.set) gsl.set(s_gdragVal);
            }
            s_dirty = true;
            return 0;
        case WM_LBUTTONUP: {
            int p = s_pressed;
            s_pressed = -1;
            s_dirty = true;
            if (s_gdrag >= 0) { s_gdrag = -1; ReleaseCapture(); return 0; }
            if (p >= 0 && p == s_hover && Enabled(p)) { s_focus = -1; Activate(p); }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT pt{(short)LOWORD(lp), (short)HIWORD(lp)};
            float x = (float)(pt.x - s_ov.x);
            float d = -GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 56 * s_s;
            if (x < Box().left + kSide * s_s) s_sideScroll = std::clamp(s_sideScroll + d, 0.f, s_sideMax);
            else s_scroll = std::clamp(s_scroll + d, 0.f, s_scrollMax);
            s_dirty = true;
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int PaneByName(std::wstring n) {
    n = Lower(n);
    const std::pair<const wchar_t*, int> names[] = {
        {L"face", prefs::LockFace}, {L"air", prefs::AirGestures}, {L"general", prefs::General},
        {L"about", prefs::About}, {L"login", prefs::LoginItems}, {L"wifi", prefs::WiFi}, {L"bluetooth", prefs::Bluetooth},
        {L"network", prefs::Network}, {L"energy", prefs::Energy}, {L"accessibility", prefs::Accessibility},
        {L"appearance", prefs::Appearance}, {L"controlcenter", prefs::CtrlCenter}, {L"dock", prefs::DesktopDock},
        {L"displays", prefs::Displays}, {L"menubar", prefs::MenuBar}, {L"spotlight", prefs::Spotlight},
        {L"wallpaper", prefs::Wallpaper}, {L"notifications", prefs::Notifications}, {L"sound", prefs::Sound},
        {L"focus", prefs::Focus}, {L"lockscreen", prefs::LockScreen}, {L"privacy", prefs::Privacy}, {L"users", prefs::Users},
        {L"gaming", prefs::Gaming}, {L"apps", prefs::Apps}, {L"keyboard", prefs::Keyboard}, {L"mouse", prefs::Mouse},
        {L"printers", prefs::Printers}};
    for (auto& [k, p] : names) if (n == k) return p;
    return prefs::General;
}
}  // namespace

namespace prefs {
bool Init() {
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    int w = (int)std::ceil((kW + kMargin * 2) * s_s), h = (int)std::ceil((kH + kMargin * 2) * s_s);
    if (!s_ov.Create(L"LiquidSettings", Proc, -w - 100, 0, w, h, nullptr)) return false;
    s_ov.SetVisible(false);
    s_ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_br);
    const wchar_t* ui = L"Segoe UI Variable Text";
    fTitle = MakeFont(L"Segoe UI Variable Display", 20 * s_s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fHead = MakeFont(ui, 14.5f * s_s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fBody = MakeFont(ui, 13 * s_s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fBodyC = MakeFont(ui, 13 * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fBodyR = MakeFont(ui, 13 * s_s, DWRITE_FONT_WEIGHT_NORMAL, false);   // values, right-aligned
    fBodyR->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    fSmall = MakeFont(ui, 11.5f * s_s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fIcon = MakeFont(L"Segoe Fluent Icons", 15 * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fSideIcon = MakeFont(L"Segoe Fluent Icons", 11.5f * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fChevron = MakeFont(L"Segoe Fluent Icons", 11 * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fSideName = MakeFont(ui, 13 * s_s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fWrap = MakeFont(ui, 12.5f * s_s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fWrapSmall = MakeFont(ui, 11.5f * s_s, DWRITE_FONT_WEIGHT_NORMAL, false);
    for (auto* f : {fWrap.Get(), fWrapSmall.Get()}) {   // paragraphs: wrap, start at the top
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    for (auto* f : {fBody.Get(), fBodyR.Get(), fSideName.Get(), fHead.Get()}) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    s_air.Events();
    s_air.Preview(false);
    return true;
}

void Open(int pane) {
    if (s_open && s_pane == pane) { Close(); return; }
    if (!s_ov.hwnd || lock::IsLocked()) return;
    if (!s_userLoaded) LoadUser();
    if (!s_open) {
        HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(hm, &mi);
        int x = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - s_ov.w) / 2;
        int y = mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top - s_ov.h) / 2 - (int)(10 * s_s);
        s_ov.Move(x, y);
        s_ov.SetVisible(true);
        SetWindowPos(s_ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        s_open = true;
        s_mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
        s_kbd = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
        Log(L"prefs: opened");
    }
    SetPane(pane);
}

void Close() {
    if (!s_open) return;
    s_open = false;
    s_hover = s_pressed = s_focus = -1;
    s_gdrag = -1;
    if (s_mouse) UnhookWindowsHookEx(s_mouse);
    if (s_kbd) UnhookWindowsHookEx(s_kbd);
    s_mouse = s_kbd = nullptr;
    s_air.Preview(false);
    s_dirty = true;
}

bool IsOpen() { return s_open; }

void Tick(bool nf) {
    if (!s_ov.hwnd) return;
    // Other apps can open a pane: Air Ring's tray menu sets Local\LiquidOpenAirGestures;
    // Liquid.exe --settings <pane> writes the pane's name to Local\LiquidOpenPane and sets Local\LiquidOpenSettings.
    static HANDLE paneMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 128, L"Local\\LiquidOpenPane");
    static HANDLE evLock = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidOpenLockFace");
    static HANDLE evAir = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidOpenAirGestures");
    static HANDLE evAny = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidOpenSettings");
    if (evLock && WaitForSingleObject(evLock, 0) == WAIT_OBJECT_0 && !(s_open && s_pane == prefs::LockFace)) Open(prefs::LockFace);
    if (evAir && WaitForSingleObject(evAir, 0) == WAIT_OBJECT_0 && !(s_open && s_pane == prefs::AirGestures)) Open(prefs::AirGestures);
    if (evAny && WaitForSingleObject(evAny, 0) == WAIT_OBJECT_0) {
        wchar_t name[64] = L"general";
        if (paneMap) {
            if (auto* v = (const wchar_t*)MapViewOfFile(paneMap, FILE_MAP_READ, 0, 0, 128)) {
                if (v[0]) wcsncpy_s(name, v, _TRUNCATE);
                UnmapViewOfFile(v);
            }
        }
        int p = PaneByName(name);
        if (!(s_open && s_pane == p)) Open(p);
    }
    if (s_open && lock::IsLocked()) Close();
    float pa = s_anim;
    Spring(s_anim, s_animV, s_open ? 1.f : 0.f, 1 / 60.f, 520.f);
    if (!s_open && s_anim == 0) {
        if (s_ov.visible) s_ov.SetVisible(false);
        return;
    }
    ULONGLONG now = GetTickCount64();
    bool camPane = s_open && s_pane == prefs::AirGestures;
    // Every frame while the camera shows; a couple of times a second otherwise (radios, volume and
    // the like change outside this window too).
    if (s_open && (camPane || now - s_lastPoll > 500)) {
        s_lastPoll = now;
        bool wasUp = s_air.Answering();
        if (s_air.Poll(s_ov.dc.Get(), camPane)) s_dirty = true;
        if (wasUp != s_air.Answering()) s_dirty = true;
        if (camPane) s_air.Preview(true);   // set again in case Air Ring restarted
        bool live = s_pane == prefs::WiFi || s_pane == prefs::Bluetooth || s_pane == prefs::Network || s_pane == prefs::Sound || s_pane == prefs::Energy;
        static ULONGLONG lastLive = 0;
        if (live && s_gdrag < 0 && now - lastLive > 1500) { lastLive = now; s_dirty = s_rebuild = true; }   // radios, volume, devices changed elsewhere
    }
    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y);
    // dragging a pane's slider (output volume): applied as it moves
    if (s_gdrag >= 0) {
        for (auto& gsl : s_gsliders) if (gsl.id == s_gdrag && gsl.set) {
            float k = std::clamp((x - gsl.l) / std::max(1.f, gsl.r - gsl.l), 0.f, 1.f);
            if (std::fabs(k - s_gdragVal) > 0.002f) { s_gdragVal = k; gsl.set(k); s_dirty = true; }
        }
        if (!(GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000)) { s_gdrag = -1; ReleaseCapture(); s_dirty = s_rebuild = true; }
    }
    D2D1_RECT_F b = Box();
    bool in = s_open && x >= b.left && x <= b.right && y >= b.top && y <= b.bottom;
    s_ov.SetClickThrough(!in && s_gdrag < 0);
    int hv = -1;
    if (in) for (auto& h : s_hits) if (h.enabled && x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) hv = h.id;
    if (hv != s_hover) { s_hover = hv; s_dirty = true; }
    if (s_dirty || nf || s_anim != pa || g.snapPending) {
        s_dirty = false;
        Render();
    }
}

void Shutdown() {
    Close();
    s_air.Preview(false);
    s_air.Disconnect();
    if (s_ov.hwnd) s_ov.Destroy();
}
}  // namespace prefs
