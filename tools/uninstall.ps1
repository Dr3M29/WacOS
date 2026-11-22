# Turns WacOS off: quits it, stops it starting at sign-in, brings the Windows taskbar back and
# unregisters Liquid Keys if it was installed. The folder itself is left for you to delete.
#   powershell -ExecutionPolicy Bypass -File tools\uninstall.ps1
$root = Split-Path $PSScriptRoot -Parent
if (Get-Process Liquid -ErrorAction SilentlyContinue) {
    & "$root\Liquid.exe" --quit
    Start-Sleep -Seconds 2
}
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Liquid' -ErrorAction SilentlyContinue
& powershell -NoProfile -ExecutionPolicy Bypass -File "$root\restore-taskbar.ps1"
if (Test-Path "$root\keys\host\com.liquid.keys.json") {
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$root\keys\install.ps1" -Remove
}
'WacOS is off and won''t start at sign-in. Delete the folder whenever you like.'
'Your passkeys (if any) stay in %LOCALAPPDATA%\Liquid\keys until you delete that too.'
