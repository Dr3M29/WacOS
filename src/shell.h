// Liquid \u2014 a macOS Tahoe-style shell layer for Windows.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <string>
#include <set>
#include <vector>
#include <memory>
#include <functional>
#include <initializer_list>

using Microsoft::WRL::ComPtr;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

// ---------- util ----------
void Log(const wchar_t* fmt, ...);
std::wstring ExeDir();
std::wstring Lower(std::wstring s);
std::wstring FileStem(const std::wstring& path);
std::wstring ProcessPath(DWORD pid);
std::wstring FileDescription(const std::wstring& path);
std::wstring ResolveLnk(const std::wstring& lnk);
std::wstring LnkArguments(const std::wstring& lnk);
DWORD WindowPid(HWND h);
bool IsAppWindow(HWND h);
void TapKeys(std::initializer_list<WORD> keys);
std::wstring HomeDir();
void Launch(const std::wstring& target, const std::wstring& args = L"", int show = SW_SHOWNORMAL);
bool LaunchViaExplorer(const std::wstring& exe, const std::wstring& args);
void ActivateWindow(HWND w);
ComPtr<IDWriteTextFormat> MakeFont(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, bool center);
float TextWidth(IDWriteTextFormat* f, const std::wstring& s);
bool IsAutostart();
void SetAutostart(bool on);
float Spring(float& x, float& v, float target, float dt, float stiffness = 380.f);

struct IconImages {
    ComPtr<ID2D1Bitmap1> color;   // original icon
    ComPtr<ID2D1Bitmap1> clear;   // white translucent glyph for glass tiles
    ComPtr<ID2D1Bitmap1> brand;   // the same glyph in the app's own colours (macOS dark mode)
    ComPtr<ID2D1Bitmap1> silver;  // Tahoe silver gradient glyph
    ComPtr<ID2D1Bitmap1> shadow;  // dark copy drawn just below the glyph for depth
    bool tile = false;            // original art was already a filled square tile
    bool custom = false;          // user-supplied icon file: drawn exactly as designed
};
IconImages LoadIcons(ID2D1DeviceContext* dc, const std::wstring& path, int size, const std::wstring& overrideImage = L"");
IconImages GlyphIcon(ID2D1DeviceContext* dc, const wchar_t* glyph);
std::wstring IconOverride(const std::wstring& name, const std::wstring& exePath);
ComPtr<ID2D1Bitmap1> DecodeImage(ID2D1DeviceContext* dc, const std::vector<BYTE>& bytes, int maxSide);

extern DWORD g_selfPid;
extern bool g_quit;
extern bool g_restart;
extern bool g_reloading;

// ---------- config ----------
struct Config {
    std::wstring logo = L"\u2726";
    bool menubarGlass = true;
    float menubarHeight = 24.f;
    float iconSize = 44.f;
    float magnify = 1.6f;
    bool finder = true;
    bool trash = true;
    std::wstring iconStyle = L"tahoe";   // clear | dark | color
    bool autohide = false;
    bool dockOverlap = true;   // windows slide under the dock (glass has something to bend)
    bool recordable = false;
    bool gestures = true;
    bool gesturesEverywhere = true;   // right-drag over normal app windows too
    bool desktopMenu = true;          // glass right-click menu on the desktop
    bool spotlight = true;            // Alt+Space opens Liquid Spotlight
    bool winKeySpotlight = true;      // tapping the Windows key opens Spotlight instead of Start
    bool lockScreen = true;           // Liquid's Mac lock screen
    bool macNotifications = true;
    bool islandNotifications = true;  // new notifications also drop into the Dynamic Island
    bool airRing = true;              // start Air Ring (webcam: Face ID, walk-away lock, air gestures)
    bool trafficLights = true;        // Mac close/minimize/zoom dots on the active window
    bool hideCaptionButtons = true;   // hide Windows' X/min/max on classic windows
    bool frostWindows = true;
    bool roundCorners = true;
    bool dynamicIsland = true;
    bool appSwitcher = true;
    bool windowAnimations = true;     // minimize into the dock, open out of it          // Mac-style Alt+Tab
    bool widgets = true;              // desktop widgets (clock, calendar, music, system)
    bool desktopIcons = true;         // Liquid's own desktop icons (Windows' are hidden)        // black pill in the menu bar: Claude status + music
    std::wstring borderlessApps = L"javaw.exe,Minecraft.Windows.exe";   // borderless full screen apps (exe names)         // force rounded corners on every window         // acrylic backdrop + dark title bars for app windows     // Liquid's banners instead of Windows' toasts
    bool lockOnStart = true, lockOnWake = true;
    float lockIdleMinutes = 5;        // 0 = never
    float awayPauseSeconds = 20, awayLockSeconds = 60;   // walk-away (Air Ring sees nobody + no input); 0 = off
    std::wstring lockPinHash, lockPinSalt;
    std::wstring lockWallpaper;       // empty = wallpapers\lock.png next to Liquid.exe; "desktop" = Windows wallpaper
    bool volumeHud = true;            // volume keys show the Mac HUD instead of the Windows flyout
    bool fullscreenSpaces = true;
    std::wstring spacesMode = L"screen";   // screen (per monitor) | desktop (Windows virtual desktops)
    std::vector<std::wstring> pins;
};
extern Config cfg;
void LoadConfig();
void SetConfigValue(const std::wstring& key, const std::wstring& value);

// ---------- graphics ----------
struct GlassShape {
    float x, y, w, h;          // window pixels
    float radius = 0;          // corner radius
    float blur = 0.6f;         // frost (mip level)
    float tint = 0.1f;         // mix toward tint colour
    float bezel = 20;          // width of the curved rim, px
    float shadow = 0;          // drop shadow opacity
    float thick = 1.6f;        // lens strength (glass thickness / bezel)
    float highlight = 1;       // rim light strength
    float opacity = 1;         // whole-shape fade
    int group = 0;             // shapes sharing a group > 0 melt together like liquid
    float melt = 16;           // how far apart they start to bridge, px
    float prism = 0;           // >0: soft rainbow light-split along the rim (visible over black)
};

struct Capture {
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> dupl;
    HMONITOR mon = nullptr;
    RECT rect{};
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    UINT w = 0, h = 0, mips = 1;
    bool have = false, live = true, fresh = false;
    bool first = false;     // just (re)started: take the first frame even if nothing new was presented
    bool wasLive = false;   // live before a remote stream switched it to the wallpaper
    ComPtr<ID3D11Texture2D> lumaStage;
    UINT lumaMip = 5, lumaW = 0, lumaH = 0;
    ULONGLONG lastTry = 0;

    bool MakeTexture(UINT w, UINT h);
    bool Start();
    bool Poll();
    bool LoadStatic(const std::wstring& imagePath);
    float Luma(const RECT& screenRect);
    ComPtr<ID3D11Texture2D> pixStage;
    bool ColorAt(POINT screenPt, float rgb[3]);
    ComPtr<ID3D11Texture2D> colStage;
    bool Column(int x, int y, int n, std::vector<float>& lum);
};

struct Gfx {
    float backdropOpacity = 0;
    ID3D11ShaderResourceView* maskSrv = nullptr;
    float maskInfo[4] = {};
    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDev;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IDCompositionDevice> dcomp;
    std::vector<std::unique_ptr<Capture>> caps;

    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11Buffer> cb;
    ComPtr<ID3D11SamplerState> samp;

    struct Layer { int x, y, w, h; std::vector<BYTE> px; };
    bool snapPending = false;
    std::vector<Layer> snapLayers;

    // Liquid's windows stay out of screen capture so the glass can read the live desktop under
    // them without seeing itself. While a remote stream (Moonlight, RustDesk) is running they
    // have to be in it, so the glass bends the wallpaper instead (remote.cpp turns this on).
    bool remoteView = false;
    std::vector<HWND> ownWindows;
    void ApplyCaptureMode(HWND h);   // every Liquid window calls this once, right after it's created
    void SetRemoteView(bool on);

    bool Init();
    Capture* CapFor(HMONITOR m);
    bool PollAll();
    void DrawGlass(Capture* cap, ID3D11RenderTargetView* rtv, int wx, int wy, int ww, int wh, float scale,
                   const std::vector<GlassShape>& shapes, const float tint[3]);
    void GrabLayer(ID3D11Texture2D* src, int x, int y, int w, int h);
    bool SaveSnapshot(const std::wstring& path);
};
extern Gfx g;

struct Overlay {
    HWND hwnd = nullptr;
    int x = 0, y = 0, w = 0, h = 0;
    Capture* cap = nullptr;
    ComPtr<IDXGISwapChain1> sc;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Bitmap1> d2dTarget;
    bool clickThrough = false, visible = true;
    ID3D11ShaderResourceView* mask = nullptr;   // glass text mask (see shader maskInfo)
    float maskInfo[4] = {};
    float backdrop = 0;   // >0: draw the capture texture itself under the glass, at this opacity (lock screen)

    bool Create(const wchar_t* cls, WNDPROC proc, int x, int y, int w, int h, void* self);
    void Move(int x, int y);
    void Render(float scale, const std::vector<GlassShape>& shapes, const float tint[3],
                const std::function<void(ID2D1DeviceContext*)>& ui);
    void SetClickThrough(bool ct);
    void SetVisible(bool v);
    void Destroy();
};

// ---------- glass popup menu ----------
struct MenuEntry {
    int id = 0;                // 0 = separator
    std::wstring text;
    bool checked = false;
    bool enabled = true;
    std::wstring shortcut;           // right-aligned grey hint, e.g. L"⌘Q" or L"Ctrl+R"
    std::vector<MenuEntry> sub;      // non-empty = cascading submenu
};
// Blocks (while the rest of the shell keeps animating) until an item is chosen or the menu is dismissed.
int GlassMenu(int x, int y, const std::vector<MenuEntry>& items);
bool GlassMenuOpen();
bool DesktopMenu(POINT pt);   // glass right-click menu for the desktop (false = let Windows show its own)

// ---------- system (audio, wi-fi, radios, media) ----------
struct AudioDevice { std::wstring id, name; bool isDefault = false; };
struct WifiNet { std::wstring ssid, profile; int signal = 0; bool secure = false, connected = false; };
struct MediaInfo {
    bool present = false, playing = false;
    std::wstring title, artist, app;
    std::vector<BYTE> art;     // encoded thumbnail
    int artVersion = 0;
};
struct Notif { uint32_t id = 0; std::wstring app, aumid, title, body; long long time = 0; };
namespace nc {   // Notification Center (clock click)
    bool Init();
    void Toggle(RECT mon);
    void Close();
    bool IsOpen();
    void Tick(bool nf);
    void Shutdown();
}
namespace lock {   // Mac-style lock screen with a Liquid PIN
    bool Init();
    void Lock(bool setPin = false, bool instant = false);   // instant: no fade-in (boot)
    bool IsLocked();
    void Key(int vk, wchar_t ch);
    void Tick(bool nf);
    void Shutdown();
    void FaceSetup(bool forget);
    void FaceTimes(double& seen, double& ok, double& fail);   // GetTickCount64 seconds; ok = 0 until matched   // asks Air Ring (it owns the webcam) to enroll or delete your face
}
namespace chrome {   // Mac traffic lights, hidden caption buttons, frosted windows
    bool Init();
    bool IsBorderlessApp(HWND h);
    void ToggleBorderlessApp(HWND h);
    void ToggleBorderlessWindow(HWND h);   // Ctrl+Win+F
    void Tick(bool nf);
    void Shutdown();
}
struct WidgetPos { std::wstring type; int x = 0, y = 0; };
struct IconPos { std::wstring name, target; int x = 0, y = 0; };
namespace layout {   // layout.txt: where widgets and desktop icons live
    extern std::vector<WidgetPos> widgets;
    extern std::vector<IconPos> icons, launch;
    extern std::set<std::wstring> seen;
    extern bool exists;
    void Load();
    void Save();
}
namespace widgets {
    bool Init(); void Tick(bool nf); void Shutdown();
    void SetEditing(bool on);   // Edit Widgets mode: drag, remove, add
    bool Editing();
}
namespace deskicons {
    bool Init(); void Tick(bool nf); void Shutdown();
    void Add(const std::wstring& name, const std::wstring& target);
}
void SetWindowsDesktopIcons(bool show);
bool ItemMenu(const std::wstring& path, POINT pt);
namespace switcher {   // Cmd+Tab style app switcher on Alt+Tab
    bool IsOpen();
    void Command(int c);   // 1 next, 2 previous, 3 switch, 4 cancel, 5 quit selected
    void Tick(bool nf);
    void Shutdown();
}
namespace anim {   // windows shrink into / grow out of the dock
    bool Init();
    void Tick(bool nf);
    void Shutdown();
}
namespace island {   // Dynamic Island
    bool Init();
    void Raise();   // above the lock screen (Face ID square)
    void Tick(bool nf);
    void Shutdown();
    void DrawFaceIdGlyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float cx, float cy, float half, float alpha);
    void Notify(const Notif& n);   // show a new notification in the island for a few seconds
}
namespace airring {  // Air Ring (air-ring\ next to Liquid.exe): webcam for Face ID, walk-away, air gestures
    std::wstring Dir();   // ends with a backslash
    bool Installed();
    bool Running();
    bool Start();
    void Tick();
}
namespace about {   // About This PC: what this computer is, read from the hardware
    bool Init(); void Open(); void Close(); bool IsOpen(); void Tick(bool nf); void Shutdown();
    // the same facts for System Settings > General > About
    void Specs(std::wstring& title, std::wstring& subtitle, std::vector<std::pair<std::wstring, std::wstring>>& rows);
}
std::wstring ScreenWallpaper(const RECT& rc, const std::wstring& fallback, int& pos);   // gfx.cpp
namespace prefs {   // System Settings: macOS's sidebar of panes; Liquid's settings native, Windows' open its own pages
    enum Pane { LockFace = 0, AirGestures = 1, General, About, LoginItems, WiFi, Bluetooth, Network, Energy,
                Accessibility, Appearance, CtrlCenter, DesktopDock, Displays, MenuBar, Spotlight, Wallpaper,
                Notifications, Sound, Focus, LockScreen, Privacy, Users, Gaming, Apps, Keyboard, Mouse, Printers, PaneCount };
    bool Init();
    void Open(int pane);    // opening the pane that's already showing closes it (menu toggle)
    void Close();
    bool IsOpen();
    void Tick(bool nf);
    void Shutdown();
}
namespace spotlight {
    bool Init();
    void Toggle();
    bool IsOpen();
    void Key(int vk, wchar_t ch);   // from the keyboard hook while open
    bool AppsReady();
    std::wstring FindApp(const std::wstring& name, std::wstring* display = nullptr);   // shell:AppsFolder\... or empty
    void Tick(bool nf);
    void Shutdown();
}
namespace sys {
    void Init();
    void Shutdown();
    float Volume();               // 0..1
    void SetVolume(float v);
    bool Muted();
    void SetMuted(bool m);
    void AudioTick();             // keeps new apps at Liquid's volume (AudioRelay mode)
    std::vector<AudioDevice> OutputDevices();
    void SetDefaultOutput(const std::wstring& id);
    bool WifiAvailable();
    std::wstring WifiConnectedSsid();
    std::vector<WifiNet> WifiNetworks();
    void WifiScan();
    void WifiConnect(const std::wstring& profile);
    bool EthernetUp();
    // radios + media live on a background WinRT thread
    int WifiRadio();              // -1 unknown, 0 off, 1 on
    int BluetoothRadio();
    void SetWifiRadio(bool on);
    void SetBluetoothRadio(bool on);
    MediaInfo Media();
    void MediaCommand(int cmd);   // 0 prev, 1 play/pause, 2 next
    std::vector<Notif> Notifications();   // newest first
    int NotificationAccess();             // -1 unknown, 0 not allowed, 1 allowed
    void DismissNotification(uint32_t id);
    void ClearNotifications();
}

// ---------- control centre ----------
struct ControlCenter {
    Overlay ov;
    float s = 1;
    bool open = false;
    int section = 0;              // 0 main, 1 wifi list, 2 sound devices
    float anim = 0, animV = 0;
    RECT mon{};
    ComPtr<IDWriteTextFormat> fTitle, fBody, fSmall, fIcon, fIconBig;
    ComPtr<ID2D1SolidColorBrush> br;
    ComPtr<ID2D1Bitmap1> art;
    int artVersion = -1;
    struct Hit { D2D1_RECT_F r; int id; };
    std::vector<Hit> hits;
    int hover = -1, pressed = -1;
    bool dragVolume = false;
    float volCache = 0.5f;
    ULONGLONG lastPoll = 0, openedAt = 0;
    std::vector<AudioDevice> devs;
    std::vector<WifiNet> nets;
    bool dirty = true;
    void* hook = nullptr;

    bool Init(float scale);
    void Toggle(RECT monitor, int startSection);
    void Close();
    void Tick(bool newFrame);
    void Render();
    void OnClick(int id, float x);
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
};

// ---------- menu bar (one per monitor) ----------
namespace appmenu {   // the menu bar's File/Edit/View/Window/Help as glass menus
    struct Top { std::wstring title; HMENU sub = nullptr; int builtin = -1; };
    std::vector<Top> TopsFor(HWND w, const std::wstring& app);
    void Open(HWND w, const Top& t, const std::wstring& app, int x, int y);
}

struct MenuBar {
    Overlay ov;
    RECT mon{};
    bool primary = false;
    float s = 1.f;
    int H = 24;
    std::wstring app = L"Finder";
    HWND lastFg = nullptr;       // the window this bar's menus act on (the active app on this screen)
    std::vector<appmenu::Top> tops;
    HWND topsFor = (HWND)-1;
    ULONGLONG lastTarget = 0;
    bool darkText = false;
    ULONGLONG lastLuma = 0;
    int lastMinute = -1;
    std::wstring clock;
    std::wstring wifiGlyph = L"\uE701", volGlyph = L"\uE767";
    ComPtr<IDWriteTextFormat> fBold, fReg, fIcon, fLogo;
    ComPtr<ID2D1SolidColorBrush> br;
    struct Hit { D2D1_RECT_F r; int id; };
    std::vector<Hit> hits;
    int hover = -1, pressed = -1;
    bool dirty = true, hidden = false;

    bool Init(RECT mon, float scale, bool primary);
    void Tick(bool newFrame);
    void Render();
    void Click(int id);
    void Shutdown();
    std::wstring AppNameFor(HWND w);
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
};

// ---------- dock ----------
struct DockItem {
    std::wstring launch, iconPath, match, matchDir, name;
    bool pinned = false, trash = false;
    IconImages icons;
    std::vector<HWND> wins;
    double bounceStart = -1;
    int bounceCycle = 0;
    // animated layout
    float size = 0, sizeV = 0, cx = 0, sepX = 0;
    float press = 0, pressV = 0, dot = 0, dotV = 0;
    bool sepBefore = false;
};

struct Dock {
    Overlay ov;
    RECT mon{};
    float s = 1.f, S = 54, padIn = 7, gap = 5, bgH = 68, margin = 6, M = 1.6f;
    int W = 0, H = 0;
    std::vector<DockItem> items;
    float hoverAmt = 0, hoverV = 0, mx = -1, lastX = -1, lastY = -1;
    float slide = 0, slideV = 0;          // auto-hide offset (0 shown, 1 hidden)
    float labelA = 0, labelV = 0;
    float jelly = 0, jellyV = 0;          // elastic wobble when the pointer arrives
    int labelIdx = -1;
    bool inside = false;
    int hoverIdx = -1, pressIdx = -1;
    D2D1_RECT_F bg{};
    float iconBottom = 0;
    ULONGLONG lastScan = 0, lastLuma = 0, leftAt = 0;
    bool light = false;
    bool dirty = true, hidden = false;
    bool fsHide = false;                  // a full-screen app covers this screen: auto-hide, reveal at the edge
    ComPtr<IDWriteTextFormat> fLabel;
    ComPtr<ID2D1SolidColorBrush> br;
    ComPtr<ID2D1LinearGradientBrush> tileBrush, sheenBrush;
    ComPtr<ID2D1PathGeometry> squircle;   // unit superellipse (Apple's continuous-corner icon shape)
    D2D1_COLOR_F accent{0.04f, 0.52f, 1.0f, 1.0f};

    bool Init(RECT mon, float scale);
    void LoadPins();
    void Scan();
    void Layout(float dt);
    int HitTest(float x, float y);
    float Bounce(const DockItem& it, double now);
    void Tick(bool newFrame);
    void Render();
    void Click(int idx);
    void RightClick(int idx);
    void BackgroundMenu();
    void Reserve();
    void Shutdown();
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
};

// ---------- gestures (right-drag to switch desktops) ----------
#define WM_LIQUID_GESTURE (WM_APP + 10)
#define WM_LIQUID_VOLKEY (WM_APP + 11)
#define WM_LIQUID_BORDERLESS (WM_APP + 14)   // Ctrl+Win+F
#define WM_LIQUID_SWITCH (WM_APP + 15)       // app switcher: wParam 1 next, 2 previous, 3 commit, 4 cancel
#define WM_LIQUID_LOCKKEY (WM_APP + 13)   // wParam 0 = lock now, else vk; lParam = typed char
#define WM_LIQUID_SPOTKEY (WM_APP + 12)   // wParam 0 = toggle, else vk; lParam = typed char

namespace osd {
    void ShowVolume();
    void VolumeKey(int vk);
    void Tick(bool nf);
    void Shutdown();
}
namespace gestures {
    void Init();
    void Shutdown();
    void Handle(WPARAM g, LPARAM pt);
}

namespace spaces {
    bool Init();
    void Tick();
    void Shutdown();
    bool PerScreen();
    bool IsHidden(HWND h);
    bool Switch(HMONITOR m, int dir);
    void Reveal(HWND w);
}

extern std::vector<std::unique_ptr<MenuBar>> menubars;
extern Dock dock;
extern ControlCenter cc;
void TickShell();   // one frame of everything (used by nested menu loops too)

namespace remote {   // is someone watching this screen through Moonlight / RustDesk / Remote Desktop?
    void Tick();
}
namespace streamglass {   // live glass during a stream, rebuilt from per-window captures
    void Start(std::vector<std::unique_ptr<Capture>>& caps);
    void Stop();
    bool Poll();          // true when a screen's backdrop changed
}
