# WacOS setup: builds Liquid, gets Air Ring ready, and starts WacOS when you sign in.
#   powershell -ExecutionPolicy Bypass -File tools\setup.ps1
#   -NoAirRing   skip the webcam features (Face ID, walk-away lock, air gestures)
#   -NoStart     don't start WacOS at the end
# Run it again any time (after pulling an update, say): it only redoes what's missing.
param([switch]$NoAirRing, [switch]$NoStart)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
function Step($t) { Write-Host "`n== $t" -ForegroundColor Cyan }

$build = [int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuild
if ($build -lt 22000) { throw "WacOS needs Windows 11 (this PC is on build $build)." }

# ---- Liquid (the shell)
Step 'Building Liquid'
$running = Get-Process Liquid -ErrorAction SilentlyContinue
if ($running) { & "$root\Liquid.exe" --quit-for-reload }
& cmd /c "`"$root\build.bat`""
if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$root\Liquid.exe")) { throw 'The build failed (see the messages above).' }

# ---- Air Ring (Python): Liquid starts it by itself once this is done
if (-not $NoAirRing) {
    Step 'Setting up Air Ring (webcam: Face ID, walk-away lock, air gestures)'
    $air = Join-Path $root 'air-ring'
    $venv = Join-Path $air '.venv'
    if (-not (Test-Path "$venv\Scripts\pythonw.exe")) {
        $ok = $false
        if (Get-Command py -ErrorAction SilentlyContinue) {
            & py -3.12 -c "import sys" 2>$null
            $ok = $LASTEXITCODE -eq 0
        }
        if (-not $ok) { throw 'Air Ring needs Python 3.12: winget install Python.Python.3.12 (or python.org). Or run setup with -NoAirRing.' }
        & py -3.12 -m venv $venv
    }
    & "$venv\Scripts\python.exe" -m pip install --disable-pip-version-check --quiet -r "$air\requirements.txt"
    if ($LASTEXITCODE -ne 0) { throw "Installing Air Ring's Python packages failed." }

    # the models it runs (MediaPipe: Apache 2.0; OpenCV Zoo YuNet: MIT, SFace: Apache 2.0)
    $models = Join-Path $air 'models'
    New-Item -ItemType Directory -Force $models | Out-Null
    $urls = [ordered]@{
        'hand_landmarker.task'                = 'https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/latest/hand_landmarker.task'
        'pose_landmarker_lite.task'           = 'https://storage.googleapis.com/mediapipe-models/pose_landmarker/pose_landmarker_lite/float16/latest/pose_landmarker_lite.task'
        'blaze_face_short_range.tflite'       = 'https://storage.googleapis.com/mediapipe-models/face_detector/blaze_face_short_range/float16/latest/blaze_face_short_range.tflite'
        'face_detection_yunet_2023mar.onnx'   = 'https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx'
        'face_recognition_sface_2021dec.onnx' = 'https://github.com/opencv/opencv_zoo/raw/main/models/face_recognition_sface/face_recognition_sface_2021dec.onnx'
    }
    $ProgressPreference = 'SilentlyContinue'
    foreach ($name in $urls.Keys) {
        $dst = Join-Path $models $name
        if (Test-Path $dst) { continue }
        Write-Host "  downloading $name"
        Invoke-WebRequest -Uri $urls[$name] -OutFile "$dst.part" -UseBasicParsing
        Move-Item "$dst.part" $dst -Force
    }
}

# ---- start with Windows (System Settings > General > Login Items turns it off again)
Step 'Starting WacOS when you sign in'
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Liquid' -Value "`"$root\Liquid.exe`""

if (-not $NoStart) {
    Step 'Starting WacOS'
    Start-Process "$root\Liquid.exe"
}
Write-Host ''
Write-Host 'Done. The first time it runs, the lock screen asks you to choose a PIN.'
Write-Host 'Quit any time with Ctrl+Alt+Shift+Q (the Windows taskbar comes back).'
