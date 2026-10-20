; Mivara (幻屏) Inno Setup script
; Build:  "D:\Program Files\Inno Setup 6\ISCC.exe" installer\Mivara.iss
; Output: dist\Mivara-Setup-<version>.exe

#define AppName "Mivara"
; Single version source: src/version.h. That file must stay comment-free —
; an inline comment after a directive aborts ISCC. Bump it, then the README
; badges + winget manifests by hand (release checklist in AGENTS.md).
#include "..\src\version.h"
#define AppVersion MIVARA_VERSION
#define AppPublisher "Mivara"
#define AppExeName "app.exe"

[Setup]
AppId={{8F3C2A61-5D4E-4B7A-9C1F-2B0A6E4D5100}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=Mivara-Setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\bin\{#AppExeName}
WizardStyle=modern

; Wizard UI language: this Inno install has no ChineseSimplified.isl.
; To get a Chinese wizard, download ChineseSimplified.isl from the inno
; setup repo into "D:\Program Files\Inno Setup 6\Languages\" and add:
;   [Languages] Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; GroupDescription: "附加图标:"

[Files]
; binaries + runtime DLL closure (sender_noUAC.exe is a local test artifact, never ship it)
Source: "..\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs; Excludes: "sender_noUAC.exe"
; helper scripts
Source: "..\scripts\*"; DestDir: "{app}\scripts"; Flags: ignoreversion
; docs
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
; virtual-display driver (extend mode) – excludes the >100MB vendor GUI
Source: "..\vdd\install_vdd.ps1"; DestDir: "{app}\vdd"; Flags: ignoreversion
Source: "..\vdd\install_vdd2.ps1"; DestDir: "{app}\vdd"; Flags: ignoreversion
Source: "..\vdd\Dependencies\*"; DestDir: "{app}\vdd\Dependencies"; Flags: ignoreversion recursesubdirs
Source: "..\vdd\SignedDrivers\*"; DestDir: "{app}\vdd\SignedDrivers"; Flags: ignoreversion recursesubdirs

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\bin\{#AppExeName}"
Name: "{group}\接收端 (receiver)"; Filename: "{app}\bin\receiver.exe"
Name: "{group}\卸载 {#AppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\bin\{#AppExeName}"; Tasks: desktopicon

[Run]
; runascurrentuser is REQUIRED: postinstall entries default to runasoriginaluser
; (unelevated), but app.exe embeds a requireAdministrator manifest, so a plain
; CreateProcess from the unelevated original user fails with ERROR_ELEVATION_REQUIRED.
; runascurrentuser inherits Setup's elevated token — the checkbox just works, no 2nd UAC.
Filename: "{app}\bin\{#AppExeName}"; Description: "启动 {#AppName}"; WorkingDir: "{app}\bin"; Flags: nowait postinstall skipifsilent runascurrentuser

[UninstallRun]
; remove the auto-start scheduled task registered by app.exe (task name must match app.cpp)
Filename: "{sys}\schtasks.exe"; Parameters: "/delete /tn ""Mivara"" /f"; Flags: runhidden; RunOnceId: "DelAutoStartTask"

[UninstallDelete]
; update staging directory (created by app.exe with an admin-only DACL, so it
; must be removed from an elevated context)
Type: filesandordirs; Name: "{commonappdata}\Mivara"
