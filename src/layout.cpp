#include "shell.h"

// Where every desktop widget and icon lives (layout.txt next to Liquid.exe):
//   w|type|x|y            a widget, screen position of its top-left corner
//   i|name|target|x|y     a desktop icon; target is a file path or shell:AppsFolder\...
//   seen|path             desktop files Liquid already knows about (new files get an icon)
//   launch|name|target    an app in the App Launcher widget

namespace layout {
std::vector<WidgetPos> widgets;
std::vector<IconPos> icons;
std::vector<IconPos> launch;
std::set<std::wstring> seen;
bool exists = false;

static std::wstring Path() { return ExeDir() + L"\\layout.txt"; }

static std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> out;
    size_t a = 0;
    for (;;) {
        size_t b = s.find(L'|', a);
        out.push_back(s.substr(a, b == std::wstring::npos ? std::wstring::npos : b - a));
        if (b == std::wstring::npos) break;
        a = b + 1;
    }
    return out;
}

void Load() {
    widgets.clear(); icons.clear(); launch.clear(); seen.clear();
    HANDLE f = CreateFileW(Path().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { exists = false; return; }
    exists = true;
    std::string u(1 << 20, 0);
    DWORD rd = 0;
    ReadFile(f, u.data(), (DWORD)u.size(), &rd, nullptr);
    CloseHandle(f);
    u.resize(rd);
    int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), nullptr, 0);
    std::wstring t(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.data(), (int)u.size(), t.data(), n);
    size_t pos = 0;
    while (pos < t.size()) {
        size_t nl = t.find(L'\n', pos);
        std::wstring line = t.substr(pos, nl == std::wstring::npos ? std::wstring::npos : nl - pos);
        pos = nl == std::wstring::npos ? t.size() : nl + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        auto p = Split(line);
        if (p[0] == L"w" && p.size() >= 4) widgets.push_back({p[1], _wtoi(p[2].c_str()), _wtoi(p[3].c_str())});
        else if (p[0] == L"i" && p.size() >= 5) icons.push_back({p[1], p[2], _wtoi(p[3].c_str()), _wtoi(p[4].c_str())});
        else if (p[0] == L"launch" && p.size() >= 3) launch.push_back({p[1], p[2], 0, 0});
        else if (p[0] == L"seen" && p.size() >= 2) seen.insert(Lower(p[1]));
    }
}

void Save() {
    std::wstring t = L"# Liquid desktop layout (widgets, icons). Edit Widgets / drag icons to change it.\n";
    for (auto& w : widgets) t += L"w|" + w.type + L"|" + std::to_wstring(w.x) + L"|" + std::to_wstring(w.y) + L"\n";
    for (auto& i : icons) t += L"i|" + i.name + L"|" + i.target + L"|" + std::to_wstring(i.x) + L"|" + std::to_wstring(i.y) + L"\n";
    for (auto& l : launch) t += L"launch|" + l.name + L"|" + l.target + L"\n";
    for (auto& s : seen) t += L"seen|" + s + L"\n";
    int n = WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, t.c_str(), (int)t.size(), u.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW(Path().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(f);
    exists = true;
}
}  // namespace layout
