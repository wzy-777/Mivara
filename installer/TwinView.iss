; TwinView (幻屏) Inno Setup script
; Build:  "D:\Program Files\Inno Setup 6\ISCC.exe" installer\TwinView.iss
; Output: dist\TwinView-Setup-<version>.exe

#define AppName "TwinView"
#define AppVersion "0.8.0"
#define AppPublisher "TwinView"
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
OutputBaseFilename=TwinView-Setup-{#AppVersion}
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
; binaries + runtime DLL closure
Source: "..\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs
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
Filename: "{app}\bin\{#AppExeName}"; Description: "启动 {#AppName}"; Flags: nowait postinstall skipifsilent
