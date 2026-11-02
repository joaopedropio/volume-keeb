# Builds dist\volume-keeb.exe (one file, no console window, tray icon) and
# its installer, dist\volume-keeb-setup.exe (volume-keeb.iss, Inno Setup 6).
# Run from the host\ folder in PowerShell, with Python 3.10+ on PATH:
#   powershell -ExecutionPolicy Bypass -File build_windows.ps1
$ErrorActionPreference = "Stop"
python -m venv .venv
.\.venv\Scripts\python -m pip install --upgrade pip
.\.venv\Scripts\python -m pip install -r requirements.txt pyinstaller
.\.venv\Scripts\python -c "from volume_keeb.tray import icon_image; icon_image(256).save('volume-keeb.ico', sizes=[(16,16),(32,32),(48,48),(256,256)])"

# The .exe's product name and version (code signing checks them against the
# signing policy), from the repo's VERSION.
$version = (Get-Content ..\VERSION -Raw).Trim()
$v = ($version.Split(".") + @("0")) -join ", "
@"
VSVersionInfo(
  ffi=FixedFileInfo(filevers=($v), prodvers=($v), mask=0x3f, flags=0x0, OS=0x40004, fileType=0x1, subtype=0x0, date=(0, 0)),
  kids=[
    StringFileInfo([StringTable('040904B0', [
      StringStruct('CompanyName', 'joaopedropio'),
      StringStruct('FileDescription', 'Volume Keeb'),
      StringStruct('FileVersion', '$version'),
      StringStruct('InternalName', 'volume-keeb'),
      StringStruct('LegalCopyright', 'GPL-2.0-or-later'),
      StringStruct('OriginalFilename', 'volume-keeb.exe'),
      StringStruct('ProductName', 'Volume Keeb'),
      StringStruct('ProductVersion', '$version')])]),
    VarFileInfo([VarStruct('Translation', [1033, 1200])])
  ]
)
"@ | Set-Content -Encoding UTF8 version_info.txt

.\.venv\Scripts\pyinstaller --noconfirm --onefile --windowed --name volume-keeb `
    --icon volume-keeb.ico `
    --version-file version_info.txt `
    --add-data "volume_keeb/web;volume_keeb/web" `
    --add-data "../VERSION;." `
    --collect-submodules winrt `
    --hidden-import volume_keeb.audio_windows `
    --hidden-import volume_keeb.autostart `
    --hidden-import pystray._win32 `
    volume_sync.py
Write-Host "built dist\volume-keeb.exe"

$iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
if (-not (Test-Path $iscc)) {
    # GitHub's Windows runners have Chocolatey; elsewhere, install Inno Setup 6 by hand.
    choco install innosetup -y --no-progress
}
& $iscc "/DAppVersion=$version" volume-keeb.iss
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed" }
Write-Host "built dist\volume-keeb-setup.exe"
