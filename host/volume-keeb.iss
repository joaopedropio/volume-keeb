; The Windows installer (Inno Setup 6): build_windows.ps1 compiles it after
; PyInstaller, into dist\volume-keeb-setup.exe. It installs for the current
; user only (no admin prompt), into %LOCALAPPDATA%\Programs\volume-keeb, with
; a Start menu shortcut and, if ticked, "Start with Windows": the same HKCU
; Run value the tray menu's checkbox (autostart.py) reads and writes.
; Settings, fonts and gifs stay in %APPDATA%\volume-keeb, and are kept on
; uninstall. Before installing it shows what the app sends over the network
; (privacy-*.txt), as SignPath Foundation's code signing terms ask.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppId={{5B0C3E4A-8F2D-4C6B-9A71-3E5D2F1B7C90}
AppName=Volume Keeb
AppVersion={#AppVersion}
AppVerName=Volume Keeb {#AppVersion}
AppPublisher=joaopedropio
AppPublisherURL=https://github.com/joaopedropio/volume-keeb
AppSupportURL=https://github.com/joaopedropio/volume-keeb/issues
AppUpdatesURL=https://github.com/joaopedropio/volume-keeb/releases/latest
DefaultDirName={autopf}\volume-keeb
DefaultGroupName=Volume Keeb
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=dist
OutputBaseFilename=volume-keeb-setup
SetupIconFile=volume-keeb.ico
UninstallDisplayIcon={app}\volume-keeb.exe
UninstallDisplayName=Volume Keeb
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
VersionInfoProductName=Volume Keeb
VersionInfoProductVersion={#AppVersion}
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Volume Keeb installer
VersionInfoCompany=joaopedropio
VersionInfoCopyright=GPL-2.0-or-later
; An update closes the running app (it holds the .exe) and starts it again.
CloseApplications=force
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"; InfoBeforeFile: "privacy-en.txt"
Name: "brazilianportuguese"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"; InfoBeforeFile: "privacy-pt.txt"

[CustomMessages]
english.AutoStart=Start with Windows
brazilianportuguese.AutoStart=Iniciar com o Windows

[Tasks]
Name: "autostart"; Description: "{cm:AutoStart}"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "dist\volume-keeb.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\Volume Keeb"; Filename: "{app}\volume-keeb.exe"
Name: "{group}\{cm:UninstallProgram,Volume Keeb}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Volume Keeb"; Filename: "{app}\volume-keeb.exe"; Tasks: desktopicon

[InstallDelete]
; Shortcuts from before the app was called "Volume Keeb" (up to v0.6.1).
Type: files; Name: "{group}\volume-keeb.lnk"
Type: files; Name: "{group}\{cm:UninstallProgram,volume-keeb}.lnk"
Type: files; Name: "{autodesktop}\volume-keeb.lnk"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "volume-keeb"; \
    ValueData: """{app}\volume-keeb.exe"""; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\volume-keeb.exe"; Parameters: "--open"; Description: "{cm:LaunchProgram,Volume Keeb}"; \
    Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM volume-keeb.exe"; Flags: runhidden; RunOnceId: "StopApp"

[Code]
// The tray's "Start with Windows" may have set the Run value even if the
// task wasn't ticked; uninstalling removes it either way.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'volume-keeb');
end;
