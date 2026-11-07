; Standalone extraction-only Revision 2 texture setup.
#ifndef PackVersion
#define PackVersion "2"
#endif
#include "texture_payload.iss"
#ifndef BuildDir
#define BuildDir "..\build-release\Release"
#endif
#ifndef PackGuid
#define PackGuid "02D24D7F-1562-48D0-9A40-B559C7BE3EA9"
#endif
#ifndef GameGuid
#define GameGuid "6F1D2B7E-3C54-4A8B-9E10-4C0D7A55B1E2"
#endif

[Setup]
AppId={{{#PackGuid}}
AppName=NHL Hitz Pro Hi-Res Textures
AppVersion={#PackVersion}
AppPublisher=dalves408/nhlhitzpro-recomp
AppPublisherURL=https://github.com/dalves408/nhlhitzpro-recomp
DefaultDirName={code:GameDir}
UsePreviousAppDir=no
AppendDefaultDirName=no
DisableDirPage=no
DisableProgramGroupPage=yes
DirExistsWarning=no
Uninstallable=no
CreateUninstallRegKey=no
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
CloseApplications=no
SetupIconFile=..\resources\nhl_hitz_pro.ico
WizardSmallImageFile=wizard_small_55.bmp,wizard_small_64.bmp,wizard_small_80.bmp,wizard_small_96.bmp,wizard_small_128.bmp
OutputBaseFilename=NHLHitzPro-HiRes-Textures-{#PackVersion}-Setup
ArchiveExtraction=enhanced/nopassword
Compression=lzma2/max
SolidCompression=yes
; Keep compression resource use bounded while other work runs on the host.
LZMANumBlockThreads=1
WizardStyle=modern

[Messages]
WelcomeLabel2=Add the 4x hi-res textures to NHL Hitz Pro.%n%nSetup uses texture-01.bin and texture-02.bin beside this EXE if available. Missing parts will be downloaded. Interrupted downloads can be resumed by running Setup again.%n%nRequires game version 1.0.9 or newer.
SelectDirLabel3=Choose your installed NHL Hitz Pro game folder.
ButtonInstall=&Extract
ReadyLabel1=Ready to extract the hi-res texture pack.
ReadyLabel2a=Click Extract to continue, or Back to choose another game folder.
StatusExtractFiles=Extracting texture pack...
FinishedHeadingLabel=Texture pack extracted
FinishedLabel=Open config.exe: set Render Scale to 2x or higher, enable Use the hi-res textures on the Textures tab, then Save.%n%nOr press F1 in the game: set Render Scale to 2x or higher and turn Hi-res textures On.

[Files]
Source: "{#BuildDir}\nhl_texture_download.exe"; Flags: dontcopy
Source: "{#BuildDir}\config.exe"; DestDir: "{app}"; Flags: replacesameversion

[Code]
#include "texture_download_code.iss"
const
  GameKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{{#GameGuid}}_is1';

function GameExists(Dir: String): Boolean;
begin
  Result := FileExists(Dir + '\nhl_hitz_pro_recomp.exe');
end;

function FindGameDir: String;
var Dir: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, GameKey, 'Inno Setup: App Path', Dir) then
    if GameExists(RemoveBackslashUnlessRoot(Dir)) then
      Result := RemoveBackslashUnlessRoot(Dir);
  if Result = '' then
    if RegQueryStringValue(HKLM, GameKey, 'Inno Setup: App Path', Dir) then
      if GameExists(RemoveBackslashUnlessRoot(Dir)) then
        Result := RemoveBackslashUnlessRoot(Dir);
end;

function GameDir(Param: String): String;
begin
  Result := FindGameDir;
  if Result = '' then Result := 'C:\Games\NHL Hitz Pro';
end;

function SupportedGame(Exe: String): Boolean;
var MS, LS: Cardinal;
begin
  Result := False;
  if GetVersionNumbers(Exe, MS, LS) then
    Result := (MS > $00010000) or ((MS = $00010000) and (LS >= $00090000));
end;

function CheckGame(Dir: String): String;
begin
  Result := '';
  if not GameExists(Dir) then
    Result := 'Choose the installed NHL Hitz Pro game folder (normal or debug build).'
  else if not SupportedGame(Dir + '\nhl_hitz_pro_recomp.exe') then
    Result := 'This DDS texture add-on requires NHL Hitz Pro version 1.0.9 or newer. Update the game first.';
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var Error: String;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not WizardSilent then begin
    Error := CheckGame(RemoveBackslashUnlessRoot(WizardDirValue));
    if Error <> '' then begin
      MsgBox(Error, mbError, MB_OK);
      Result := False;
    end;
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  { Silent installs must fail cleanly instead of waiting at a directory-page dialog. }
  Result := CheckGame(RemoveBackslashUnlessRoot(ExpandConstant('{app}')));
  if Result = '' then Result := PrepareTexturePayload(1048576);
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpReady then begin
    if TexturePackNeedsInstall then begin
      WizardForm.NextButton.Caption := SetupMessage(msgButtonInstall);
      WizardForm.PageNameLabel.Caption := SetupMessage(msgWizardReady);
      WizardForm.PageDescriptionLabel.Caption := SetupMessage(msgReadyLabel1);
      WizardForm.ReadyLabel.Caption := SetupMessage(msgReadyLabel2a);
    end else begin
      WizardForm.NextButton.Caption := '&Continue';
      WizardForm.PageNameLabel.Caption := 'Texture pack already installed';
      WizardForm.PageDescriptionLabel.Caption := 'The current release is already present in this game folder.';
      WizardForm.ReadyLabel.Caption := 'Click Continue to finish. No texture download or extraction is needed.';
    end;
  end;
  if (CurPageID = wpFinished) and TexturePackAlreadyCurrent then begin
    WizardForm.FinishedHeadingLabel.Caption := 'Texture pack already installed';
    WizardForm.FinishedLabel.Caption := 'The current 4x texture pack is already installed. No download or extraction was needed.';
  end;
end;

function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := MemoDirInfo;
  if not TexturePackNeedsInstall then
    Result := Result + NewLine + NewLine + 'The current 4x texture pack is already installed. No download or extraction is needed.';
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  TextureCancelButtonClick(Cancel, Confirm);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    InstallTexturePack;
  end;
  if CurStep = ssDone then TextureFinishCacheChoice;
end;

procedure DeinitializeSetup;
begin
  TextureCleanupOnExit;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if not TexturePackInstalled then Result := 1;
end;
