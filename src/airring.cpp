#include "shell.h"

// Air Ring is part of WacOS: the Python app in air-ring\ next to Liquid.exe that owns the webcam
// for Face ID, the walk-away lock and air gestures. Liquid starts it (no tray icon of its own,
// "--embedded"), and starts it again if it stops. It quits by itself 30 seconds after Liquid does.
// tools\setup.ps1 makes its Python environment (air-ring\.venv); until then this does nothing.
// config.ini: air_ring = off keeps the camera off entirely.

namespace airring {
namespace {
ULONGLONG s_lastStart = 0, s_lastCheck = 0;
int s_starts = 0;
}

std::wstring Dir() { return ExeDir() + L"\\air-ring\\"; }

bool Installed() {
    return GetFileAttributesW((Dir() + L"air_ring.py").c_str()) != INVALID_FILE_ATTRIBUTES &&
           GetFileAttributesW((Dir() + L".venv\\Scripts\\pythonw.exe").c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool Running() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"AirRingSingleInstance");
    if (m) CloseHandle(m);
    return m != nullptr;
}

bool Start() {
    if (!Installed() || Running()) return false;
    std::wstring py = Dir() + L".venv\\Scripts\\pythonw.exe";
    std::wstring cmd = L"\"" + py + L"\" \"" + Dir() + L"air_ring.py\" --embedded";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring dir = Dir();
    if (!CreateProcessW(py.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        Log(L"air ring: couldn't start it (error %lu)", GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    s_lastStart = GetTickCount64();
    s_starts++;
    Log(L"air ring: started (%s)", s_starts == 1 ? L"with Liquid" : L"it had stopped");
    return true;
}

// Every few seconds: keep it running. A crash loop backs off to one try a minute.
void Tick() {
    if (!cfg.airRing) return;
    ULONGLONG now = GetTickCount64();
    if (now - s_lastCheck < 5000) return;
    s_lastCheck = now;
    if (Running() || !Installed()) return;
    ULONGLONG wait = s_starts < 3 ? 15000 : 60000;
    if (s_lastStart && now - s_lastStart < wait) return;
    Start();
}
}  // namespace airring
