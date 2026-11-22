#include "shell.h"
#include <shellapi.h>
#include <d2d1effects.h>
#include <dwmapi.h>
#include <cmath>
#include <algorithm>

Dock dock;
static const float PI = 3.14159265f;
static const double kBouncePeriod = 0.6;
static const float kDt = 1 / 60.f;
static const wchar_t* kRecycleBin = L"known:recyclebin";

static double NowSec() { return GetTickCount64() / 1000.0; }

bool Dock::Init(RECT m, float scale) {
    mon = m;
    s = scale;
    S = cfg.iconSize * s;
    padIn = 7 * s;
    gap = 6 * s;
    bgH = S + 2 * padIn;
    margin = 6 * s;
    M = cfg.magnify;
    W = mon.right - mon.left;
    H = (int)std::ceil(margin + bgH + S * (std::max(M, 1.6f) - 1) + 64 * s);
    if (!ov.Create(L"LiquidDock", Proc, mon.left, mon.bottom - H, W, H, this)) return false;

    APPBARDATA a{sizeof(a)};
    a.hWnd = ov.hwnd;
    a.uCallbackMessage = WM_APP + 2;
    SHAppBarMessage(ABM_NEW, &a);
    Reserve();

    fLabel = MakeFont(L"Segoe UI Variable Text", 13 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
    slide = cfg.autohide ? 1.f : 0.f;
    LoadPins();
    Scan();
    for (auto& it : items) it.size = S;
    return true;
}

// keep maximised windows above the dock (nothing reserved when it auto-hides)
void Dock::Reserve() {
    APPBARDATA a{sizeof(a)};
    a.hWnd = ov.hwnd;
    a.uEdge = ABE_BOTTOM;
    int reserve = (cfg.autohide || cfg.dockOverlap) ? 0 : (int)std::ceil(margin + bgH);
    a.rc = {mon.left, mon.bottom - reserve, mon.right, mon.bottom};
    SHAppBarMessage(ABM_QUERYPOS, &a);
    a.rc.top = a.rc.bottom - reserve;
    SHAppBarMessage(ABM_SETPOS, &a);
}

void Dock::Shutdown() {
    APPBARDATA a{sizeof(a)};
    a.hWnd = ov.hwnd;
    SHAppBarMessage(ABM_REMOVE, &a);
    ov.Destroy();
}

static std::wstring MatchDirFor(const std::wstring& lowerTarget) {
    size_t slash = lowerTarget.find_last_of(L'\\');
    if (slash == std::wstring::npos) return L"";
    std::wstring dir = lowerTarget.substr(0, slash + 1);
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    if (dir.rfind(Lower(win), 0) == 0) return L"";
    if (std::count(dir.begin(), dir.end(), L'\\') < 3) return L"";
    return dir;
}

void Dock::LoadPins() {
    items.clear();
    int px = 256;
    if (cfg.finder) {
        DockItem f;
        wchar_t win[MAX_PATH];
        GetWindowsDirectoryW(win, MAX_PATH);
        std::wstring ex = std::wstring(win) + L"\\explorer.exe";
        f.launch = ex; f.iconPath = ex; f.match = Lower(ex); f.name = L"Finder"; f.pinned = true;
        std::wstring fo = IconOverride(L"finder", ex);
        f.icons = LoadIcons(ov.dc.Get(), f.iconPath, px, fo);
        if (fo.empty()) { auto gi = GlyphIcon(ov.dc.Get(), L"\uE8D5"); f.icons.clear = gi.clear; f.icons.brand = gi.brand; f.icons.silver = gi.silver; f.icons.shadow = gi.shadow; }
        items.push_back(f);
    }
    for (auto& p : cfg.pins) {
        DockItem it;
        it.launch = p; it.iconPath = p; it.pinned = true;
        it.name = FileStem(p);
        std::wstring target = Lower(p);
        if (target.size() > 4 && target.compare(target.size() - 4, 4, L".lnk") == 0) target = Lower(ResolveLnk(p));
        it.match = target;
        it.matchDir = MatchDirFor(target);
        // Store apps (pin = explorer.exe shell:AppsFolder\Name_publisher!App, or that string itself):
        // their exe lives in C:\Program Files\WindowsApps\Name_<version>..., which changes every update
        std::wstring aumid = Lower(p).rfind(L"shell:appsfolder\\", 0) == 0 ? p.substr(17) : L"";
        if (aumid.empty() && target.size() >= 12 && target.compare(target.size() - 12, 12, L"explorer.exe") == 0) {
            std::wstring args = Lower(LnkArguments(p));
            size_t at = args.find(L"shell:appsfolder\\");
            if (at != std::wstring::npos) aumid = args.substr(at + 17);
        }
        if (!aumid.empty()) {
            size_t us = aumid.find(L'_');
            if (us != std::wstring::npos) {
                wchar_t pf[MAX_PATH];
                ExpandEnvironmentStringsW(L"%ProgramFiles%\\WindowsApps\\", pf, MAX_PATH);
                it.match.clear();
                it.matchDir = Lower(pf) + Lower(aumid.substr(0, us + 1));
            }
        }
        it.icons = LoadIcons(ov.dc.Get(), it.iconPath, px, IconOverride(it.name, target));
        items.push_back(it);
    }
    if (cfg.trash) {
        DockItem t;
        t.launch = L"shell:RecycleBinFolder"; t.iconPath = kRecycleBin; t.name = L"Trash";
        t.pinned = true; t.trash = true;
        std::wstring to = IconOverride(L"trash", L"");
        t.icons = LoadIcons(ov.dc.Get(), t.iconPath, px, to);
        if (to.empty()) { auto gi = GlyphIcon(ov.dc.Get(), L"\uE74D"); t.icons.clear = gi.clear; t.icons.brand = gi.brand; t.icons.silver = gi.silver; t.icons.shadow = gi.shadow; }
        items.push_back(t);
    }
    Log(L"dock: %d pinned items", (int)items.size());
}

static bool Matches(const DockItem& it, const std::wstring& exe) {
    if (it.match.empty()) return false;
    if (exe == it.match) return true;
    return !it.matchDir.empty() && exe.rfind(it.matchDir, 0) == 0;
}

struct EnumCtx { std::vector<std::pair<std::wstring, HWND>> wins; };
static BOOL CALLBACK EnumWins(HWND h, LPARAM lp) {
    if (!IsAppWindow(h)) return TRUE;
    std::wstring path = ProcessPath(WindowPid(h));
    if (path.empty()) return TRUE;
    ((EnumCtx*)lp)->wins.push_back({path, h});
    return TRUE;
}

void Dock::Scan() {
    EnumCtx ec;
    EnumWindows(EnumWins, (LPARAM)&ec);
    for (auto& it : items) it.wins.clear();
    for (auto& [path, h] : ec.wins) {
        std::wstring exe = Lower(path);
        bool matched = false;
        for (auto& it : items)
            if (Matches(it, exe)) { it.wins.push_back(h); matched = true; break; }
        if (matched) continue;
        DockItem n;
        n.launch = path; n.iconPath = path; n.match = exe;
        n.name = FileDescription(path);
        n.icons = LoadIcons(ov.dc.Get(), path, 256, IconOverride(n.name, path));
        n.wins.push_back(h);
        n.size = 0;   // grows in
        auto pos = std::find_if(items.begin(), items.end(), [](const DockItem& d) { return d.trash; });
        items.insert(pos, n);
        dirty = true;
    }
    size_t before = items.size();
    items.erase(std::remove_if(items.begin(), items.end(), [](const DockItem& d) {
        return !d.pinned && d.wins.empty() && d.bounceStart < 0;
    }), items.end());
    if (items.size() != before) dirty = true;
}

// sizes spring toward the magnification curve; positions follow the sizes
void Dock::Layout(float dt) {
    int n = (int)items.size();
    if (!n) return;
    float sepW = 14 * s;
    int nSep = 0;
    bool seenUnpinned = false;
    for (auto& it : items) {
        it.sepBefore = false;
        if (!it.pinned && !seenUnpinned) { it.sepBefore = true; seenUnpinned = true; }
        if (it.trash) it.sepBefore = true;
    }
    items[0].sepBefore = false;
    for (auto& it : items) if (it.sepBefore) nSep++;

    float baseTotal = n * S + (n - 1) * gap + nSep * sepW;
    float bx = (W - baseTotal) / 2;
    float R = S * 2.8f, total = 0;
    for (auto& it : items) {
        if (it.sepBefore) bx += sepW;
        float c = bx + S / 2;
        bx += S + gap;
        float d = std::fabs(mx - c);
        float f = d < R ? 0.5f * (std::cos(PI * d / R) + 1) : 0;
        float target = S * (1 + (M - 1) * f * hoverAmt);
        if (dt > 0) Spring(it.size, it.sizeV, target, dt, 700.f);
        total += it.size;
    }
    total += (n - 1) * gap + nSep * sepW;
    float x = (W - total) / 2;
    for (auto& it : items) {
        if (it.sepBefore) { it.sepX = x - gap / 2 + sepW / 2; x += sepW; }
        it.cx = x + it.size / 2;
        x += it.size + gap;
    }
    float left = (W - total) / 2 - padIn;
    float drop = slide * (bgH + margin + 16 * s);
    bg = D2D1::RectF(left - jelly * 0.35f, H - margin - bgH + drop - jelly, left + total + 2 * padIn + jelly * 0.35f, H - margin + drop);
    iconBottom = bg.bottom - padIn;
}

int Dock::HitTest(float x, float y) {
    for (int i = 0; i < (int)items.size(); i++) {
        auto& it = items[i];
        float half = it.size / 2 + gap / 2;
        if (std::fabs(x - it.cx) <= half && y >= iconBottom - it.size - 4 * s && y <= bg.bottom) return i;
    }
    return -1;
}

float Dock::Bounce(const DockItem& it, double now) {
    if (it.bounceStart < 0) return 0;
    double t = now - it.bounceStart;
    double phase = std::fmod(t, kBouncePeriod) / kBouncePeriod;
    float h = (float)std::sin(phase * PI);
    return h * h * 20 * s;   // eased hop
}

void Dock::Tick(bool newFrame) {
    ULONGLONG nowMs = GetTickCount64();
    double now = nowMs / 1000.0;
    if (nowMs - lastScan > 500) { Scan(); lastScan = nowMs; }

    if (nowMs - lastLuma > 500 && ov.cap) {
        lastLuma = nowMs;
        RECT r{ov.x + (int)bg.left, ov.y + (int)bg.top, ov.x + (int)bg.right, ov.y + (int)bg.bottom};
        float l = ov.cap->Luma(r);
        bool lt = light ? l > 0.5f : l > 0.6f;
        if (lt != light) { light = lt; dirty = true; }
    }

    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - ov.x), y = (float)(p.y - ov.y);
    float extra = hoverAmt > 0.05f ? S * (M - 1) * hoverAmt + 8 * s : 0;
    bool in = slide < 0.5f && x >= bg.left - 2 && x <= bg.right + 2 && y >= bg.top - extra && y <= H && y >= 0;
    if (pressIdx >= 0 || GlassMenuOpen() && inside) in = true;

    // auto-hide: reveal at the screen's bottom edge, tuck away after leaving
    float slideTarget = 0;
    if (cfg.autohide || fsHide) {   // (fsHide: a full-screen app is up; the dock still pops up from the edge)
        bool edge = p.y >= mon.bottom - 2 && p.x >= mon.left && p.x < mon.right;
        if (in || edge) leftAt = nowMs;
        slideTarget = (nowMs - leftAt < 700) ? 0.f : 1.f;
    }
    float prevSlide = slide;
    Spring(slide, slideV, slideTarget, kDt, 300.f);

    if (in && !inside) jellyV -= 70 * s;   // pointer arrives: the glass gives a little
    {   // underdamped spring = visible wobble
        float k = 260.f, c = 11.f;
        jellyV += (-k * jelly - c * jellyV) * kDt;
        jelly += jellyV * kDt;
        if (std::fabs(jelly) < 0.02f && std::fabs(jellyV) < 0.5f) { jelly = 0; jellyV = 0; }
    }
    inside = in;
    float prevAmt = hoverAmt;
    Spring(hoverAmt, hoverV, in ? 1.f : 0.f, kDt, 260.f);
    if (in) mx = x;
    ov.SetClickThrough(!in);
    Layout(kDt);
    int hi = in ? HitTest(x, y) : -1;
    if (hi != hoverIdx) { hoverIdx = hi; dirty = true; }
    if (hi >= 0) labelIdx = hi;
    float prevLabel = labelA;
    Spring(labelA, labelV, (hi >= 0 && hoverAmt > 0.5f) ? 1.f : 0.f, kDt, 600.f);

    // cursor near the glass moves the light, so redraw while it moves close by
    float nearDist = std::max({bg.left - x, x - bg.right, 0.f}) + std::max({bg.top - y, 0.f});
    bool moved = x != lastX || y != lastY;
    bool anim = hoverAmt != prevAmt || slide != prevSlide || labelA != prevLabel || jelly != 0 ||
                (moved && (in || nearDist < 200 * s));
    for (int i = 0; i < (int)items.size(); i++) {
        auto& it = items[i];
        float ps = it.press, ds = it.dot;
        Spring(it.press, it.pressV, i == pressIdx ? 1.f : 0.f, kDt, 900.f);
        Spring(it.dot, it.dotV, it.wins.empty() ? 0.f : 1.f, kDt, 300.f);
        if (it.sizeV != 0 || it.press != ps || it.dot != ds) anim = true;
        if (it.bounceStart >= 0) {
            double t = now - it.bounceStart;
            int cyc = (int)(t / kBouncePeriod);
            if (cyc != it.bounceCycle) {
                it.bounceCycle = cyc;
                if (!it.wins.empty() || t > 4.2) { it.bounceStart = -1; dirty = true; continue; }
            }
            anim = true;
        }
    }
    lastX = x; lastY = y;
    if (hidden) return;
    bool capFresh = newFrame && ov.cap && ov.cap->fresh;
    if (capFresh || dirty || anim || g.snapPending) {
        Render();
        dirty = false;
    }
}

void Dock::Render() {
    double now = NowSec();
    float vis = 1 - slide;
    std::vector<GlassShape> sh;
    GlassShape base{bg.left, bg.top, bg.right - bg.left, bg.bottom - bg.top};
    base.radius = bgH * 0.42f; base.blur = 0.0f; base.tint = -0.28f;   // light smoky black
    base.bezel = bgH * 0.5f; base.thick = 2.4f; base.shadow = 0.0f; base.highlight = 0.0f;   // real refraction, no white outline
    base.group = 1; base.melt = 20 * s;
    base.opacity = std::clamp(vis * 1.4f, 0.f, 1.f);
    sh.push_back(base);

    bool clear = cfg.iconStyle == L"clear", dark = cfg.iconStyle == L"dark", tahoe = cfg.iconStyle == L"tahoe" || cfg.iconStyle == L"tinted";
    if (!tileBrush) {
        D2D1_GRADIENT_STOP st[2] = {{0, D2D1::ColorF(0.33f, 0.33f, 0.35f, 0.62f)}, {1, D2D1::ColorF(0.10f, 0.10f, 0.11f, 0.74f)}};
        ComPtr<ID2D1GradientStopCollection> col;
        ov.dc->CreateGradientStopCollection(st, 2, &col);
        ov.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)), col.Get(), &tileBrush);
        D2D1_GRADIENT_STOP sh2[3] = {{0, D2D1::ColorF(1, 1, 1, 0.42f)}, {0.45f, D2D1::ColorF(1, 1, 1, 0.05f)}, {1, D2D1::ColorF(1, 1, 1, 0.16f)}};
        ComPtr<ID2D1GradientStopCollection> col2;
        ov.dc->CreateGradientStopCollection(sh2, 3, &col2);
        // superellipse |x|^5 + |y|^5 = 1, mapped to the unit square
        g.d2dFactory->CreatePathGeometry(&squircle);
        ComPtr<ID2D1GeometrySink> sink;
        squircle->Open(&sink);
        const int N = 96;
        for (int k = 0; k <= N; k++) {
            float th = 2 * PI * k / N, c = std::cos(th), sn = std::sin(th);
            float px = 0.5f + 0.5f * (c < 0 ? -1.f : 1.f) * std::pow(std::fabs(c), 0.4f);
            float py = 0.5f + 0.5f * (sn < 0 ? -1.f : 1.f) * std::pow(std::fabs(sn), 0.4f);
            if (k == 0) sink->BeginFigure(D2D1::Point2F(px, py), D2D1_FIGURE_BEGIN_FILLED);
            else sink->AddLine(D2D1::Point2F(px, py));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        ov.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)), col2.Get(), &sheenBrush);
        DWORD argb = 0;
        BOOL opaque = FALSE;
        if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque))) {
            accent = D2D1::ColorF(((argb >> 16) & 255) / 255.f, ((argb >> 8) & 255) / 255.f, (argb & 255) / 255.f);
            float mx = std::max({accent.r, accent.g, accent.b});
            if (mx > 0.01f) { accent.r /= mx; accent.g /= mx; accent.b /= mx; }   // full brightness
        }
    }
    std::vector<D2D1_RECT_F> iconRects(items.size());
    for (size_t i = 0; i < items.size(); i++) {
        auto& it = items[i];
        float sz = it.size * (1 - 0.08f * it.press);
        float bo = Bounce(it, now);
        float cyb = iconBottom - (it.size - sz) / 2;
        iconRects[i] = D2D1::RectF(it.cx - sz / 2, cyb - sz - bo, it.cx + sz / 2, cyb - bo);
        if ((dark || tahoe) && !it.icons.custom && sh.size() < 30 && sz > 1) {
            // smoky tile behind each icon: flat glass (no liquid rim), same tint as the dock
            auto& r = iconRects[i];
            GlassShape t{r.left, r.top, sz, sz};
            t.radius = sz * 0.24f; t.blur = 0.0f; t.bezel = sz * 0.34f; t.thick = 2.4f;
            t.tint = -0.55f; t.shadow = 0.0f; t.highlight = 0.0f; t.opacity = base.opacity;
            sh.push_back(t);
        }
        if (clear && sh.size() < 30 && sz > 1) {
            // macOS "clear" icons: frosted, bright glass tiles
            auto& r = iconRects[i];
            GlassShape t{r.left, r.top, sz, sz};
            t.radius = sz * 0.225f; t.blur = 2.4f; t.bezel = sz * 0.2f; t.thick = 1.2f;
            t.tint = light ? 0.42f : 0.26f;
            t.shadow = 0.16f; t.highlight = 0.45f; t.opacity = base.opacity;
            sh.push_back(t);
        }
    }

    D2D1_RECT_F label{};
    std::wstring labelText;
    if (labelIdx >= 0 && labelIdx < (int)items.size() && labelA > 0.01f) {
        auto& it = items[labelIdx];
        labelText = it.name;
        float tw = TextWidth(fLabel.Get(), labelText);
        float lw = tw + 24 * s, lh = 26 * s;
        // the label beads up out of the dock glass and pinches off (same liquid group as the dock)
        float finalTop = iconRects[labelIdx].top - 10 * s - lh;
        float top = bg.top + 2 * s + (finalTop - bg.top - 2 * s) * std::clamp(labelA, 0.f, 1.2f);
        float lx = std::clamp(it.cx - lw / 2, 4.f, W - lw - 4.f);
        label = D2D1::RectF(lx, std::max(2.f, top), lx + lw, std::max(2.f, top) + lh);
        GlassShape lg{label.left, label.top, lw, lh};
        lg.radius = lh / 2; lg.blur = 0.0f; lg.tint = 0.0f; lg.bezel = lh / 2; lg.thick = 2.2f;
        lg.shadow = 0.0f; lg.highlight = 0.0f; lg.opacity = std::clamp(labelA, 0.f, 1.f);
        lg.group = 1; lg.melt = 20 * s;
        sh.push_back(lg);
    }

    float tint[3] = {1, 1, 1};
    float op = base.opacity;
    ov.Render(s, sh, tint, [&](ID2D1DeviceContext* dc) {
        for (int i = 0; i < (int)items.size(); i++) {
            auto& it = items[i];
            D2D1_RECT_F r = iconRects[i];
            float sz = r.right - r.left;
            if (sz <= 1) continue;
            float dim = 1 - 0.35f * it.press;
            if (it.icons.custom && it.icons.color) {
                dc->DrawBitmap(it.icons.color.Get(), &r, dim * op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            } else if ((dark || tahoe) && it.icons.clear) {
                // macOS dark / tinted icon: solid dark squircle, soft shadow, faint top sheen, glowing glyph
                // the tile itself is clear liquid glass (drawn by the shader); only the glyph is painted here
                float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
                D2D1_RECT_F gr = D2D1::RectF(cx - sz / 2, cy - sz / 2, cx + sz / 2, cy + sz / 2);
                if (dark && it.icons.brand) {
                    dc->DrawBitmap(it.icons.brand.Get(), &gr, dim * op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                } else if (it.icons.silver) {
                    float off = sz * 0.022f;
                    D2D1_RECT_F srect = D2D1::RectF(gr.left, gr.top + off, gr.right, gr.bottom + off);
                    if (it.icons.shadow) dc->DrawBitmap(it.icons.shadow.Get(), &srect, dim * op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                    dc->DrawBitmap(it.icons.silver.Get(), &gr, dim * op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                }
            } else if (clear && it.icons.clear) {
                float k = 1.0f;
                float g = sz * k, cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
                D2D1_RECT_F gr = D2D1::RectF(cx - g / 2, cy - g / 2, cx + g / 2, cy + g / 2);
                float glyphA = (light && clear ? 0.75f : 0.95f) * dim * op;
                if (light && clear) {
                    // dark glyph on bright backdrops so it stays readable
                    ComPtr<ID2D1Effect> fx;
                    dc->CreateEffect(CLSID_D2D1ColorMatrix, &fx);
                    fx->SetInput(0, it.icons.clear.Get());
                    D2D1_MATRIX_5X4_F mtx = D2D1::Matrix5x4F(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, glyphA * 0.8f, 0.1f, 0.1f, 0.12f, 0);
                    fx->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, mtx);
                    D2D1_SIZE_F bs = it.icons.clear->GetSize();
                    dc->SetTransform(D2D1::Matrix3x2F::Scale(g / bs.width, g / bs.height) * D2D1::Matrix3x2F::Translation(gr.left, gr.top));
                    dc->DrawImage(fx.Get(), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                    dc->SetTransform(D2D1::Matrix3x2F::Identity());
                } else {
                    dc->DrawBitmap(it.icons.clear.Get(), &gr, glyphA, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                }
            } else if (it.icons.color) {
                dc->DrawBitmap(it.icons.color.Get(), &r, dim * op, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            } else {
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.25f * op));
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, sz * 0.22f, sz * 0.22f), br.Get());
            }
            if (it.dot > 0.01f) {
                br->SetColor(light ? D2D1::ColorF(0, 0, 0, 0.7f * it.dot * op) : D2D1::ColorF(1, 1, 1, 0.85f * it.dot * op));
                float rr = 2 * s * it.dot;
                dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(it.cx, bg.bottom - 3.2f * s), rr, rr), br.Get());
            }
            if (it.sepBefore) {
                br->SetColor(light ? D2D1::ColorF(0, 0, 0, 0.22f * op) : D2D1::ColorF(1, 1, 1, 0.28f * op));
                dc->FillRectangle(D2D1::RectF(it.sepX - 0.5f * s, bg.top + bgH * 0.2f,
                                              it.sepX + 0.5f * s, bg.bottom - bgH * 0.2f), br.Get());
            }
        }
        if (!labelText.empty()) {
            br->SetColor(light ? D2D1::ColorF(0, 0, 0, 0.88f * labelA) : D2D1::ColorF(1, 1, 1, 0.97f * labelA));
            dc->DrawTextW(labelText.c_str(), (UINT32)labelText.size(), fLabel.Get(), label, br.Get());
        }
    });
}

void Dock::Click(int idx) {
    if (idx < 0 || idx >= (int)items.size()) return;
    auto& it = items[idx];
    if (!it.wins.empty()) {
        HWND fg = GetForegroundWindow();
        HWND target = it.wins[0];
        if (fg == it.wins[0] && it.wins.size() > 1) target = it.wins.back();
        if (spaces::IsHidden(target)) spaces::Reveal(target);   // it lives in another space
        else ActivateWindow(target);
    } else {
        Launch(it.launch);
        if (!it.trash) { it.bounceStart = NowSec(); it.bounceCycle = 0; }
    }
    dirty = true;
}

void Dock::RightClick(int idx) {
    if (idx < 0 || idx >= (int)items.size()) return;
    auto& it = items[idx];
    std::vector<MenuEntry> menu;
    for (size_t k = 0; k < it.wins.size() && k < 15; k++) {
        wchar_t t[256];
        GetWindowTextW(it.wins[k], t, 256);
        menu.push_back({100 + (int)k, t, it.wins[k] == GetForegroundWindow()});
    }
    if (!menu.empty()) menu.push_back({0});
    menu.push_back({1, it.wins.empty() ? L"Open" : L"New Window"});
    if (it.trash) menu.push_back({3, L"Empty Trash..."});
    if (!it.wins.empty()) { menu.push_back({0}); menu.push_back({2, L"Quit"}); }
    POINT p;
    GetCursorPos(&p);
    int c = GlassMenu(p.x - (int)(20 * s), ov.y + (int)bg.top - 8, menu);
    if (c >= 100 && c - 100 < (int)it.wins.size()) {
        HWND w = it.wins[c - 100];
        if (spaces::IsHidden(w)) spaces::Reveal(w); else ActivateWindow(w);
    }
    else if (c == 1) { Launch(it.launch); if (it.wins.empty() && !it.trash) { it.bounceStart = NowSec(); it.bounceCycle = 0; } }
    else if (c == 2) for (HWND w : it.wins) PostMessageW(w, WM_CLOSE, 0, 0);
    else if (c == 3) SHEmptyRecycleBinW(nullptr, nullptr, 0);   // Windows shows its own confirmation
}

void Dock::BackgroundMenu() {
    bool login = IsAutostart();
    std::vector<MenuEntry> menu = {
        {1, cfg.autohide ? L"Turn Hiding Off" : L"Turn Hiding On"},
        {2, L"Magnification", cfg.magnify > 1.01f},
        {0},
        {13, L"Tahoe Icons (silver)", cfg.iconStyle == L"tahoe" || cfg.iconStyle == L"tinted"},
        {11, L"Dark Icons (app colors)", cfg.iconStyle == L"dark"},
        {10, L"Clear Glass Icons", cfg.iconStyle == L"clear"},
        {12, L"Original Icons", cfg.iconStyle == L"color"},
        {0},
        {20, L"Show in Recordings (restarts Liquid)", cfg.recordable},
        {21, L"Open at Login", login},
        {0},
        {30, L"Edit Settings..."},
        {31, L"Quit Liquid"},
    };
    POINT p;
    GetCursorPos(&p);
    int c = GlassMenu(p.x - (int)(20 * s), ov.y + (int)bg.top - 8, menu);
    switch (c) {
        case 1:
            cfg.autohide = !cfg.autohide;
            SetConfigValue(L"autohide", cfg.autohide ? L"on" : L"off");
            leftAt = GetTickCount64();
            Reserve();
            break;
        case 2:
            cfg.magnify = cfg.magnify > 1.01f ? 1.f : 1.6f;
            M = cfg.magnify;
            SetConfigValue(L"magnify", cfg.magnify > 1.01f ? L"1.6" : L"1");
            break;
        case 10: case 11: case 12: case 13:
            cfg.iconStyle = c == 10 ? L"clear" : c == 11 ? L"dark" : c == 12 ? L"color" : L"tahoe";
            SetConfigValue(L"icon_style", cfg.iconStyle);
            break;
        case 20:
            SetConfigValue(L"recordable", cfg.recordable ? L"off" : L"on");
            g_restart = true;
            g_quit = true;
            break;
        case 21: SetAutostart(!login); break;
        case 30: Launch(L"notepad.exe", L"\"" + ExeDir() + L"\\config.ini\""); break;
        case 31: g_quit = true; break;
    }
    dirty = true;
}

LRESULT CALLBACK Dock::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Dock& d = dock;
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN: d.pressIdx = d.hoverIdx; d.dirty = true; return 0;
        case WM_LBUTTONUP: {
            int p = d.pressIdx;
            d.pressIdx = -1;
            d.dirty = true;
            if (p >= 0 && p == d.hoverIdx) d.Click(p);
            return 0;
        }
        case WM_RBUTTONUP:
            if (d.hoverIdx >= 0) d.RightClick(d.hoverIdx);
            else if (d.inside) d.BackgroundMenu();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
