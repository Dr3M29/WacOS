#include "shell.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <shobjidl.h>
#include <propkey.h>
#include <algorithm>

// Right-click on the desktop opens a Liquid glass menu instead of the Windows one.
// The entries are the real shell verbs (Open, Cut, Copy, Delete, Properties, New, Send to,
// app extensions...) read out of the IContextMenu, plus macOS-style View / Sort By items.

namespace {

ComPtr<IFolderView2> DesktopView(HWND* listOut) {
    ComPtr<IShellWindows> wins;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&wins)))) return nullptr;
    VARIANT loc{}, root{};
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    long hwnd = 0;
    ComPtr<IDispatch> disp;
    if (wins->FindWindowSW(&loc, &root, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &disp) != S_OK || !disp) return nullptr;
    ComPtr<IServiceProvider> sp;
    ComPtr<IShellBrowser> browser;
    ComPtr<IShellView> view;
    ComPtr<IFolderView2> fv;
    if (FAILED(disp.As(&sp)) || FAILED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
        FAILED(browser->QueryActiveShellView(&view)) || FAILED(view.As(&fv))) return nullptr;
    if (listOut) {
        HWND dv = nullptr;
        view->GetWindow(&dv);
        *listOut = dv ? FindWindowExW(dv, nullptr, L"SysListView32", nullptr) : nullptr;
    }
    return fv;
}

struct Pidl {
    PITEMID_CHILD p = nullptr;
    ~Pidl() { if (p) CoTaskMemFree(p); }
};

// which desktop icon is under the point (index into the view), -1 = empty desktop
int HitIcon(IFolderView2* fv, HWND list, POINT screen) {
    if (!list) return -1;
    POINT pt = screen;
    ScreenToClient(list, &pt);
    POINT cell{};
    fv->GetSpacing(&cell);
    int n = 0;
    fv->ItemCount(SVGIO_ALLVIEW, &n);
    for (int i = 0; i < n; i++) {
        Pidl id;
        POINT pos{};
        if (FAILED(fv->Item(i, &id.p)) || FAILED(fv->GetItemPosition(id.p, &pos))) continue;
        if (pt.x >= pos.x && pt.x < pos.x + cell.x && pt.y >= pos.y && pt.y < pos.y + cell.y) return i;
    }
    return -1;
}

std::wstring Clean(const std::wstring& raw, std::wstring* shortcut) {
    std::wstring t = raw;
    size_t tab = t.find(L'\t');
    if (tab != std::wstring::npos) {
        if (shortcut) *shortcut = t.substr(tab + 1);
        t = t.substr(0, tab);
    }
    std::wstring o;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == L'&') {
            if (i + 1 < t.size() && t[i + 1] == L'&') { o += L'&'; i++; }
            continue;
        }
        o += t[i];
    }
    return o;
}

// HMENU from IContextMenu -> MenuEntry tree (ids offset by `base`)
void ReadMenu(HMENU hm, IContextMenu* cm, int base, std::vector<MenuEntry>& out, int depth = 0) {
    ComPtr<IContextMenu3> cm3;
    ComPtr<IContextMenu2> cm2;
    cm->QueryInterface(IID_PPV_ARGS(&cm3));
    cm->QueryInterface(IID_PPV_ARGS(&cm2));
    int n = GetMenuItemCount(hm);
    for (int i = 0; i < n; i++) {
        wchar_t buf[512] = {};
        MENUITEMINFOW mi{sizeof(mi)};
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = buf;
        mi.cch = 511;
        if (!GetMenuItemInfoW(hm, i, TRUE, &mi)) continue;
        if (mi.fType & MFT_SEPARATOR) {
            if (!out.empty() && out.back().id != 0) out.push_back({0});
            continue;
        }
        MenuEntry e;
        e.text = Clean(buf, &e.shortcut);
        if (e.text.empty()) continue;   // owner-drawn without text
        e.checked = (mi.fState & MFS_CHECKED) != 0;
        e.enabled = !(mi.fState & (MFS_DISABLED | MFS_GRAYED));
        if (mi.hSubMenu) {
            if (depth > 3) continue;
            LRESULT res = 0;
            if (cm3) cm3->HandleMenuMsg2(WM_INITMENUPOPUP, (WPARAM)mi.hSubMenu, i, &res);   // lazy menus (New, Send to)
            else if (cm2) cm2->HandleMenuMsg(WM_INITMENUPOPUP, (WPARAM)mi.hSubMenu, i);
            ReadMenu(mi.hSubMenu, cm, base, e.sub, depth + 1);
            if (e.sub.empty()) continue;
            e.id = -1;
            e.shortcut.clear();
        } else {
            if (mi.wID < (UINT)base) continue;
            e.id = (int)mi.wID;
        }
        out.push_back(std::move(e));
    }
    while (!out.empty() && out.back().id == 0) out.pop_back();
    if (!out.empty() && out.front().id == 0) out.erase(out.begin());
}

std::wstring Verb(IContextMenu* cm, int cmd) {
    wchar_t v[128] = {};
    if (FAILED(cm->GetCommandString(cmd, GCS_VERBW, nullptr, (LPSTR)v, 127))) return L"";
    return v;
}

bool Invoke(IContextMenu* cm, int cmd, HWND owner, POINT pt) {
    CMINVOKECOMMANDINFOEX ci{sizeof(ci)};
    ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE | CMIC_MASK_ASYNCOK;
    if (GetKeyState(VK_CONTROL) < 0) ci.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (GetKeyState(VK_SHIFT) < 0) ci.fMask |= CMIC_MASK_SHIFT_DOWN;
    ci.hwnd = owner;
    ci.lpVerb = MAKEINTRESOURCEA(cmd);
    ci.lpVerbW = MAKEINTRESOURCEW(cmd);
    ci.nShow = SW_SHOWNORMAL;
    ci.ptInvoke = pt;
    return SUCCEEDED(cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ci));
}

enum : int {
    D_FIRST = 0x7000,
    D_LARGE = D_FIRST, D_MEDIUM, D_SMALL, D_SHOWICONS, D_AUTOARRANGE,
    D_SORT_NAME, D_SORT_SIZE, D_SORT_TYPE, D_SORT_DATE,
    D_REFRESH, D_DISPLAY, D_PERSONALIZE, D_TERMINAL, D_NEWFOLDER, D_WIDGETS,
};

}  // namespace

// show / hide Windows' own desktop icons (Liquid draws its own)
void SetWindowsDesktopIcons(bool show) {
    HWND list = nullptr;
    ComPtr<IFolderView2> fv = DesktopView(&list);
    if (!fv) return;
    DWORD ff = 0;
    fv->GetCurrentFolderFlags(&ff);
    bool hidden = (ff & FWF_NOICONS) != 0;
    if (hidden == !show) return;
    fv->SetCurrentFolderFlags(FWF_NOICONS, show ? 0 : FWF_NOICONS);
}

// the real Windows right-click menu for one file or folder, shown as a Liquid glass menu
bool ItemMenu(const std::wstring& path, POINT pt) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr))) return false;
    ComPtr<IShellFolder> parent;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child);
    ComPtr<IContextMenu> cm;
    HWND owner = GetShellWindow();
    if (SUCCEEDED(hr)) parent->GetUIObjectOf(owner, 1, &child, IID_IContextMenu, nullptr, (void**)cm.GetAddressOf());
    if (!cm) { CoTaskMemFree(pidl); return false; }
    HMENU hm = CreatePopupMenu();
    cm->QueryContextMenu(hm, 0, 1, D_FIRST - 1, CMF_NORMAL | CMF_CANRENAME | (GetKeyState(VK_SHIFT) < 0 ? CMF_EXTENDEDVERBS : 0));
    std::vector<MenuEntry> items;
    ReadMenu(hm, cm.Get(), 1, items);
    int pick = items.empty() ? 0 : GlassMenu(pt.x, pt.y, items);
    if (pick > 0 && pick < D_FIRST) Invoke(cm.Get(), pick - 1, owner, pt);
    DestroyMenu(hm);
    CoTaskMemFree(pidl);
    return true;
}

// returns false when the Windows menu should be shown instead
bool DesktopMenu(POINT pt) {
    HWND list = nullptr;
    ComPtr<IFolderView2> fv = DesktopView(&list);
    if (!fv) { Log(L"desktop menu: no desktop view"); return false; }
    ComPtr<IShellFolder> desk;
    if (FAILED(SHGetDesktopFolder(&desk))) return false;

    int hit = HitIcon(fv.Get(), list, pt);
    std::vector<PITEMID_CHILD> sel;
    if (hit >= 0) {
        Pidl hp;
        fv->Item(hit, &hp.p);
        DWORD st = 0;
        fv->GetSelectionState(hp.p, &st);
        if (!(st & SVSI_SELECT)) fv->SelectItem(hit, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_FOCUSED);
        int n = 0;
        fv->ItemCount(SVGIO_ALLVIEW, &n);
        for (int i = 0; i < n; i++) {
            PITEMID_CHILD p = nullptr;
            if (FAILED(fv->Item(i, &p))) continue;
            DWORD s2 = 0;
            fv->GetSelectionState(p, &s2);
            if (s2 & SVSI_SELECT) sel.push_back(p); else CoTaskMemFree(p);
        }
    } else {
        fv->SelectItem(-1, SVSI_DESELECTOTHERS);
    }

    HWND owner = list ? list : GetShellWindow();
    ComPtr<IContextMenu> cm;
    if (!sel.empty())
        desk->GetUIObjectOf(owner, (UINT)sel.size(), (PCUITEMID_CHILD_ARRAY)sel.data(), IID_IContextMenu, nullptr, (void**)cm.GetAddressOf());
    else
        desk->CreateViewObject(owner, IID_PPV_ARGS(&cm));
    for (auto p : sel) CoTaskMemFree(p);
    if (!cm) { Log(L"desktop menu: no context menu"); return false; }

    HMENU hm = CreatePopupMenu();
    UINT flags = CMF_NORMAL | (GetKeyState(VK_SHIFT) < 0 ? CMF_EXTENDEDVERBS : 0);
    if (!sel.empty()) flags |= CMF_CANRENAME;
    cm->QueryContextMenu(hm, 0, 1, D_FIRST - 1, flags);
    std::vector<MenuEntry> shellItems;
    ReadMenu(hm, cm.Get(), 1, shellItems);

    std::vector<MenuEntry> menu;
    if (sel.empty() && hit < 0) {
        FOLDERVIEWMODE mode{};
        int iconSize = 48;
        fv->GetViewModeAndIconSize(&mode, &iconSize);
        DWORD ff = 0;
        fv->GetCurrentFolderFlags(&ff);
        bool auto_ = (ff & FWF_AUTOARRANGE) != 0, hidden = (ff & FWF_NOICONS) != 0;
        menu.push_back({D_NEWFOLDER, L"New Folder", false, true, L"Ctrl+Shift+N"});
        menu.push_back({0});
        MenuEntry view{-1, L"View"};
        view.sub = {{D_LARGE, L"Large Icons", iconSize >= 80}, {D_MEDIUM, L"Medium Icons", iconSize >= 40 && iconSize < 80},
                    {D_SMALL, L"Small Icons", iconSize < 40}, {0},
                    {D_AUTOARRANGE, L"Auto Arrange", auto_}, {D_SHOWICONS, L"Show Desktop Icons", !hidden}};
        MenuEntry sort{-1, L"Sort By"};
        sort.sub = {{D_SORT_NAME, L"Name"}, {D_SORT_SIZE, L"Size"}, {D_SORT_TYPE, L"Kind"}, {D_SORT_DATE, L"Date Modified"}};
        menu.push_back(view);
        menu.push_back(sort);
        menu.push_back({D_REFRESH, L"Refresh", false, true, L"F5"});
        // shell extras (Paste, New ->, app extensions) minus things we already show
        std::vector<MenuEntry> extra;
        for (auto& e : shellItems) {
            std::wstring v = e.id > 0 ? Verb(cm.Get(), e.id - 1) : L"";
            if (e.text == L"View" || e.text == L"Sort by" || e.text == L"Refresh" || v == L"Display" ||
                v == L"Personalize" || e.text.find(L"Display settings") != std::wstring::npos ||
                e.text == L"Personalize") continue;
            extra.push_back(e);
        }
        while (!extra.empty() && extra.front().id == 0) extra.erase(extra.begin());
        if (!extra.empty()) { menu.push_back({0}); menu.insert(menu.end(), extra.begin(), extra.end()); }
        while (!menu.empty() && menu.back().id == 0) menu.pop_back();
        menu.push_back({0});
        menu.push_back({D_TERMINAL, L"Open in Terminal"});
        menu.push_back({0});
        menu.push_back({D_WIDGETS, widgets::Editing() ? L"Done Editing Widgets" : L"Edit Widgets..."});
        menu.push_back({0});
        menu.push_back({D_DISPLAY, L"Display Settings..."});
        menu.push_back({D_PERSONALIZE, L"Change Wallpaper..."});
    } else {
        menu = shellItems;
    }
    if (menu.empty()) { DestroyMenu(hm); return false; }

    int pick = GlassMenu(pt.x, pt.y, menu);
    if (pick > 0 && pick < D_FIRST) {
        std::wstring v = Verb(cm.Get(), pick - 1);
        if (v == L"rename" && list) {
            SetForegroundWindow(GetAncestor(list, GA_ROOT));
            SetFocus(list);
            PostMessageW(list, WM_KEYDOWN, VK_F2, 0);
            PostMessageW(list, WM_KEYUP, VK_F2, 0);
        } else {
            Invoke(cm.Get(), pick - 1, owner, pt);
        }
    } else if (pick >= D_FIRST) {
        auto sortBy = [&](const PROPERTYKEY& k) {
            SORTCOLUMN sc{k, SORT_ASCENDING};
            fv->SetSortColumns(&sc, 1);
        };
        switch (pick) {
            case D_LARGE: fv->SetViewModeAndIconSize(FVM_ICON, 96); break;
            case D_MEDIUM: fv->SetViewModeAndIconSize(FVM_ICON, 48); break;
            case D_SMALL: fv->SetViewModeAndIconSize(FVM_ICON, 32); break;
            case D_AUTOARRANGE: {
                DWORD ff = 0;
                fv->GetCurrentFolderFlags(&ff);
                fv->SetCurrentFolderFlags(FWF_AUTOARRANGE, (ff & FWF_AUTOARRANGE) ? 0 : FWF_AUTOARRANGE);
                break;
            }
            case D_SHOWICONS: {
                DWORD ff = 0;
                fv->GetCurrentFolderFlags(&ff);
                fv->SetCurrentFolderFlags(FWF_NOICONS, (ff & FWF_NOICONS) ? 0 : FWF_NOICONS);
                break;
            }
            case D_SORT_NAME: sortBy(PKEY_ItemNameDisplay); break;
            case D_SORT_SIZE: sortBy(PKEY_Size); break;
            case D_SORT_TYPE: sortBy(PKEY_ItemTypeText); break;
            case D_SORT_DATE: sortBy(PKEY_DateModified); break;
            case D_REFRESH:
                if (list) { PostMessageW(list, WM_KEYDOWN, VK_F5, 0); PostMessageW(list, WM_KEYUP, VK_F5, 0); }
                SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
                break;
            case D_NEWFOLDER: {
                wchar_t path[MAX_PATH];
                if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, path))) {
                    std::wstring base = std::wstring(path) + L"\\New Folder", p = base;
                    for (int k = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; k++) p = base + L" " + std::to_wstring(k);
                    CreateDirectoryW(p.c_str(), nullptr);
                    SHChangeNotify(SHCNE_MKDIR, SHCNF_PATHW, p.c_str(), nullptr);
                }
                break;
            }
            case D_TERMINAL: {
                wchar_t path[MAX_PATH];
                SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, path);
                Launch(L"wt.exe", std::wstring(L"-d \"") + path + L"\"");
                break;
            }
            case D_WIDGETS: widgets::SetEditing(!widgets::Editing()); break;
            case D_DISPLAY: Launch(L"ms-settings:display"); break;
            case D_PERSONALIZE: Launch(L"ms-settings:personalization-background"); break;
        }
    }
    DestroyMenu(hm);
    return true;
}
