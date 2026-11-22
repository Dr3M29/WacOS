#include "shell.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <cmath>
#include <algorithm>

// Desktop icons, macOS style: dock-style liquid glass tiles and names underneath. You pick what lives here:
//   * first run: File Explorer on the left side (the files already on the desktop stay hidden)
//   * files you drop on the desktop later get an icon automatically
//   * drag icons anywhere; right-click: the file's real menu, plus "Remove from Desktop" (the file
//     itself stays where it is); Spotlight's right-click "Add to Desktop" puts an app here
// Windows' own desktop icons are hidden while Liquid runs.

namespace {
struct Item {
    std::wstring name, target;
    int x = 0, y = 0;            // screen position of the cell's top-left corner
    IconImages icons;
    bool loaded = false;
};
Overlay s_ov;
float s_s = 1;
std::vector<Item> s_items;
int s_hover = -1, s_sel = -1, s_drag = -1;
bool s_dirty = true, s_moved = false, s_pendingDefaults = false;
POINT s_dragStart{}, s_dragOff{};
ULONGLONG s_lastScan = 0, s_lastBottom = 0, s_lastClick = 0;
int s_lastClickIdx = -1;
ComPtr<ID2D1SolidColorBrush> s_br;
ComPtr<IDWriteTextFormat> fLabel;
const float kTile = 54, kCellW = 96, kCellH = 100;
const wchar_t* kWanted[] = {L"File Explorer"};   // first run; add more from Spotlight (right-click > Add to Desktop)

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    if (p) CoTaskMemFree(p);
    return r;
}

std::vector<std::wstring> DesktopFiles() {
    std::vector<std::wstring> out;
    for (auto& dir : {Known(FOLDERID_Desktop), Known(FOLDERID_PublicDesktop)}) {
        if (dir.empty()) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring n = fd.cFileName;
            if (n == L"." || n == L".." || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) || Lower(n) == L"desktop.ini") continue;
            out.push_back(dir + L"\\" + n);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

std::wstring DisplayName(const std::wstring& path) {
    std::wstring n = path.substr(path.find_last_of(L'\\') + 1);
    std::wstring l = Lower(n);
    for (const wchar_t* ext : {L".lnk", L".url"})
        if (l.size() > 4 && l.compare(l.size() - 4, 4, ext) == 0) return n.substr(0, n.size() - 4);
    return n;
}

RECT Area() { RECT r{s_ov.x, s_ov.y, s_ov.x + s_ov.w, s_ov.y + s_ov.h}; return r; }

// the next free grid cell, filling columns from the top-left (or the top-right)
POINT FreeCell(bool fromRight = false) {
    RECT a = Area();
    int cw = (int)(kCellW * s_s), ch = (int)(kCellH * s_s);
    int cols = std::max(1, (int)((a.right - a.left) / cw)), rows = std::max(1, (int)((a.bottom - a.top) / ch));
    for (int c = 0; c < cols; c++)
        for (int r = 0; r < rows; r++) {
            int x = fromRight ? a.right - (c + 1) * cw : a.left + (int)(12 * s_s) + c * cw, y = a.top + (int)(14 * s_s) + r * ch;
            bool used = false;
            for (auto& it : s_items) if (std::abs(it.x - x) < cw / 2 && std::abs(it.y - y) < ch / 2) used = true;
            if (!used) return {x, y};
        }
    return {a.left, a.top};
}

void Save() {
    layout::icons.clear();
    for (auto& it : s_items) layout::icons.push_back({it.name, it.target, it.x, it.y});
    layout::Save();
}

void AddItem(const std::wstring& name, const std::wstring& target) {
    for (auto& it : s_items) if (Lower(it.target) == Lower(target)) return;
    POINT p = FreeCell();
    Item it;
    it.name = name; it.target = target; it.x = p.x; it.y = p.y;
    s_items.push_back(it);
    s_dirty = true;
}

void BuildDefaults() {
    auto files = DesktopFiles();
    for (auto& w : kWanted) {
        std::wstring lw = Lower(w), found;
        for (auto& f : files) if (Lower(DisplayName(f)).find(lw) != std::wstring::npos) { found = f; break; }
        if (!found.empty()) { AddItem(DisplayName(found), found); continue; }
        std::wstring disp, app = spotlight::FindApp(w, &disp);
        if (!app.empty()) AddItem(disp.empty() ? w : disp, app);
    }
    for (auto& f : files) layout::seen.insert(Lower(f));   // everything already there stays hidden
    Save();
}

void Scan() {
    // files that newly appear on the desktop get an icon; ones that vanish lose theirs
    bool changed = false;
    for (auto& f : DesktopFiles()) {
        std::wstring lf = Lower(f);
        if (layout::seen.count(lf)) continue;
        layout::seen.insert(lf);
        AddItem(DisplayName(f), f);
        changed = true;
    }
    for (size_t i = 0; i < s_items.size();) {
        auto& t = s_items[i].target;
        bool file = t.size() > 2 && t[1] == L':';
        if (file && GetFileAttributesW(t.c_str()) == INVALID_FILE_ATTRIBUTES) { s_items.erase(s_items.begin() + i); changed = true; s_sel = -1; }
        else i++;
    }
    if (changed) { Save(); s_dirty = true; }
}

void SinkToDesktop(HWND h) {
    HWND progman = FindWindowW(L"Progman", nullptr);
    if (progman) SetWindowLongPtrW(h, GWLP_HWNDPARENT, (LONG_PTR)progman);
    SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

D2D1_RECT_F CellRect(const Item& it) {
    float x = (float)(it.x - s_ov.x), y = (float)(it.y - s_ov.y);
    return D2D1::RectF(x, y, x + kCellW * s_s, y + kCellH * s_s);
}
D2D1_RECT_F TileRect(const Item& it) {
    D2D1_RECT_F c = CellRect(it);
    float cx = (c.left + c.right) / 2, t = kTile * s_s;
    return D2D1::RectF(cx - t / 2, c.top + 6 * s_s, cx + t / 2, c.top + 6 * s_s + t);
}

void ShowMenu(int i) {
    POINT pt;
    GetCursorPos(&pt);
    Item it = s_items[i];
    bool file = it.target.size() > 2 && it.target[1] == L':';
    std::vector<MenuEntry> m = {{1, L"Open"}, {2, L"Run as Administrator"}};
    if (file) m.push_back({3, L"More Options..."});
    m.push_back({0});
    m.push_back({4, L"Remove from Desktop"});
    int c = GlassMenu(pt.x, pt.y, m);
    if (c == 1) Launch(it.target);
    else if (c == 2) {
        SHELLEXECUTEINFOW ei{sizeof(ei)};
        ei.lpVerb = L"runas"; ei.lpFile = it.target.c_str(); ei.nShow = SW_SHOWNORMAL;
        std::wstring home = HomeDir();
        ei.lpDirectory = home.c_str();
        if (!ShellExecuteExW(&ei)) Launch(it.target);
    } else if (c == 3) ItemMenu(it.target, pt);   // the full Windows menu (rename, delete, properties...)
    else if (c == 4) {
        for (size_t k = 0; k < s_items.size(); k++) if (s_items[k].target == it.target) { s_items.erase(s_items.begin() + k); break; }
        s_sel = -1;
        Save();
    }
    s_dirty = true;
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_WINDOWPOSCHANGING: {
            auto* p = (WINDOWPOS*)lp;
            if (!(p->flags & SWP_NOZORDER)) p->hwndInsertAfter = HWND_BOTTOM;
            break;
        }
        case WM_LBUTTONDOWN: {
            if (s_hover < 0) return 0;
            ULONGLONG now = GetTickCount64();
            if (s_hover == s_lastClickIdx && now - s_lastClick < GetDoubleClickTime()) {
                Launch(s_items[s_hover].target);   // double-click opens
                s_lastClickIdx = -1;
                return 0;
            }
            s_lastClickIdx = s_hover;
            s_lastClick = now;
            s_sel = s_drag = s_hover;
            s_moved = false;
            GetCursorPos(&s_dragStart);
            s_dragOff = {s_dragStart.x - s_items[s_drag].x, s_dragStart.y - s_items[s_drag].y};
            SetCapture(h);
            s_dirty = true;
            return 0;
        }
        case WM_MOUSEMOVE:
            if (s_drag >= 0) {
                POINT pt;
                GetCursorPos(&pt);
                if (!s_moved && std::abs(pt.x - s_dragStart.x) + std::abs(pt.y - s_dragStart.y) < (int)(5 * s_s)) return 0;
                s_moved = true;
                s_items[s_drag].x = pt.x - s_dragOff.x;
                s_items[s_drag].y = pt.y - s_dragOff.y;
                s_dirty = true;
            }
            return 0;
        case WM_LBUTTONUP:
            if (s_drag >= 0) {
                ReleaseCapture();
                if (s_moved) {   // snap to the grid
                    RECT a = Area();
                    int cw = (int)(kCellW * s_s), ch = (int)(kCellH * s_s);
                    auto& it = s_items[s_drag];
                    int gx = a.left + (int)(12 * s_s), gy = a.top + (int)(14 * s_s);
                    it.x = gx + (int)std::lround((it.x - gx) / (float)cw) * cw;
                    it.y = gy + (int)std::lround((it.y - gy) / (float)ch) * ch;
                    it.x = std::clamp(it.x, (int)a.left, (int)(a.right - cw));
                    it.y = std::clamp(it.y, (int)a.top, (int)(a.bottom - ch));
                    Save();
                }
                s_drag = -1;
                s_dirty = true;
            }
            return 0;
        case WM_RBUTTONUP:
            if (s_hover >= 0) { s_sel = s_hover; ShowMenu(s_hover); }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Render() {
    float s = s_s;
    std::vector<GlassShape> sh;
    for (auto& it : s_items) {
        if (sh.size() >= 46) break;
        if (it.icons.custom) continue;
        D2D1_RECT_F r = TileRect(it);
        float sz = r.right - r.left;
        GlassShape t{r.left, r.top, sz, sz};
        t.radius = sz * 0.24f; t.blur = 0.0f; t.bezel = sz * 0.34f; t.thick = 2.4f;
        t.tint = -0.55f; t.shadow = 0.0f; t.highlight = 0.0f; t.opacity = 1;
        sh.push_back(t);
    }
    float tint[3] = {1, 1, 1};
    s_ov.Render(s, sh, tint, [&](ID2D1DeviceContext* dc) {
        for (int i = 0; i < (int)s_items.size(); i++) {
            auto& it = s_items[i];
            if (!it.loaded) { it.icons = LoadIcons(dc, it.target, (int)(kTile * s * 2), IconOverride(it.name, it.target)); it.loaded = true; }
            D2D1_RECT_F r = TileRect(it), cell = CellRect(it);
            float sz = r.right - r.left;
            if (i == s_sel || i == s_hover) {
                s_br->SetColor(D2D1::ColorF(1, 1, 1, i == s_sel ? 0.14f : 0.07f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 6 * s, r.top - 4 * s, r.right + 6 * s, r.bottom + 4 * s), 14 * s, 14 * s), s_br.Get());
            }
            if (it.icons.custom && it.icons.color) dc->DrawBitmap(it.icons.color.Get(), &r, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            else if (it.icons.silver) {
                float off = sz * 0.022f;
                D2D1_RECT_F sr = D2D1::RectF(r.left, r.top + off, r.right, r.bottom + off);
                if (it.icons.shadow) dc->DrawBitmap(it.icons.shadow.Get(), &sr, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                dc->DrawBitmap(it.icons.silver.Get(), &r, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            } else if (it.icons.color) {
                D2D1_RECT_F in = D2D1::RectF(r.left + sz * 0.18f, r.top + sz * 0.18f, r.right - sz * 0.18f, r.bottom - sz * 0.18f);
                dc->DrawBitmap(it.icons.color.Get(), &in, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            }
            D2D1_RECT_F lr = D2D1::RectF(cell.left + 4 * s, r.bottom + 6 * s, cell.right - 4 * s, cell.bottom);
            if (i == s_sel) {
                float tw = std::min(TextWidth(fLabel.Get(), it.name) + 12 * s, lr.right - lr.left), cx = (lr.left + lr.right) / 2;
                s_br->SetColor(D2D1::ColorF(0.0f, 0.42f, 0.95f, 0.95f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - tw / 2, lr.top - 1 * s, cx + tw / 2, lr.top + 17 * s), 5 * s, 5 * s), s_br.Get());
            } else {
                s_br->SetColor(D2D1::ColorF(0, 0, 0, 0.6f));
                D2D1_RECT_F shr = D2D1::RectF(lr.left, lr.top + 1 * s, lr.right, lr.bottom + 1 * s);
                dc->DrawTextW(it.name.c_str(), (UINT32)it.name.size(), fLabel.Get(), shr, s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
            s_br->SetColor(D2D1::ColorF(1, 1, 1, 0.96f));
            dc->DrawTextW(it.name.c_str(), (UINT32)it.name.size(), fLabel.Get(), lr, s_br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    });
}
}  // namespace

namespace deskicons {
void Add(const std::wstring& name, const std::wstring& target) {
    if (!s_ov.hwnd || target.empty()) return;
    AddItem(name, target);
    Save();
}

bool Init() {
    if (!cfg.desktopIcons) return true;
    HMONITOR hm = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s_s = dx / 96.f;
    RECT wa = mi.rcWork;
    int bottomSpace = (int)(90 * s_s);   // keep clear of the dock
    if (!s_ov.Create(L"LiquidDesktopIcons", Proc, wa.left, wa.top, wa.right - wa.left, (wa.bottom - wa.top) - bottomSpace, nullptr)) return false;
    SinkToDesktop(s_ov.hwnd);
    s_ov.SetClickThrough(true);
    s_ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &s_br);
    fLabel = MakeFont(L"Segoe UI Variable Text", 12 * s_s, DWRITE_FONT_WEIGHT_SEMI_BOLD, true);
    fLabel->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    fLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    SetWindowsDesktopIcons(false);
    for (auto& ip : layout::icons) { Item it; it.name = ip.name; it.target = ip.target; it.x = ip.x; it.y = ip.y; s_items.push_back(it); }
    s_pendingDefaults = layout::icons.empty() && layout::seen.empty();   // first run: wait for the app list
    return true;
}

void Tick(bool nf) {
    if (!s_ov.hwnd) return;
    ULONGLONG now = GetTickCount64();
    if (s_pendingDefaults && spotlight::AppsReady()) { s_pendingDefaults = false; BuildDefaults(); }
    if (!s_pendingDefaults && now - s_lastScan > 2000) { s_lastScan = now; Scan(); }
    if (now - s_lastBottom > 2000) {
        s_lastBottom = now;
        // Liquid can start before Windows' desktop exists (early sign-in start): keep hooking it up
        if (!GetWindowLongPtrW(s_ov.hwnd, GWLP_HWNDPARENT)) SinkToDesktop(s_ov.hwnd);
        SetWindowsDesktopIcons(false);
        SetWindowPos(s_ov.hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - s_ov.x), y = (float)(p.y - s_ov.y);
    int hv = -1;
    for (int i = 0; i < (int)s_items.size(); i++) {
        D2D1_RECT_F c = CellRect(s_items[i]);
        if (x >= c.left && x <= c.right && y >= c.top && y <= c.bottom) hv = i;
    }
    if (hv < 0 && s_sel >= 0 && s_drag < 0 && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) && WindowFromPoint(p) != s_ov.hwnd) { s_sel = -1; s_dirty = true; }
    if (hv != s_hover && s_drag < 0) { s_hover = hv; s_dirty = true; }
    s_ov.SetClickThrough(hv < 0 && s_drag < 0);
    static ULONGLONG lastGlass = 0;
    bool glass = nf && s_ov.cap && s_ov.cap->fresh && now - lastGlass > 150;
    if (s_dirty || glass || g.snapPending) { s_dirty = false; lastGlass = now; Render(); }
}

void Shutdown() {
    if (s_ov.hwnd) { if (!g_reloading && !g_restart) SetWindowsDesktopIcons(true); s_ov.Destroy(); }
}
}  // namespace deskicons
