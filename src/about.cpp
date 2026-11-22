#include "shell.h"
#include <shellscalingapi.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

// About This PC: the Apple menu's About This Mac, for this PC. A small glass window with a
// picture of the screen (showing your own wallpaper), what kind of PC it is, and the parts that
// matter: processor, graphics, memory, storage and Windows. Everything is read from the machine
// (registry, SMBIOS, DXGI, the disk) when the window opens; nothing is typed in.

namespace {
const float kW = 320, kMargin = 40;
float s_hgt = 520;                 // grows to fit the rows
Overlay s_ov;
float s_s = 1, s_anim = 0, s_animV = 0;
bool s_open = false, s_dirty = true;
ComPtr<ID2D1SolidColorBrush> s_br;
ComPtr<IDWriteTextFormat> fTitle, fSub, fLabel, fValue, fBtn, fIcon;
ComPtr<ID2D1Bitmap1> s_wall;
HHOOK s_mouse = nullptr, s_kbd = nullptr;
struct Hit { D2D1_RECT_F r; int id; };
std::vector<Hit> s_hits;
int s_hover = -1, s_pressed = -1;
enum { H_CLOSE = 1, H_MORE };

struct Row { std::wstring label, value; };
struct Info {
    std::wstring title, subtitle;
    std::vector<Row> rows;
} s_info;

std::wstring Reg(HKEY root, const wchar_t* key, const wchar_t* name) {
    wchar_t buf[512];
    DWORD sz = sizeof(buf);
    if (RegGetValueW(root, key, name, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}
DWORD RegDword(HKEY root, const wchar_t* key, const wchar_t* name) {
    DWORD v = 0, sz = sizeof(v);
    RegGetValueW(root, key, name, RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v;
}
std::wstring Squash(std::wstring s) {   // trims, and turns runs of spaces into one
    std::wstring o;
    for (wchar_t c : s) if (!(c == L' ' && (o.empty() || o.back() == L' '))) o += c;
    while (!o.empty() && o.back() == L' ') o.pop_back();
    return o;
}
void Erase(std::wstring& s, const wchar_t* what) {
    for (size_t p; (p = s.find(what)) != std::wstring::npos;) s.erase(p, wcslen(what));
}
// Board makers leave these in the model fields of home-built PCs.
bool Placeholder(const std::wstring& v) {
    std::wstring l = Lower(v);
    for (const wchar_t* p : {L"system product name", L"to be filled", L"default string", L"o.e.m", L"not applicable",
                             L"system version", L"not specified", L"none"})
        if (l.find(p) != std::wstring::npos) return true;
    return Squash(l).empty();
}
std::wstring ShortMaker(const std::wstring& m) {
    std::wstring l = Lower(m);
    if (l.find(L"asustek") != std::wstring::npos) return L"ASUS";
    if (l.find(L"micro-star") != std::wstring::npos) return L"MSI";
    if (l.find(L"gigabyte") != std::wstring::npos) return L"Gigabyte";
    if (l.find(L"asrock") != std::wstring::npos) return L"ASRock";
    std::wstring o = m;
    for (const wchar_t* t : {L" COMPUTER INC.", L" Computer Inc.", L" Inc.", L" INC.", L" Corporation", L" CORPORATION", L" Co., Ltd.", L" Ltd."}) Erase(o, t);
    return Squash(o);
}
std::wstring Bytes(unsigned long long b) {   // decimal units, like Finder and About This Mac
    wchar_t buf[32];
    double gb = b / 1e9;
    if (gb >= 1000) {
        double tb = gb / 1000;
        swprintf(buf, 32, std::fabs(tb - std::round(tb)) < 0.05 ? L"%.0f TB" : L"%.1f TB", tb);
    } else swprintf(buf, 32, L"%.0f GB", gb);
    return buf;
}

// Memory type and speed from the SMBIOS memory-device records (type 17).
void MemoryDetails(std::wstring& type, int& mts) {
    UINT sz = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
    if (!sz) return;
    std::vector<BYTE> buf(sz);
    if (GetSystemFirmwareTable('RSMB', 0, buf.data(), sz) != sz || sz < 8) return;
    DWORD len = *(DWORD*)(buf.data() + 4);
    BYTE* p = buf.data() + 8;
    BYTE* end = p + std::min<size_t>(len, sz - 8);
    while (p + 4 <= end) {
        BYTE t = p[0], l = p[1];
        if (t == 127 || l < 4) break;
        if (t == 17 && l >= 0x17 && p + l <= end) {
            WORD size = *(WORD*)(p + 0x0C);
            if (size != 0 && size != 0xFFFF) {
                BYTE mt = p[0x12];
                WORD speed = *(WORD*)(p + 0x15);
                if (l >= 0x22 && *(WORD*)(p + 0x20)) speed = *(WORD*)(p + 0x20);   // configured speed
                switch (mt) {
                    case 0x18: type = L"DDR3"; break;
                    case 0x1A: type = L"DDR4"; break;
                    case 0x1D: type = L"LPDDR3"; break;
                    case 0x1E: type = L"LPDDR4"; break;
                    case 0x22: type = L"DDR5"; break;
                    case 0x23: type = L"LPDDR5"; break;
                }
                if (speed) mts = speed;
            }
        }
        BYTE* q = p + l;   // then the record's strings, ended by two zero bytes
        while (q + 1 < end && !(q[0] == 0 && q[1] == 0)) q++;
        p = q + 2;
    }
}

void Gather() {
    Info in;
    const wchar_t* bios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";
    std::wstring maker = Squash(Reg(HKEY_LOCAL_MACHINE, bios, L"SystemManufacturer"));
    std::wstring product = Squash(Reg(HKEY_LOCAL_MACHINE, bios, L"SystemProductName"));
    std::wstring family = Squash(Reg(HKEY_LOCAL_MACHINE, bios, L"SystemFamily"));
    std::wstring boardMaker = Squash(Reg(HKEY_LOCAL_MACHINE, bios, L"BaseBoardManufacturer"));
    std::wstring board = Squash(Reg(HKEY_LOCAL_MACHINE, bios, L"BaseBoardProduct"));
    SYSTEM_POWER_STATUS ps{};
    bool laptop = GetSystemPowerStatus(&ps) && ps.BatteryFlag != 128 && ps.BatteryFlag != 255;
    if (!Placeholder(product)) {
        in.title = (Lower(product).find(Lower(ShortMaker(maker))) == std::wstring::npos && !Placeholder(maker))
                       ? ShortMaker(maker) + L" " + product : product;
        in.subtitle = !Placeholder(family) ? family : laptop ? L"Laptop" : L"Desktop";
    } else {
        // a home-built PC: the board is the one part that names the machine
        in.title = laptop ? L"Laptop" : L"Desktop PC";
        in.subtitle = Placeholder(board) ? L"" : ShortMaker(boardMaker) + L" " + board;
    }

    std::wstring cpu = Reg(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString");
    for (const wchar_t* t : {L"(R)", L"(TM)", L"(tm)"}) Erase(cpu, t);
    cpu = Squash(cpu);
    if (cpu.size() > 10 && cpu.compare(cpu.size() - 10, 10, L" Processor") == 0) cpu.erase(cpu.size() - 10);
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    if (!cpu.empty()) in.rows.push_back({L"Processor", cpu + L", " + std::to_wstring(si.dwNumberOfProcessors) + L" threads"});

    std::wstring gpus;
    ComPtr<IDXGIFactory1> fac;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) {
        ComPtr<IDXGIAdapter1> ad;
        std::vector<std::wstring> seen;
        for (UINT i = 0; fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; i++, ad.Reset()) {
            DXGI_ADAPTER_DESC1 d;
            if (FAILED(ad->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
            if (d.DedicatedVideoMemory < (256ull << 20)) continue;   // virtual display drivers
            std::wstring n = Squash(d.Description);
            if (std::find(seen.begin(), seen.end(), n) != seen.end()) continue;
            seen.push_back(n);
            wchar_t vram[32];
            swprintf(vram, 32, L" %.0f GB", std::round(d.DedicatedVideoMemory / double(1ull << 30)));
            gpus += (gpus.empty() ? L"" : L"\n") + n + vram;
        }
    }
    if (!gpus.empty()) in.rows.push_back({L"Graphics", gpus});

    ULONGLONG kb = 0;
    if (GetPhysicallyInstalledSystemMemory(&kb) && kb) {
        std::wstring type;
        int mts = 0;
        MemoryDetails(type, mts);
        std::wstring mem = std::to_wstring((unsigned long long)std::llround(kb / 1048576.0)) + L" GB";
        if (!type.empty()) mem += L" " + type;
        if (mts) mem += L" " + std::to_wstring(mts) + L" MT/s";
        in.rows.push_back({L"Memory", mem});
    }

    wchar_t sysDrive[8] = L"C:";
    GetEnvironmentVariableW(L"SystemDrive", sysDrive, 8);
    ULARGE_INTEGER freeB{}, total{};
    if (GetDiskFreeSpaceExW((std::wstring(sysDrive) + L"\\").c_str(), &freeB, &total, nullptr))
        in.rows.push_back({L"Storage", Bytes(freeB.QuadPart) + L" available of " + Bytes(total.QuadPart)});

    const wchar_t* nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring prod = Reg(HKEY_LOCAL_MACHINE, nt, L"ProductName"), ver = Reg(HKEY_LOCAL_MACHINE, nt, L"DisplayVersion");
    int build = _wtoi(Reg(HKEY_LOCAL_MACHINE, nt, L"CurrentBuild").c_str());
    if (build >= 22000) {   // Windows 11 still calls itself "Windows 10" in the registry
        size_t p = prod.find(L"Windows 10");
        if (p != std::wstring::npos) prod.replace(p, 10, L"Windows 11");
    }
    if (prod.rfind(L"Windows ", 0) == 0) prod.erase(0, 8);
    std::wstring win = prod + (ver.empty() ? L"" : L" " + ver);
    if (build) win += L"\nBuild " + std::to_wstring(build) + L"." + std::to_wstring(RegDword(HKEY_LOCAL_MACHINE, nt, L"UBR"));
    in.rows.push_back({L"Windows", win});
    s_info = in;
}

void LoadWallpaper() {
    s_wall.Reset();
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    wchar_t fallback[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, fallback, 0);
    int pos = 0;
    std::wstring path = ScreenWallpaper(mi.rcMonitor, fallback, pos);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    GetFileSizeEx(f, &sz);
    std::vector<BYTE> bytes;
    if (sz.QuadPart > 0 && sz.QuadPart < (64ll << 20)) {
        bytes.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, bytes.data(), (DWORD)bytes.size(), &got, nullptr) || got != bytes.size()) bytes.clear();
    }
    CloseHandle(f);
    if (!bytes.empty()) s_wall = DecodeImage(s_ov.dc.Get(), bytes, (int)(420 * s_s));
}

float RowsHeight(float valueW) {   // how tall the spec rows are once long values wrap
    float h = 0;
    for (auto& r : s_info.rows) {
        ComPtr<IDWriteTextLayout> tl;
        g.dw->CreateTextLayout(r.value.c_str(), (UINT32)r.value.size(), fValue.Get(), valueW, 1000, &tl);
        DWRITE_TEXT_METRICS m{};
        if (tl) tl->GetMetrics(&m);
        h += std::max(m.height, 18 * s_s) + 7 * s_s;
    }
    return h;
}

D2D1_RECT_F Box() {
    float m = kMargin * s_s, yoff = (1 - s_anim) * 12 * s_s;
    return D2D1::RectF(m, m + yoff, m + kW * s_s, m + yoff + s_hgt * s_s);
}

LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_open && !GlassMenuOpen() && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        D2D1_RECT_F b = Box();
        RECT box{s_ov.x + (int)b.left, s_ov.y + (int)b.top, s_ov.x + (int)b.right, s_ov.y + (int)b.bottom};
        if (!PtInRect(&box, m->pt)) about::Close();
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && s_open && wp == WM_KEYDOWN && ((KBDLLHOOKSTRUCT*)lp)->vkCode == VK_ESCAPE) {
        about::Close();
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void Render() {
    s_hits.clear();
    float s = s_s, a = s_anim;
    D2D1_RECT_F box = Box();
    std::vector<GlassShape> sh;
    GlassShape gs{box.left, box.top, kW * s, s_hgt * s};
    gs.radius = 26 * s; gs.blur = 2.6f; gs.tint = -0.4f; gs.bezel = 18 * s; gs.thick = 2.2f;
    gs.shadow = 0.32f; gs.highlight = 0.0f; gs.opacity = a;
    sh.push_back(gs);
    float tint[3] = {0.1f, 0.1f, 0.12f};
    const D2D1_COLOR_F fg = D2D1::ColorF(1, 1, 1, 0.95f * a), fg2 = D2D1::ColorF(1, 1, 1, 0.66f * a);

    s_ov.Render(s, sh, tint, [&](ID2D1DeviceContext* dc) {
        ComPtr<ID2D1RoundedRectangleGeometry> clipGeo;
        g.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(box, 26 * s, 26 * s), &clipGeo);
        dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), clipGeo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), a), nullptr);
        s_br->SetColor(D2D1::ColorF(0, 0, 0, 0.5f));   // text stays readable over a white page behind
        dc->FillRectangle(box, s_br.Get());
        dc->PopLayer();
        auto text = [&](IDWriteTextFormat* f, const std::wstring& t, D2D1_RECT_F r, D2D1_COLOR_F c) {
            s_br->SetColor(c);
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };

        // traffic lights: close works; minimise and zoom are greyed out, as on About This Mac
        for (int i = 0; i < 3; i++) {
            D2D1_POINT_2F c = D2D1::Point2F(box.left + (22 + i * 20) * s, box.top + 22 * s);
            s_br->SetColor(i == 0 ? D2D1::ColorF(1.0f, 0.373f, 0.341f, a) : D2D1::ColorF(1, 1, 1, 0.2f * a));
            dc->FillEllipse(D2D1::Ellipse(c, 6.5f * s, 6.5f * s), s_br.Get());
            if (i == 0) {
                s_hits.push_back({D2D1::RectF(c.x - 11 * s, c.y - 11 * s, c.x + 11 * s, c.y + 11 * s), H_CLOSE});
                if (s_hover == H_CLOSE) text(fIcon.Get(), L"", D2D1::RectF(c.x - 6 * s, c.y - 6 * s, c.x + 6 * s, c.y + 6 * s), D2D1::ColorF(0.3f, 0, 0, 0.8f * a));
            }
        }

        // the screen, showing your wallpaper, on a stand
        float cx = (box.left + box.right) / 2, top = box.top + 46 * s;
        float dw = 184 * s, dh = 116 * s;
        D2D1_RECT_F disp = D2D1::RectF(cx - dw / 2, top, cx + dw / 2, top + dh);
        D2D1_RECT_F neck = D2D1::RectF(cx - 18 * s, disp.bottom, cx + 18 * s, disp.bottom + 20 * s);
        s_br->SetColor(D2D1::ColorF(0.72f, 0.73f, 0.75f, a));   // aluminium stand
        dc->FillRectangle(neck, s_br.Get());
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 36 * s, neck.bottom - 1 * s, cx + 36 * s, neck.bottom + 4 * s), 2 * s, 2 * s), s_br.Get());
        s_br->SetColor(D2D1::ColorF(0, 0, 0, 0.25f * a));
        dc->FillRectangle(D2D1::RectF(neck.left, neck.top, neck.right, neck.top + 4 * s), s_br.Get());   // shade under the screen
        s_br->SetColor(D2D1::ColorF(0.05f, 0.05f, 0.06f, a));   // black bezel
        dc->FillRoundedRectangle(D2D1::RoundedRect(disp, 9 * s, 9 * s), s_br.Get());
        s_br->SetColor(D2D1::ColorF(0.6f, 0.61f, 0.64f, 0.9f * a));   // the aluminium edge
        dc->DrawRoundedRectangle(D2D1::RoundedRect(disp, 9 * s, 9 * s), s_br.Get(), 1.2f * s);
        D2D1_RECT_F scr = D2D1::RectF(disp.left + 6 * s, disp.top + 6 * s, disp.right - 6 * s, disp.bottom - 6 * s);
        if (s_wall) {
            auto sz = s_wall->GetSize();
            float k = std::max((scr.right - scr.left) / sz.width, (scr.bottom - scr.top) / sz.height);
            float sw = (scr.right - scr.left) / k, shh = (scr.bottom - scr.top) / k;
            D2D1_RECT_F src = D2D1::RectF((sz.width - sw) / 2, (sz.height - shh) / 2, (sz.width + sw) / 2, (sz.height + shh) / 2);
            ComPtr<ID2D1RoundedRectangleGeometry> geo;
            g.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(scr, 4 * s, 4 * s), &geo);
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
            dc->DrawBitmap(s_wall.Get(), &scr, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, &src, nullptr);
            dc->PopLayer();
        } else {
            s_br->SetColor(D2D1::ColorF(0.13f, 0.14f, 0.18f, a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(scr, 4 * s, 4 * s), s_br.Get());
        }

        // what it is
        float y = neck.bottom + 22 * s;
        text(fTitle.Get(), s_info.title, D2D1::RectF(box.left + 16 * s, y, box.right - 16 * s, y + 32 * s), fg);
        y += 32 * s;
        if (!s_info.subtitle.empty()) text(fSub.Get(), s_info.subtitle, D2D1::RectF(box.left + 16 * s, y, box.right - 16 * s, y + 18 * s), fg2);
        y += 34 * s;

        // the parts
        float lx = box.left + 104 * s, vx = lx + 10 * s, vr = box.right - 20 * s;
        for (auto& r : s_info.rows) {
            ComPtr<IDWriteTextLayout> tl;
            g.dw->CreateTextLayout(r.value.c_str(), (UINT32)r.value.size(), fValue.Get(), vr - vx, 1000, &tl);
            DWRITE_TEXT_METRICS m{};
            if (tl) tl->GetMetrics(&m);
            float h = std::max(m.height, 18 * s);
            text(fLabel.Get(), r.label, D2D1::RectF(box.left + 12 * s, y, lx, y + 18 * s), fg);
            if (tl) { s_br->SetColor(fg2); dc->DrawTextLayout(D2D1::Point2F(vx, y), tl.Get(), s_br.Get()); }
            y += h + 7 * s;
        }

        // More Info... opens System Settings > General > About
        y += 12 * s;
        std::wstring more = L"More Info…";
        float bw = TextWidth(fBtn.Get(), more) + 30 * s;
        D2D1_RECT_F b = D2D1::RectF(cx - bw / 2, y, cx + bw / 2, y + 28 * s);
        s_hits.push_back({b, H_MORE});
        float alpha = s_pressed == H_MORE && s_hover == H_MORE ? 0.08f : s_hover == H_MORE ? 0.22f : 0.14f;
        s_br->SetColor(D2D1::ColorF(1, 1, 1, alpha * a));
        dc->FillRoundedRectangle(D2D1::RoundedRect(b, 7 * s, 7 * s), s_br.Get());
        text(fBtn.Get(), more, b, fg);
    });
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, s_hover >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        case WM_LBUTTONDOWN: s_pressed = s_hover; s_dirty = true; return 0;
        case WM_LBUTTONUP: {
            int p = s_pressed;
            s_pressed = -1;
            s_dirty = true;
            if (p >= 0 && p == s_hover) {
                if (p == H_CLOSE) about::Close();
                else if (p == H_MORE) { about::Close(); prefs::Open(prefs::About); }
            }
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}
}  // namespace

namespace about {
bool Init() {
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    int w = (int)std::ceil((kW + kMargin * 2) * s_s), h = (int)std::ceil((640 + kMargin * 2) * s_s);
    if (!s_ov.Create(L"LiquidAbout", Proc, -w - 100, 0, w, h, nullptr)) return false;
    s_ov.SetVisible(false);
    s_ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_br);
    const wchar_t* ui = L"Segoe UI Variable Text";
    fTitle = MakeFont(L"Segoe UI Variable Display", 24 * s_s, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    fSub = MakeFont(ui, 12 * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fLabel = MakeFont(ui, 12.5f * s_s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fLabel->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    fValue = MakeFont(ui, 12.5f * s_s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fValue->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fValue->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    fBtn = MakeFont(ui, 13 * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fIcon = MakeFont(L"Segoe Fluent Icons", 8 * s_s, DWRITE_FONT_WEIGHT_NORMAL, true);
    return true;
}

void Open() {
    if (!s_ov.hwnd || lock::IsLocked()) return;
    if (s_open) { Close(); return; }
    Gather();
    LoadWallpaper();
    // height: picture and name (about 250), the rows, the button
    s_hgt = std::min(640.f, (250 * s_s + 34 * s_s + RowsHeight((kW - 134) * s_s) + 70 * s_s) / s_s);
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    int x = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - s_ov.w) / 2;
    int y = mi.rcMonitor.top + (mi.rcMonitor.bottom - mi.rcMonitor.top - (int)((s_hgt + kMargin * 2) * s_s)) / 2 - (int)(30 * s_s);
    s_ov.Move(x, y);
    s_ov.SetVisible(true);
    SetWindowPos(s_ov.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    s_open = true;
    s_mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    s_kbd = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    s_dirty = true;
    Log(L"about: opened (%s, %d rows)", s_info.title.c_str(), (int)s_info.rows.size());
}

void Close() {
    if (!s_open) return;
    s_open = false;
    s_hover = s_pressed = -1;
    if (s_mouse) UnhookWindowsHookEx(s_mouse);
    if (s_kbd) UnhookWindowsHookEx(s_kbd);
    s_mouse = s_kbd = nullptr;
    s_dirty = true;
}

bool IsOpen() { return s_open; }

void Specs(std::wstring& title, std::wstring& subtitle, std::vector<std::pair<std::wstring, std::wstring>>& rows) {
    if (s_info.rows.empty()) Gather();
    title = s_info.title;
    subtitle = s_info.subtitle;
    rows.clear();
    for (auto& r : s_info.rows) rows.push_back({r.label, r.value});
}

void Tick(bool nf) {
    if (!s_ov.hwnd) return;
    static HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, L"Local\\LiquidOpenAbout");   // Liquid.exe --about
    if (ev && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0 && !s_open) Open();
    if (s_open && lock::IsLocked()) Close();
    float pa = s_anim;
    Spring(s_anim, s_animV, s_open ? 1.f : 0.f, 1 / 60.f, 520.f);
    if (!s_open && s_anim == 0) {
        if (s_ov.visible) s_ov.SetVisible(false);
        return;
    }
    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y);
    D2D1_RECT_F b = Box();
    bool in = s_open && x >= b.left && x <= b.right && y >= b.top && y <= b.bottom;
    s_ov.SetClickThrough(!in);
    int hv = -1;
    if (in) for (auto& h : s_hits) if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) hv = h.id;
    if (hv != s_hover) { s_hover = hv; s_dirty = true; }
    if (s_dirty || nf || s_anim != pa || g.snapPending) { s_dirty = false; Render(); }
}

void Shutdown() {
    Close();
    if (s_ov.hwnd) s_ov.Destroy();
}
}  // namespace about
