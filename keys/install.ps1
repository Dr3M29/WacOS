# Registers LiquidKeys.exe as the native messaging host for the Liquid Keys extension, for your
# Windows account only (HKCU), in every Chromium browser's location (Opera, Chrome, Edge, Brave).
#   powershell -ExecutionPolicy Bypass -File keys\install.ps1            install
#   powershell -ExecutionPolicy Bypass -File keys\install.ps1 -Remove    undo
# The extension itself is loaded from the browser: its extensions page > Developer mode >
# Load unpacked > the keys\extension folder
param([switch]$Remove)

$name = 'com.liquid.keys'
$extId = 'pgicggdhfghafaheccjhkpfkjkaiddoc'   # fixed by the "key" in extension\manifest.json
$manifest = Join-Path $PSScriptRoot "host\$name.json"
$locations = @(
    'HKCU:\Software\Google\Chrome\NativeMessagingHosts',
    'HKCU:\Software\Chromium\NativeMessagingHosts',
    'HKCU:\Software\Microsoft\Edge\NativeMessagingHosts',
    'HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts'
)

if ($Remove) {
    foreach ($l in $locations) { Remove-Item -Path "$l\$name" -ErrorAction SilentlyContinue }
    Remove-Item $manifest -ErrorAction SilentlyContinue
    "Liquid Keys helper unregistered. Your passkeys stay in %LOCALAPPDATA%\Liquid\keys."
    return
}

$exe = Join-Path $PSScriptRoot 'host\LiquidKeys.exe'
if (-not (Test-Path $exe)) { "Build it first: keys\build.bat"; exit 1 }
$json = [ordered]@{
    name = $name
    description = 'Liquid Keys: passkeys kept on this PC, unlocked by Air Ring face match'
    path = $exe
    type = 'stdio'
    allowed_origins = @("chrome-extension://$extId/")
} | ConvertTo-Json
[IO.File]::WriteAllText($manifest, $json, (New-Object Text.UTF8Encoding($false)))
foreach ($l in $locations) {
    New-Item -Path "$l\$name" -Force | Out-Null
    Set-ItemProperty -Path "$l\$name" -Name '(default)' -Value $manifest
}
"Liquid Keys helper registered for extension $extId"
