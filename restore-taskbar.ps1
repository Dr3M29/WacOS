# Emergency restore: brings back the Windows taskbar if Liquid crashed while it was hidden.
# Run:  powershell -ExecutionPolicy Bypass -File restore-taskbar.ps1   (from the WacOS folder)
Get-Process Liquid -ErrorAction SilentlyContinue | Stop-Process -Force
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class TB {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l,t,r,b; }
  [StructLayout(LayoutKind.Sequential)] public struct APPBARDATA { public int cbSize; public IntPtr hWnd; public uint msg; public uint edge; public RECT rc; public IntPtr lParam; }
  [DllImport("shell32.dll")] public static extern IntPtr SHAppBarMessage(uint m, ref APPBARDATA d);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
"@
$h = [TB]::FindWindow("Shell_TrayWnd", $null)
$d = New-Object TB+APPBARDATA
$d.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($d)
$d.hWnd = $h
$d.lParam = [IntPtr]2   # always-on-top, not auto-hide
[TB]::SHAppBarMessage(10, [ref]$d) | Out-Null   # ABM_SETSTATE
[TB]::ShowWindow($h, 5) | Out-Null
"Taskbar restored."

# Bring back any windows Liquid had parked in another space
$hf = Join-Path $PSScriptRoot 'hidden-windows.txt'
if (Test-Path $hf) {
  Add-Type @"
using System; using System.Runtime.InteropServices;
public static class HW { [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c); }
"@
  Get-Content $hf | % { [HW]::ShowWindow([IntPtr][Int64]$_, 4) | Out-Null }
  Remove-Item $hf
  "Hidden windows restored."
}