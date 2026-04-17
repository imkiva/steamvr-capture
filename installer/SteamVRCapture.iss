#ifndef MyAppName
  #define MyAppName "SteamVR Capture"
#endif
#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#ifndef MyAppPublisher
  #define MyAppPublisher "steamvr-capture"
#endif
#ifndef MySourceRoot
  #define MySourceRoot "..\dist\SteamVRCapture"
#endif
#ifndef MyOutputDir
  #define MyOutputDir "..\dist"
#endif

[Setup]
AppId={{73CF70E4-13F6-4D10-9B43-450F1C35C6EE}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
DefaultDirName={localappdata}\Programs\SteamVRCapture
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir={#MyOutputDir}
OutputBaseFilename=SteamVRCaptureSetup-{#MyAppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\tools\steamvr_capture_overlay.exe

[Files]
Source: "{#MySourceRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Dirs]
Name: "{userdocs}\SteamVR Capture\Sessions"

[Icons]
Name: "{group}\SteamVR Capture Replay"; Filename: "{app}\tools\steamvr_capture_overlay.exe"
Name: "{group}\SteamVR Capture Status"; Filename: "{app}\tools\steamvr_capture_setup_helper.exe"; Parameters: "--status"

[Run]
Filename: "{app}\tools\steamvr_capture_setup_helper.exe"; Parameters: "--status"; Flags: postinstall nowait skipifsilent unchecked; Description: "Show SteamVR Capture installation status"

[UninstallRun]
Filename: "{app}\tools\steamvr_capture_setup_helper.exe"; Parameters: "--uninstall"; Flags: runhidden waituntilterminated

[Code]
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    WizardForm.StatusLabel.Caption := 'Registering SteamVR Capture with SteamVR...';
    if (not Exec(ExpandConstant('{app}\tools\steamvr_capture_setup_helper.exe'),
      '--install', ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode)) or
      (ResultCode <> 0) then
    begin
      MsgBox('SteamVR Capture was copied, but SteamVR registration failed. Run tools\steamvr_capture_setup_helper.exe --status for details.',
        mbError, MB_OK);
      Abort;
    end;
  end;
end;
