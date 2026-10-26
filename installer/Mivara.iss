; Mivara (幻屏) Inno Setup script
; Build:  ISCC.exe installer\Mivara.iss   (set ISCC=... if ISCC is not on PATH)
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
SetupIconFile=..\res\mivara.ico
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\bin\{#AppExeName}
WizardStyle=modern

; Wizard UI language: this Inno install has no ChineseSimplified.isl.
; To get a Chinese wizard, download ChineseSimplified.isl from the inno
; setup repo into "<Inno Setup install dir>\Languages\" and add:
;   [Languages] Name: "chinesesimp"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式(&D)"; GroupDescription: "附加任务:"
; Exactly one of the two VDD tasks is shown, decided by VddDevicePresent().
; A fresh machine gets the checked "install" task; a machine that already has
; the Root\MttVDD device gets an unchecked "reinstall" task instead.
Name: "installvdd"; Description: "安装虚拟显示驱动（扩展模式需要，推荐）"; GroupDescription: "附加任务:"; Flags: checkedonce; Check: not VddDevicePresent
Name: "reinstallvdd"; Description: "重新安装虚拟显示驱动（已检测到驱动，通常无需勾选）"; GroupDescription: "附加任务:"; Flags: unchecked; Check: VddDevicePresent

[Code]
// True when the Root\MttVDD device exists in any state (including the normal
// "disabled, code 22" state left behind by vddShutdown). Enum\ROOT\DISPLAY is
// where the driver's Display-class devnode lives; match the hardware ID rather
// than a fixed instance number, and never match other vendors' virtual displays.
function VddDevicePresent(): Boolean;
var
  Names: TArrayOfString;
  Key, Ids, Id: String;
  I: Integer;
begin
  Result := False;
  if not RegGetSubkeyNames(HKLM, 'SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY', Names) then
    exit;
  for I := 0 to GetArrayLength(Names) - 1 do
  begin
    Key := 'SYSTEM\CurrentControlSet\Enum\ROOT\DISPLAY\' + Names[I];
    // RegQueryMultiStringValue returns the REG_MULTI_SZ items in one String,
    // separated by #0 (Inno Setup 6.6 signature), not a TArrayOfString.
    if RegQueryMultiStringValue(HKLM, Key, 'HardwareID', Ids) then
    begin
      Ids := Ids + #0;
      while Ids <> '' do
      begin
        Id := Copy(Ids, 1, Pos(#0, Ids) - 1);
        Ids := Copy(Ids, Pos(#0, Ids) + 1, Length(Ids));
        if CompareText(Id, 'Root\MttVDD') = 0 then
        begin
          Result := True;
          exit;
        end;
      end;
    end;
  end;
end;

procedure InstallVirtualDisplayDriver;
var
  ResultCode: Integer;
  Installer: String;
  LogPath: String;
  Params: String;
  Reinstall: Boolean;
begin
  Reinstall := IsTaskSelected('reinstallvdd');
  if (not IsTaskSelected('installvdd')) and (not Reinstall) then
    exit;
  Installer := ExpandConstant('{app}\vdd\install_vdd.ps1');
  LogPath := ExpandConstant('{app}\vdd\install_log.txt');
  Params := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + Installer + '"';
  if Reinstall then
    Params := Params + ' -Reinstall';
  Log('Installing the virtual display driver for Extend mode.');
  // The 32-bit setup must start 64-bit PowerShell: {sys} is redirected to
  // SysWOW64 for a 32-bit process, where pnputil.exe does not exist.
  if not Exec(ExpandConstant('{sysnative}\WindowsPowerShell\v1.0\powershell.exe'),
              Params,
              ExpandConstant('{app}\vdd'), SW_HIDE, ewWaitUntilTerminated, ResultCode) then
  begin
    Log(Format('Could not start the VDD installer: %s', [SysErrorMessage(ResultCode)]));
    if WizardSilent then
      RaiseException('Virtual display driver installation failed. See ' + LogPath);
    SuppressibleMsgBox('The virtual display driver installer could not be started. ' +
      'Mirror and Receive are still available. For Extend mode, run install_vdd.ps1 ' +
      'from an administrator PowerShell. Log: ' + LogPath,
      mbError, MB_OK, IDOK);
    exit;
  end;
  if ResultCode <> 0 then
  begin
    Log(Format('VDD installation failed with exit code %d.', [ResultCode]));
    if WizardSilent then
      RaiseException('Virtual display driver installation failed. See ' + LogPath);
    SuppressibleMsgBox('Mivara was installed, but the virtual display driver could not ' +
      'be installed. Mirror and Receive are still available. For Extend mode, review ' +
      'the log and run install_vdd.ps1 from an administrator PowerShell. Log: ' + LogPath,
      mbError, MB_OK, IDOK);
    exit;
  end;
  Log('Virtual display driver installation completed.');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    InstallVirtualDisplayDriver;
end;

[Files]
; binaries + runtime DLL closure (sender_noUAC.exe is a local test artifact, never ship it)
Source: "..\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs; Excludes: "sender_noUAC.exe"
; helper scripts
Source: "..\scripts\*"; DestDir: "{app}\scripts"; Flags: ignoreversion
; docs — shipped offline so the relative links inside the installed READMEs resolve
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README_EN.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\CODE_SIGNING_POLICY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\SECURITY.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD-PARTY-NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSES\*"; DestDir: "{app}\LICENSES"; Flags: ignoreversion recursesubdirs
Source: "..\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs
; virtual-display driver (extend mode) – excludes the >100MB vendor GUI
Source: "..\vdd\install_vdd.ps1"; DestDir: "{app}\vdd"; Flags: ignoreversion
Source: "..\vdd\LICENSE"; DestDir: "{app}\vdd"; Flags: ignoreversion
Source: "..\vdd\install_vdd2.ps1"; DestDir: "{app}\vdd"; Flags: ignoreversion
Source: "..\vdd\Dependencies\*"; DestDir: "{app}\vdd\Dependencies"; Flags: ignoreversion recursesubdirs
; both driver packages: install_vdd.ps1 picks x86/AMD64 or ARM64 for the machine
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
