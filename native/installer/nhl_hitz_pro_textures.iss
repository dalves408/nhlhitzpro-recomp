; Inno Setup script for the NHL Hitz Pro hi-res texture pack (the add-on installer).
; It puts the pack into <game folder>\texture_pack, where the game looks for it. The game's Setup can run it
; (setup /SILENT /DIR=<game folder>) or the player can run it alone.
; Build:  python gen_pack_manifest.py manifest <pack folder>      (writes manifest.txt and pack_version.iss)
;         ISCC /DPackDir=<pack folder> nhl_hitz_pro_textures.iss  -> Output\NHLHitzPro-HiRes-Textures-<version>.exe
;         python gen_pack_manifest.py release <pack folder> <that exe> <download url>   (for the game's Setup)
#include "pack_version.iss"
#ifndef PackDir
#define PackDir "..\..\analysis\texture_pack"
#endif

[Setup]
AppId={{B3A5C2D4-7E91-4F0A-8C36-2D5E9A41F7B8}
AppName=NHL Hitz Pro Hi-Res Textures
AppVersion={#PackVersion}
AppPublisher=dalves408/nhlhitzpro-recomp
AppPublisherURL=https://github.com/dalves408/nhlhitzpro-recomp
; the pack goes into the game's own folder; it is found from the game's uninstall entry
DefaultDirName={code:GameDir}
UsePreviousAppDir=no
DisableProgramGroupPage=yes
; the add-on's uninstaller must not collide with the game's unins000.exe in the same folder
UninstallFilesDir={app}\addons\hires_textures
UninstallDisplayName=NHL Hitz Pro Hi-Res Textures
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile=..\resources\nhl_hitz_pro.ico
WizardSmallImageFile=wizard_small_55.bmp,wizard_small_64.bmp,wizard_small_80.bmp,wizard_small_96.bmp,wizard_small_128.bmp
OutputBaseFilename=NHLHitzPro-HiRes-Textures-{#PackVersion}
; the pictures are PNG files, already compressed: a fast setting avoids minutes of work for no gain
Compression=zip/1
SolidCompression=no
WizardStyle=modern

[Messages]
WelcomeLabel2=This adds the hi-res (4x) texture pack to NHL Hitz Pro: sharper ice, boards, logos and jerseys.%n%nThe game must already be installed. The pack is copied into the game's folder.

[InstallDelete]
; a pack of an older version is replaced as a whole, so no stale file stays behind
Type: filesandordirs; Name: "{app}\texture_pack"

[Files]
Source: "{#PackDir}\*"; DestDir: "{app}\texture_pack"; Flags: recursesubdirs createallsubdirs ignoreversion

[UninstallDelete]
Type: filesandordirs; Name: "{app}\texture_pack"
Type: dirifempty; Name: "{app}\addons"

[Code]
const
  GameKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F1D2B7E-3C54-4A8B-9E10-4C0D7A55B1E2}_is1';

{ The folder the game's Setup installed into, or '' }
function FindGameDir: String;
var
  Dir: String;
begin
  Result := '';
  if RegQueryStringValue(HKCU, GameKey, 'Inno Setup: App Path', Dir) or
     RegQueryStringValue(HKLM, GameKey, 'Inno Setup: App Path', Dir) then
    if FileExists(RemoveBackslashUnlessRoot(Dir) + '\nhl_hitz_pro_recomp.exe') then
      Result := RemoveBackslashUnlessRoot(Dir);
end;

function GameDir(Param: String): String;
begin
  Result := FindGameDir;
  if Result = '' then Result := ExpandConstant('{autopf}\NHL Hitz Pro');
end;

procedure InitializeWizard;
begin
  if (FindGameDir = '') and not WizardSilent then
    MsgBox('Setup did not find an installed NHL Hitz Pro on this computer.' + #13#10#13#10 +
      'Install the game first, or on the next pages choose the folder that holds nhl_hitz_pro_recomp.exe.',
      mbInformation, MB_OK);
end;

{ No folder question when the game was found }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = wpSelectDir) and (FindGameDir <> '');
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = wpSelectDir then
    if not FileExists(WizardDirValue + '\nhl_hitz_pro_recomp.exe') then
    begin
      MsgBox('That folder does not contain nhl_hitz_pro_recomp.exe. Choose the folder NHL Hitz Pro is installed in.',
        mbError, MB_OK);
      Result := False;
    end;
end;
