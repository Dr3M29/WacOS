#include "shell.h"
#include <cmath>
#include <algorithm>

ControlCenter cc;

enum {
    H_WIFI_TOGGLE = 1, H_WIFI_LIST, H_BT_TOGGLE, H_BT_SETTINGS, H_NET,
    H_VOL = 10, H_SOUND_LIST, H_MUTE,
    H_PREV = 20, H_PLAY, H_NEXT,
    H_BACK = 30, H_WIFI_SETTINGS, H_SOUND_SETTINGS,
    H_NET0 = 100, H_DEV0 = 200
};

static const float kW = 320, kMargin = 30, kPad = 12;
static HHOOK s_ccMouse = nullptr, s_ccKbd = nullptr;
static float s_panelH = 0, s_panelHV = 0;

static LRESULT CALLBACK CCMouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && cc.open && !GlassMenuOpen() &&
        (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
        auto* m = (MSLLHOOKSTRUCT*)lp;
        RECT box{cc.ov.x + (int)(kMargin * cc.s), cc.ov.y + (int)(kMargin * cc.s),
                 cc.ov.x + (int)((kMargin + kW) * cc.s), cc.ov.y + (int)(kMargin * cc.s + s_panelH)};
        bool onBar = false;
        for (auto& mb : menubars) {
            RECT r{mb->ov.x, mb->ov.y, mb->ov.x + mb->ov.w, mb->ov.y + mb->H};
            if (PtInRect(&r, m->pt)) onBar = true;   // the menu bar toggles it itself
        }
        if (!PtInRect(&box, m->pt) && !onBar) cc.Close();
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static LRESULT CALLBACK CCKey(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && cc.open && wp == WM_KEYDOWN && ((KBDLLHOOKSTRUCT*)lp)->vkCode == VK_ESCAPE) {
        cc.Close();
        return 1;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

bool ControlCenter::Init(float scale) {
    s = scale;
    int w = (int)std::ceil((kW + kMargin * 2) * s), h = (int)std::ceil((560 + kMargin * 2) * s);
    if (!ov.Create(L"LiquidControlCenter", Proc, -w - 100, 0, w, h, this)) return false;
    ov.SetVisible(false);
    const wchar_t* ui = L"Segoe UI Variable Text";
    fTitle = MakeFont(ui, 13.5f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    fBody = MakeFont(ui, 13 * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fSmall = MakeFont(ui, 11.5f * s, DWRITE_FONT_WEIGHT_NORMAL, false);
    fIcon = MakeFont(L"Segoe Fluent Icons", 14 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    fIconBig = MakeFont(L"Segoe Fluent Icons", 18 * s, DWRITE_FONT_WEIGHT_NORMAL, true);
    ov.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
    return true;
}

void ControlCenter::Toggle(RECT m, int startSection) {
    if (open && section == startSection) { Close(); return; }
    mon = m;
    section = startSection;
    if (!open) {
        int x = mon.right - (int)((kW + kMargin + 6) * s);
        int y = mon.top + (int)(cfg.menubarHeight * s) + 4 - (int)(kMargin * s);
        ov.Move(x, y);
        ov.SetVisible(true);
        open = true;
        openedAt = GetTickCount64();
        s_ccMouse = SetWindowsHookExW(WH_MOUSE_LL, CCMouse, GetModuleHandleW(nullptr), 0);
        s_ccKbd = SetWindowsHookExW(WH_KEYBOARD_LL, CCKey, GetModuleHandleW(nullptr), 0);
    }
    if (section == 1) sys::WifiScan();
    lastPoll = 0;
    dirty = true;
}

void ControlCenter::Close() {
    if (!open) return;
    open = false;
    dragVolume = false;
    if (s_ccMouse) UnhookWindowsHookEx(s_ccMouse);
    if (s_ccKbd) UnhookWindowsHookEx(s_ccKbd);
    s_ccMouse = s_ccKbd = nullptr;
    dirty = true;
}

static float ContentHeight(int section, size_t nNets, size_t nDevs, float s) {
    if (section == 1) return (44 + 34 * (float)std::min<size_t>(nNets ? nNets : 1, 9) + 8 + 36 + kPad) * s;
    if (section == 2) return (44 + 44 + 34 * (float)std::max<size_t>(nDevs, 1) + 8 + 36 + kPad) * s;
    return (kPad + 132 + kPad + 70 + kPad + 78 + kPad) * s;
}

void ControlCenter::Tick(bool newFrame) {
    float prevA = anim;
    Spring(anim, animV, open ? 1.f : 0.f, 1 / 60.f, 520.f);
    if (!open && anim == 0) {
        if (ov.visible) ov.SetVisible(false);
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (open && now - lastPoll > (section == 1 ? 1500u : 400u)) {
        lastPoll = now;
        if (!dragVolume) volCache = sys::Volume();
        if (section == 2 || devs.empty()) devs = sys::OutputDevices();
        if (section == 1) nets = sys::WifiNetworks();
        dirty = true;
    }
    float target = ContentHeight(section, nets.size(), devs.size(), s);
    if (s_panelH == 0) s_panelH = target;
    float prevH = s_panelH;
    Spring(s_panelH, s_panelHV, target, 1 / 60.f, 500.f);

    POINT p;
    GetCursorPos(&p);
    float x = (float)(p.x - ov.x), y = (float)(p.y - ov.y);
    float m = kMargin * s;
    bool in = x >= m && x <= m + kW * s && y >= m && y <= m + s_panelH;
    ov.SetClickThrough(!in || !open);
    int hv = -1;
    if (in) for (auto& h : hits) if (x >= h.r.left && x < h.r.right && y >= h.r.top && y < h.r.bottom) hv = h.id;
    if (hv != hover) { hover = hv; dirty = true; }
    if (dragVolume) {
        for (auto& h : hits) if (h.id == H_VOL) {
            float v = std::clamp((x - h.r.left) / (h.r.right - h.r.left), 0.f, 1.f);
            if (std::fabs(v - volCache) > 0.002f) { volCache = v; sys::SetVolume(v); dirty = true; }
        }
    }
    if (dirty || newFrame || anim != prevA || s_panelH != prevH || g.snapPending) {
        Render();
        dirty = false;
    }
}

void ControlCenter::Render() {
    hits.clear();
    float a = anim, m = kMargin * s, W = kW * s, pad = kPad * s;
    float yoff = (1 - a) * -10 * s;
    D2D1_RECT_F box = D2D1::RectF(m, m + yoff, m + W, m + yoff + s_panelH);
    RECT behind{ov.x + (int)box.left, ov.y + (int)box.top, ov.x + (int)box.right, ov.y + (int)box.bottom};
    bool light = ov.cap ? ov.cap->Luma(behind) > 0.55f : false;

    MediaInfo media = sys::Media();
    if (media.artVersion != artVersion) {
        artVersion = media.artVersion;
        art = DecodeImage(ov.dc.Get(), media.art, (int)(96 * s));
    }
    int wifiR = sys::WifiRadio(), btR = sys::BluetoothRadio();
    std::wstring ssid = sys::WifiConnectedSsid();
    bool eth = sys::EthernetUp();
    bool muted = sys::Muted();

    std::vector<GlassShape> sh;
    GlassShape gs{box.left, box.top, W, s_panelH};
    gs.radius = 22 * s; gs.blur = 2.4f; gs.tint = light ? 0.38f : -0.34f; gs.bezel = 16 * s; gs.thick = 2.2f;   // Tahoe: frosted refracting glass, no white rim
    gs.shadow = 0.22f; gs.highlight = 0.0f; gs.opacity = a;
    sh.push_back(gs);
    float tintLight[3] = {0.96f, 0.96f, 0.97f}, tintDark[3] = {0.12f, 0.12f, 0.14f};

    D2D1_COLOR_F fg = light ? D2D1::ColorF(0, 0, 0, 0.88f * a) : D2D1::ColorF(1, 1, 1, 0.95f * a);
    D2D1_COLOR_F fg2 = light ? D2D1::ColorF(0, 0, 0, 0.5f * a) : D2D1::ColorF(1, 1, 1, 0.6f * a);
    D2D1_COLOR_F modBg = light ? D2D1::ColorF(1, 1, 1, 0.10f * a) : D2D1::ColorF(1, 1, 1, 0.06f * a);
    D2D1_COLOR_F hotBg = light ? D2D1::ColorF(0, 0, 0, 0.06f * a) : D2D1::ColorF(1, 1, 1, 0.08f * a);
    D2D1_COLOR_F accent = D2D1::ColorF(0.04f, 0.52f, 1.0f, a);

    ov.Render(s, sh, light ? tintLight : tintDark, [&](ID2D1DeviceContext* dc) {
        dc->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        auto text = [&](IDWriteTextFormat* f, const std::wstring& t, D2D1_RECT_F r, D2D1_COLOR_F c) {
            br->SetColor(c);
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        auto module = [&](D2D1_RECT_F r) {
            br->SetColor(modBg);
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 16 * s, 16 * s), br.Get());
        };
        auto hotRow = [&](D2D1_RECT_F r, int id) {
            hits.push_back({r, id});
            if (hover == id) {
                br->SetColor(hotBg);
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8 * s, 8 * s), br.Get());
            }
        };
        auto circle = [&](float cx, float cy, float r, bool on, const wchar_t* glyph, int id) {
            hits.push_back({D2D1::RectF(cx - r, cy - r, cx + r, cy + r), id});
            br->SetColor(on ? accent : (light ? D2D1::ColorF(0, 0, 0, 0.1f * a) : D2D1::ColorF(1, 1, 1, 0.14f * a)));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), br.Get());
            text(fIcon.Get(), glyph, D2D1::RectF(cx - r, cy - r, cx + r, cy + r), on ? D2D1::ColorF(1, 1, 1, a) : fg);
        };
        auto slider = [&](D2D1_RECT_F r, float v, int id) {
            hits.push_back({D2D1::RectF(r.left, r.top - 6 * s, r.right, r.bottom + 6 * s), id});
            float h = r.bottom - r.top;
            br->SetColor(light ? D2D1::ColorF(0, 0, 0, 0.1f * a) : D2D1::ColorF(1, 1, 1, 0.16f * a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, h / 2, h / 2), br.Get());
            D2D1_RECT_F f = r;
            f.right = r.left + std::max(h, (r.right - r.left) * v);
            br->SetColor(light ? D2D1::ColorF(1, 1, 1, a) : D2D1::ColorF(1, 1, 1, 0.95f * a));
            dc->FillRoundedRectangle(D2D1::RoundedRect(f, h / 2, h / 2), br.Get());
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.15f * a));
            dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(f.right - h / 2, (r.top + r.bottom) / 2), h / 2 - 0.5f, h / 2 - 0.5f), br.Get(), 1);
        };
        auto header = [&](const std::wstring& title) {
            float y = box.top + pad;
            hotRow(D2D1::RectF(box.left + pad, y, box.left + pad + 28 * s, y + 28 * s), H_BACK);
            text(fIconBig.Get(), L"\uE76B", D2D1::RectF(box.left + pad, y, box.left + pad + 28 * s, y + 28 * s), fg);
            text(fTitle.Get(), title, D2D1::RectF(box.left + pad + 34 * s, y, box.right - pad, y + 28 * s), fg);
        };
        auto footer = [&](const std::wstring& label, int id) {
            float y = box.top + s_panelH - pad - 36 * s;
            br->SetColor(light ? D2D1::ColorF(0, 0, 0, 0.1f * a) : D2D1::ColorF(1, 1, 1, 0.12f * a));
            dc->FillRectangle(D2D1::RectF(box.left + pad, y - 4 * s, box.right - pad, y - 3 * s), br.Get());
            D2D1_RECT_F r = D2D1::RectF(box.left + pad, y, box.right - pad, y + 32 * s);
            hotRow(r, id);
            text(fBody.Get(), label, D2D1::RectF(r.left + 10 * s, r.top, r.right, r.bottom), fg);
        };

        if (section == 0) {
            float y = box.top + pad;
            // connectivity module
            D2D1_RECT_F mod = D2D1::RectF(box.left + pad, y, box.right - pad, y + 132 * s);
            module(mod);
            struct Row { const wchar_t* glyph; std::wstring title, sub; bool on; int toggle, open; };
            std::wstring wifiSub = wifiR == 0 ? L"Off" : !ssid.empty() ? ssid : sys::WifiAvailable() ? L"Not connected" : L"No adapter";
            Row rows[3] = {
                {L"\uE701", L"Wi-Fi", wifiSub, wifiR == 1, H_WIFI_TOGGLE, H_WIFI_LIST},
                {L"\uE702", L"Bluetooth", btR == 1 ? L"On" : btR == 0 ? L"Off" : L"Unavailable", btR == 1, H_BT_TOGGLE, H_BT_SETTINGS},
                {L"\uE839", L"Ethernet", eth ? L"Connected" : L"Not connected", eth, H_NET, H_NET},
            };
            for (int i = 0; i < 3; i++) {
                float ry = mod.top + 8 * s + i * 40 * s;
                D2D1_RECT_F rr = D2D1::RectF(mod.left + 44 * s, ry, mod.right - 6 * s, ry + 38 * s);
                hotRow(rr, rows[i].open);
                circle(mod.left + 24 * s, ry + 19 * s, 14 * s, rows[i].on, rows[i].glyph, rows[i].toggle);
                text(fTitle.Get(), rows[i].title, D2D1::RectF(rr.left + 6 * s, ry + 2 * s, rr.right, ry + 20 * s), fg);
                text(fSmall.Get(), rows[i].sub, D2D1::RectF(rr.left + 6 * s, ry + 19 * s, rr.right - 4 * s, ry + 36 * s), fg2);
            }
            y = mod.bottom + pad;
            // sound module
            mod = D2D1::RectF(box.left + pad, y, box.right - pad, y + 70 * s);
            module(mod);
            text(fTitle.Get(), L"Sound", D2D1::RectF(mod.left + 14 * s, mod.top + 6 * s, mod.right, mod.top + 28 * s), fg);
            D2D1_RECT_F chev = D2D1::RectF(mod.right - 34 * s, mod.top + 4 * s, mod.right - 6 * s, mod.top + 30 * s);
            hotRow(chev, H_SOUND_LIST);
            text(fIcon.Get(), L"\uE76C", chev, fg);
            D2D1_RECT_F spk = D2D1::RectF(mod.left + 8 * s, mod.top + 34 * s, mod.left + 36 * s, mod.top + 62 * s);
            hotRow(spk, H_MUTE);
            text(fIcon.Get(), muted ? L"\uE74F" : L"\uE767", spk, fg);
            slider(D2D1::RectF(mod.left + 42 * s, mod.top + 40 * s, mod.right - 14 * s, mod.top + 56 * s), muted ? 0 : volCache, H_VOL);
            y = mod.bottom + pad;
            // now playing module
            mod = D2D1::RectF(box.left + pad, y, box.right - pad, y + 78 * s);
            module(mod);
            D2D1_RECT_F ar = D2D1::RectF(mod.left + 12 * s, mod.top + 12 * s, mod.left + 66 * s, mod.top + 66 * s);
            if (art) {
                ComPtr<ID2D1RoundedRectangleGeometry> geo;
                g.d2dFactory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(ar, 8 * s, 8 * s), &geo);
                dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                     D2D1::IdentityMatrix(), a), nullptr);
                dc->DrawBitmap(art.Get(), &ar, 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
                dc->PopLayer();
            } else {
                br->SetColor(light ? D2D1::ColorF(0, 0, 0, 0.08f * a) : D2D1::ColorF(1, 1, 1, 0.1f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(ar, 8 * s, 8 * s), br.Get());
                text(fIconBig.Get(), L"\uE8D6", ar, fg2);
            }
            float tx = ar.right + 12 * s, bw = 30 * s;
            float bx = mod.right - 10 * s - bw * 3;
            if (media.present) {
                text(fTitle.Get(), media.title.empty() ? media.app : media.title,
                     D2D1::RectF(tx, mod.top + 18 * s, bx - 4 * s, mod.top + 38 * s), fg);
                text(fSmall.Get(), media.artist.empty() ? media.app : media.artist,
                     D2D1::RectF(tx, mod.top + 38 * s, bx - 4 * s, mod.top + 58 * s), fg2);
            } else {
                text(fTitle.Get(), L"Not Playing", D2D1::RectF(tx, mod.top + 18 * s, bx - 4 * s, mod.bottom - 18 * s), fg2);
            }
            const wchar_t* glyphs[3] = {L"\uE892", media.playing ? L"\uE769" : L"\uE768", L"\uE893"};
            int ids[3] = {H_PREV, H_PLAY, H_NEXT};
            for (int i = 0; i < 3; i++) {
                D2D1_RECT_F r = D2D1::RectF(bx + i * bw, mod.top + 24 * s, bx + (i + 1) * bw, mod.top + 54 * s);
                hotRow(r, ids[i]);
                text(i == 1 ? fIconBig.Get() : fIcon.Get(), glyphs[i], r, media.present ? fg : fg2);
            }
        } else if (section == 1) {
            header(L"Wi-Fi");
            D2D1_RECT_F tg = D2D1::RectF(box.right - pad - 44 * s, box.top + pad + 4 * s, box.right - pad, box.top + pad + 24 * s);
            hits.push_back({tg, H_WIFI_TOGGLE});
            br->SetColor(wifiR == 1 ? accent : (light ? D2D1::ColorF(0, 0, 0, 0.15f * a) : D2D1::ColorF(1, 1, 1, 0.2f * a)));
            dc->FillRoundedRectangle(D2D1::RoundedRect(tg, 10 * s, 10 * s), br.Get());
            br->SetColor(D2D1::ColorF(1, 1, 1, a));
            float kx = wifiR == 1 ? tg.right - 10 * s : tg.left + 10 * s;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, (tg.top + tg.bottom) / 2), 8 * s, 8 * s), br.Get());
            float y = box.top + 44 * s;
            if (nets.empty()) text(fBody.Get(), wifiR == 0 ? L"Wi-Fi is off" : L"No networks found",
                                   D2D1::RectF(box.left + pad + 10 * s, y, box.right - pad, y + 34 * s), fg2);
            for (size_t i = 0; i < nets.size() && i < 9; i++) {
                auto& n = nets[i];
                D2D1_RECT_F r = D2D1::RectF(box.left + pad, y, box.right - pad, y + 32 * s);
                hotRow(r, H_NET0 + (int)i);
                circle(r.left + 18 * s, (r.top + r.bottom) / 2, 12 * s, n.connected, L"\uE701", H_NET0 + (int)i);
                text(fBody.Get(), n.ssid, D2D1::RectF(r.left + 38 * s, r.top, r.right - 50 * s, r.bottom), fg);
                if (n.secure) text(fIcon.Get(), L"\uE72E", D2D1::RectF(r.right - 48 * s, r.top, r.right - 26 * s, r.bottom), fg2);
                text(fSmall.Get(), std::to_wstring(n.signal) + L"%", D2D1::RectF(r.right - 30 * s, r.top, r.right, r.bottom), fg2);
                y += 34 * s;
            }
            footer(L"Wi-Fi Settings...", H_WIFI_SETTINGS);
        } else {
            header(L"Sound");
            float y = box.top + 44 * s;
            D2D1_RECT_F spk = D2D1::RectF(box.left + pad, y + 6 * s, box.left + pad + 28 * s, y + 34 * s);
            hotRow(spk, H_MUTE);
            text(fIcon.Get(), muted ? L"\uE74F" : L"\uE767", spk, fg);
            slider(D2D1::RectF(spk.right + 8 * s, y + 12 * s, box.right - pad - 8 * s, y + 28 * s), muted ? 0 : volCache, H_VOL);
            y += 44 * s;
            text(fSmall.Get(), L"Output", D2D1::RectF(box.left + pad + 8 * s, y - 8 * s, box.right, y + 6 * s), fg2);
            y += 4 * s;
            for (size_t i = 0; i < devs.size(); i++) {
                D2D1_RECT_F r = D2D1::RectF(box.left + pad, y, box.right - pad, y + 32 * s);
                hotRow(r, H_DEV0 + (int)i);
                circle(r.left + 18 * s, (r.top + r.bottom) / 2, 12 * s, devs[i].isDefault, L"\uE7F5", H_DEV0 + (int)i);
                text(fBody.Get(), devs[i].name, D2D1::RectF(r.left + 38 * s, r.top, r.right - 8 * s, r.bottom), fg);
                y += 34 * s;
            }
            footer(L"Sound Settings...", H_SOUND_SETTINGS);
        }
        dc->PopAxisAlignedClip();
    });
}

void ControlCenter::OnClick(int id, float) {
    if (id == H_WIFI_TOGGLE) sys::SetWifiRadio(sys::WifiRadio() != 1);
    else if (id == H_WIFI_LIST) { section = 1; sys::WifiScan(); lastPoll = 0; }
    else if (id == H_BT_TOGGLE) sys::SetBluetoothRadio(sys::BluetoothRadio() != 1);
    else if (id == H_BT_SETTINGS) { Launch(L"ms-settings:bluetooth"); Close(); }
    else if (id == H_NET) { Launch(L"ms-settings:network"); Close(); }
    else if (id == H_SOUND_LIST) { section = 2; devs = sys::OutputDevices(); }
    else if (id == H_MUTE) sys::SetMuted(!sys::Muted());
    else if (id == H_PREV) sys::MediaCommand(0);
    else if (id == H_PLAY) sys::MediaCommand(1);
    else if (id == H_NEXT) sys::MediaCommand(2);
    else if (id == H_BACK) section = 0;
    else if (id == H_WIFI_SETTINGS) { Launch(L"ms-settings:network-wifi"); Close(); }
    else if (id == H_SOUND_SETTINGS) { Launch(L"ms-settings:sound"); Close(); }
    else if (id >= H_DEV0 && id < H_DEV0 + (int)devs.size()) {
        sys::SetDefaultOutput(devs[id - H_DEV0].id);
        devs = sys::OutputDevices();
    } else if (id >= H_NET0 && id < H_NET0 + (int)nets.size()) {
        auto& n = nets[id - H_NET0];
        if (n.connected) {}
        else if (!n.profile.empty()) sys::WifiConnect(n.profile);
        else { Launch(L"ms-settings:network-wifi"); Close(); }
    }
    lastPoll = 0;
    dirty = true;
}

LRESULT CALLBACK ControlCenter::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    ControlCenter& c = cc;
    switch (msg) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_LBUTTONDOWN:
            c.pressed = c.hover;
            if (c.hover == H_VOL) { c.dragVolume = true; SetCapture(h); }
            return 0;
        case WM_LBUTTONUP: {
            if (c.dragVolume) { c.dragVolume = false; ReleaseCapture(); c.pressed = -1; return 0; }
            int p = c.pressed;
            c.pressed = -1;
            if (p >= 0 && p == c.hover) c.OnClick(p, 0);
            return 0;
        }
        case WM_MOUSEWHEEL:
            if (c.hover == H_VOL || c.section != 1) {
                float d = GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 0.04f;
                c.volCache = std::clamp(c.volCache + d, 0.f, 1.f);
                sys::SetVolume(c.volCache);
                c.dirty = true;
            }
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
