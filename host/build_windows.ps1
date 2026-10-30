# Builds dist\volume-keeb.exe: one file, no console window, tray icon.
# Run from the host\ folder in PowerShell, with Python 3.10+ on PATH:
#   powershell -ExecutionPolicy Bypass -File build_windows.ps1
$ErrorActionPreference = "Stop"
python -m venv .venv
.\.venv\Scripts\python -m pip install --upgrade pip
.\.venv\Scripts\python -m pip install -r requirements.txt pyinstaller
.\.venv\Scripts\python -c "from volume_keeb.tray import icon_image; icon_image(256).save('volume-keeb.ico', sizes=[(16,16),(32,32),(48,48),(256,256)])"
.\.venv\Scripts\pyinstaller --noconfirm --onefile --windowed --name volume-keeb `
    --icon volume-keeb.ico `
    --add-data "volume_keeb/web;volume_keeb/web" `
    --collect-submodules winrt `
    --hidden-import volume_keeb.audio_windows `
    --hidden-import volume_keeb.autostart `
    --hidden-import pystray._win32 `
    volume_sync.py
Write-Host "built dist\volume-keeb.exe"
