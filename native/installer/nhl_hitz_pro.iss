; Inno Setup script for the NHL Hitz Pro recompilation (Windows 10/11, 64-bit).
; Build the player exe and the import tool first:
;   cmake -S native -B native/build-release -DNHL_RELEASE=ON
;   cmake --build native/build-release --config Release --target nhl_hitz_pro_recomp nhl_import nhl_config
; then compile this script with ISCC.exe. Output: native\installer\Output\NHLHitzPro-Setup-<version>.exe
; If the game is already installed (its game files are in place), Setup only updates the program files:
; it does not ask for the disc and does not copy the 1.5 GB of game files again.
#define AppVersion "1.0.7"
#include "pack_version.iss"
#include "pack_release.iss"
#define BuildDir "..\build-release\Release"

[Setup]
AppId={{6F1D2B7E-3C54-4A8B-9E10-4C0D7A55B1E2}
AppName=NHL Hitz Pro
AppVersion={#AppVersion}
AppPublisher=dalves408/nhlhitzpro-recomp
AppPublisherURL=https://github.com/dalves408/nhlhitzpro-recomp
DefaultDirName={autopf}\NHL Hitz Pro
DefaultGroupName=NHL Hitz Pro
DisableProgramGroupPage=yes
; an existing install is found by AppId: its folder is reused and the folder page is not shown
DisableDirPage=auto
UsePreviousAppDir=yes
; a running game or settings program is closed so its exe can be replaced
CloseApplications=yes
CloseApplicationsFilter=nhl_hitz_pro_recomp.exe,config.exe
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile=..\resources\nhl_hitz_pro.ico
; the picture at the top right of every wizard page is the same icon (the sizes are for different screen scalings)
WizardSmallImageFile=wizard_small_55.bmp,wizard_small_64.bmp,wizard_small_80.bmp,wizard_small_96.bmp,wizard_small_128.bmp
UninstallDisplayIcon={app}\nhl_hitz_pro_recomp.exe
OutputBaseFilename=NHLHitzPro-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ExtraDiskSpaceRequired=1700000000

[Messages]
WelcomeLabel2=This installs the NHL Hitz Pro PC recompilation.%n%nThe game's own files are not included. You need your own copy of the Xbox disc: a disc image (.iso or .xiso) or a folder with the game's files. Setup will ask for it and copy the files it needs.

[Files]
Source: "{#BuildDir}\nhl_hitz_pro_recomp.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\config.exe"; DestDir: "{app}"; Flags: ignoreversion
; the import tool only runs during setup; it is not installed
Source: "{#BuildDir}\nhl_import.exe"; DestDir: "{tmp}"; Flags: ignoreversion deleteafterinstall

[Icons]
Name: "{autoprograms}\NHL Hitz Pro"; Filename: "{app}\nhl_hitz_pro_recomp.exe"; WorkingDir: "{app}"
Name: "{autoprograms}\NHL Hitz Pro Settings"; Filename: "{app}\config.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\NHL Hitz Pro"; Filename: "{app}\nhl_hitz_pro_recomp.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{app}\game_files"
; the hi-res texture pack (added by its own installer) and that installer's uninstall files
Type: filesandordirs; Name: "{app}\texture_pack"
Type: filesandordirs; Name: "{app}\addons"

[Code]
var
  SrcPage, UpdatePage: TWizardPage;
  SrcEdit: TNewEdit;
  UpdateOnly, UpdateMode: Boolean;
  OldDir, OldVer, NewVer: String;
  VerRel: Integer;          { -1: the installed version is older, 0: same, 1: newer than this Setup }
  RepairRadio, QuitRadio: TNewRadioButton;
  QuitNow: Boolean;
  { hi-res texture pack }
  PackPage: TWizardPage;
  PackCheck: TNewCheckBox;
  PackInfo, PackNote: TNewStaticText;
  PackDlPage: TDownloadWizardPage;
  PackState: Integer;       { 0 not installed, 1 installed and every file matches, 2 another version, 3 damaged }
  PackSource: String;       { the add-on installer found next to this Setup, or '' }
  PackWanted: Boolean;
  PackCheckedDir: String;   { the folder whose pack was checked (the result is kept while it stays the same) }
  PackDeep: Boolean;        { the last check also hashed every file }
  PackDeepBtn: TNewButton;

{ The game files are in place: default.xbe and a few of the largest files, so an install that was
  interrupted halfway is not mistaken for a finished one. }
function GameFilesPresent(Dir: String): Boolean;
begin
  Result := FileExists(Dir + '\game_files\default.xbe') and
            FileExists(Dir + '\game_files\Sound\BaSpeech.dat') and
            FileExists(Dir + '\game_files\NIS\nis.pck') and
            FileExists(Dir + '\game_files\Jerseys\Jerseys.cmn');
end;

{ Where a previous Setup put the game, or '' }
function PreviousAppDir: String;
var
  Key, Dir: String;
begin
  Result := '';
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F1D2B7E-3C54-4A8B-9E10-4C0D7A55B1E2}_is1';
  if RegQueryStringValue(HKCU, Key, 'Inno Setup: App Path', Dir) or
     RegQueryStringValue(HKLM, Key, 'Inno Setup: App Path', Dir) then
    Result := RemoveBackslashUnlessRoot(Dir);
end;

{ The version a previous Setup recorded, or '' }
function PreviousVersion: String;
var
  Key, V: String;
begin
  Result := '';
  Key := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F1D2B7E-3C54-4A8B-9E10-4C0D7A55B1E2}_is1';
  if RegQueryStringValue(HKCU, Key, 'DisplayVersion', V) or
     RegQueryStringValue(HKLM, Key, 'DisplayVersion', V) then
    Result := V;
end;

{ The n-th dot-separated number of a version such as 1.0.2 (0 when missing) }
function VerPart(V: String; N: Integer): Integer;
var
  I, P: Integer;
begin
  for I := 1 to N do
  begin
    P := Pos('.', V);
    if P = 0 then begin V := ''; Break; end;
    V := Copy(V, P + 1, Length(V));
  end;
  P := Pos('.', V);
  if P > 0 then V := Copy(V, 1, P - 1);
  Result := StrToIntDef(V, 0);
end;

{ -1 if A is older than B, 0 if the same, 1 if newer }
function CompareVer(A, B: String): Integer;
var
  I: Integer;
begin
  Result := 0;
  for I := 0 to 3 do
    if VerPart(A, I) <> VerPart(B, I) then
    begin
      if VerPart(A, I) < VerPart(B, I) then Result := -1 else Result := 1;
      Exit;
    end;
end;

{ The page that says what was found and asks the player to go ahead }
var
  UpdY: Integer;

procedure AddLine(Text: String; Bold: Boolean);
var
  L: TNewStaticText;
begin
  L := TNewStaticText.Create(UpdatePage);
  L.Parent := UpdatePage.Surface;
  L.AutoSize := False;
  L.WordWrap := True;
  L.Left := 0;
  L.Top := UpdY;
  L.Width := UpdatePage.SurfaceWidth;
  if Bold then L.Font.Style := [fsBold];
  L.Caption := Text;
  L.Height := ScaleY(18) * (1 + Length(Text) div 70);
  UpdY := UpdY + L.Height + ScaleY(6);
end;

procedure RadioClick(Sender: TObject);
begin
  if QuitRadio.Checked then
    WizardForm.NextButton.Caption := 'Quit'
  else
    WizardForm.NextButton.Caption := 'Repair';
end;

procedure AddChoice(var R: TNewRadioButton; Text: String; Checked: Boolean);
begin
  R := TNewRadioButton.Create(UpdatePage);
  R.Parent := UpdatePage.Surface;
  R.Left := 0;
  R.Top := UpdY;
  R.Width := UpdatePage.SurfaceWidth;
  R.Height := ScaleY(20);
  R.Caption := Text;
  R.Checked := Checked;
  R.OnClick := @RadioClick;
  UpdY := UpdY + R.Height + ScaleY(6);
end;

procedure CreateUpdatePage;
begin
  if VerRel = 0 then
    UpdatePage := CreateCustomPage(wpWelcome, 'This version is already installed',
      'Setup found the same version of NHL Hitz Pro on this computer.')
  else
    UpdatePage := CreateCustomPage(wpWelcome, 'NHL Hitz Pro is already installed',
      'Setup found an existing installation of the game on this computer.');
  UpdY := 0;
  AddLine('Installed in:  ' + OldDir, False);
  if OldVer <> '' then
    AddLine('Installed version:  ' + OldVer, False)
  else
    AddLine('Installed version:  (not recorded)', False);
  AddLine('This Setup:  version ' + NewVer, True);
  UpdY := UpdY + ScaleY(10);
  if VerRel = 0 then
  begin
    AddLine('The installed version matches this Setup. You can repair the installation, which reinstalls the ' +
            'program files and shortcuts. Your game files, saves and settings are kept, and you do not need your disc.', False);
    UpdY := UpdY + ScaleY(10);
    AddChoice(RepairRadio, 'Repair the installation', True);
    AddChoice(QuitRadio, 'Quit (leave everything as it is)', False);
    Exit;
  end;
  if VerRel < 0 then
    AddLine('Setup will update the program files from version ' + OldVer + ' to version ' + NewVer +
            '. Your game files, saves and settings are kept, and you do not need your disc.', False)
  else
    AddLine('A NEWER version than this Setup is installed. Continuing will replace it with the older version ' +
            NewVer + '. Your game files, saves and settings are kept.', False);
  UpdY := UpdY + ScaleY(10);
  AddLine('Click Update to continue, or Cancel to leave the installed version as it is.', False);
end;

{ ---- hi-res texture pack (a separate add-on installer, run by this Setup) ---- }

function MB(Bytes: Int64): String;
begin
  Result := IntToStr((Bytes + 524288) div 1048576) + ' MB';
end;

{ The add-on installer, if it sits next to this Setup and has the expected size }
function FindPackBesideSetup: String;
var
  F: String;
  Size: Int64;
begin
  Result := '';
  F := ExtractFilePath(ExpandConstant('{srcexe}')) + '{#PackFileName}';
  if FileExists(F) and FileSize64(F, Size) and (Size = {#PackFileSize}) then
    Result := F;
end;

{ Compare the installed pack with the manifest of this release: first the manifest itself (is it the current
  release?), then the size and SHA-256 of every file in it (is it complete and undamaged?). }
procedure CheckInstalledPack(AppDir: String; Deep: Boolean);
var
  Dir, Sum: String;
  Lines: TArrayOfString;
  I, P, Q, Bad, N: Integer;
  Hash, Rel, F: String;
  Size, Want: Int64;
begin
  Dir := AppDir + '\texture_pack';
  PackState := 0;
  PackNote.Caption := '';
  if not FileExists(Dir + '\manifest.txt') then Exit;
  Sum := GetSHA256OfFile(Dir + '\manifest.txt');
  if CompareText(Sum, '{#PackManifestSha256}') <> 0 then
  begin
    PackState := 2;
    Exit;
  end;
  if not LoadStringsFromFile(Dir + '\manifest.txt', Lines) then
  begin
    PackState := 3;
    Exit;
  end;
  Bad := 0; N := GetArrayLength(Lines);
  for I := 0 to N - 1 do
  begin
    if Trim(Lines[I]) = '' then Continue;
    { <sha256> <size> <relative path> }
    P := Pos(' ', Lines[I]);
    Hash := Copy(Lines[I], 1, P - 1);
    Rel := Copy(Lines[I], P + 1, Length(Lines[I]));
    Q := Pos(' ', Rel);
    Want := StrToInt64Def(Copy(Rel, 1, Q - 1), -1);
    Rel := Copy(Rel, Q + 1, Length(Rel));
    StringChangeEx(Rel, '/', '\', True);
    F := Dir + '\' + Rel;
    { the quick check: the file is there and has the right size; the deep check also hashes it }
    if not (FileExists(F) and FileSize64(F, Size) and (Size = Want)) then
      Bad := Bad + 1
    else if Deep and (CompareText(GetSHA256OfFile(F), Hash) <> 0) then
      Bad := Bad + 1;
    if Deep and (I mod 25 = 0) then
    begin
      PackInfo.Caption := 'Checking every file of the installed texture pack... ' + IntToStr(I * 100 div N) + '%';
      PackInfo.Repaint;
    end;
  end;
  PackDeep := Deep;
  if Bad = 0 then PackState := 1
  else
  begin
    PackState := 3;
    PackNote.Caption := IntToStr(Bad) + ' of ' + IntToStr(N) + ' files are missing or do not match this release.';
  end;
end;

procedure ShowPackPage(Recheck: Boolean);
var
  Beside: Boolean;
begin
  if Recheck or (PackCheckedDir <> WizardDirValue) then
  begin
    PackInfo.Caption := 'Checking the installed texture pack...';
    PackCheck.Visible := False;
    PackDeepBtn.Visible := False;
    PackInfo.Repaint;
    CheckInstalledPack(WizardDirValue, False);   { quick: files present and the right size }
    PackCheckedDir := WizardDirValue;
  end;
  PackSource := FindPackBesideSetup;
  Beside := PackSource <> '';
  PackCheck.Visible := True;
  case PackState of
    1:
      begin
        if PackDeep then
          PackInfo.Caption := 'The hi-res texture pack is installed. Every file was checked and matches the current release (pack version {#PackVersion}).'
        else
          PackInfo.Caption := 'The hi-res texture pack is installed and complete (pack version {#PackVersion}: every file is present at the right size).';
        PackCheck.Caption := 'Install it again';
        PackCheck.Checked := False;
      end;
    2:
      begin
        PackInfo.Caption := 'A different version of the texture pack is installed. This Setup comes with pack version {#PackVersion}.';
        PackCheck.Caption := 'Update the hi-res texture pack';
        PackCheck.Checked := True;
      end;
    3:
      begin
        PackInfo.Caption := 'The installed texture pack is damaged or incomplete.';
        PackCheck.Caption := 'Repair the hi-res texture pack';
        PackCheck.Checked := True;
      end;
  else
  begin
    PackInfo.Caption := 'Adds 4x sharper textures: the ice and boards, team logos, jerseys and more. About ' +
      MB({#PackInstalledBytes}) + ' of disk space. The game runs without it.';
    if Beside then
    begin
      PackCheck.Caption := 'Install the hi-res texture pack (found next to this Setup)';
      PackCheck.Checked := True;
    end else
    begin
      PackCheck.Caption := 'Download and install the hi-res texture pack (' + MB({#PackFileSize}) + ' download)';
      PackCheck.Checked := False;
    end;
  end;
  end;
  PackDeepBtn.Visible := (PackState = 1) and not PackDeep;
  if PackState <> 0 then
    if Beside then
      PackNote.Caption := PackNote.Caption + ' The add-on installer next to this Setup will be used.'
    else
      PackNote.Caption := PackNote.Caption + ' It will be downloaded (' + MB({#PackFileSize}) + ').';
  if (PackState = 0) and not Beside then
    PackNote.Caption := 'The add-on installer ({#PackFileName}) was not found next to this Setup. ' +
      'Tick the box to download it from the internet; the download is checked before it is used.';
end;

function PackOnDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  if ProgressMax > 0 then
    PackDlPage.SetText('Downloading the hi-res texture pack', MB(Progress) + ' of ' + MB(ProgressMax));
  Result := True;
end;

procedure PackDeepClick(Sender: TObject);
begin
  PackDeepBtn.Visible := False;
  PackCheck.Visible := False;
  CheckInstalledPack(WizardDirValue, True);
  ShowPackPage(False);
end;

procedure CreatePackPage;
begin
  PackPage := CreateCustomPage(SrcPage.ID, 'Hi-res texture pack (optional)',
    'Sharper textures for the ice, boards, logos and jerseys.');
  PackInfo := TNewStaticText.Create(PackPage);
  PackInfo.Parent := PackPage.Surface;
  PackInfo.AutoSize := False; PackInfo.WordWrap := True;
  PackInfo.Left := 0; PackInfo.Top := 0;
  PackInfo.Width := PackPage.SurfaceWidth; PackInfo.Height := ScaleY(48);
  PackCheck := TNewCheckBox.Create(PackPage);
  PackCheck.Parent := PackPage.Surface;
  PackCheck.Left := 0; PackCheck.Top := PackInfo.Top + PackInfo.Height + ScaleY(8);
  PackCheck.Width := PackPage.SurfaceWidth; PackCheck.Height := ScaleY(20);
  PackNote := TNewStaticText.Create(PackPage);
  PackNote.Parent := PackPage.Surface;
  PackNote.AutoSize := False; PackNote.WordWrap := True;
  PackNote.Left := 0; PackNote.Top := PackCheck.Top + PackCheck.Height + ScaleY(12);
  PackNote.Width := PackPage.SurfaceWidth; PackNote.Height := ScaleY(60);
  PackDeepBtn := TNewButton.Create(PackPage);
  PackDeepBtn.Parent := PackPage.Surface;
  PackDeepBtn.Caption := 'Check every file (slower)';
  PackDeepBtn.Left := 0; PackDeepBtn.Top := PackNote.Top + ScaleY(70);
  PackDeepBtn.Width := ScaleX(190);
  PackDeepBtn.Visible := False;
  PackDeepBtn.OnClick := @PackDeepClick;
  PackDlPage := CreateDownloadPage('Downloading the hi-res texture pack', 'This can take several minutes.',
    @PackOnDownloadProgress);
end;

{ Free space for the download (temp drive) and the installed pack (game drive) }
function PackSpaceOk: Boolean;
var
  Free, Total: Int64;
begin
  Result := True;
  if GetSpaceOnDisk64(ExtractFileDrive(WizardDirValue), Free, Total) and (Free < {#PackInstalledBytes} + 200000000) then
  begin
    MsgBox('There is not enough free space on ' + ExtractFileDrive(WizardDirValue) + ' for the texture pack (about ' +
      MB({#PackInstalledBytes} + 200000000) + ' needed). Free some space or untick the pack.', mbError, MB_OK);
    Result := False;
    Exit;
  end;
  if (PackSource = '') and
     GetSpaceOnDisk64(ExtractFileDrive(ExpandConstant('{tmp}')), Free, Total) and (Free < {#PackFileSize} + 200000000) then
  begin
    MsgBox('There is not enough free space on ' + ExtractFileDrive(ExpandConstant('{tmp}')) +
      ' for the download (about ' + MB({#PackFileSize} + 200000000) + ' needed). Free some space or untick the pack.',
      mbError, MB_OK);
    Result := False;
  end;
end;

{ Make the add-on installer available: next to this Setup (checked against the release) or downloaded }
function PreparePackInstaller: Boolean;
var
  Url: String;
begin
  Result := True;
  if PackSource <> '' then
  begin
    WizardForm.StatusLabel.Caption := 'Checking the add-on installer...';
    if CompareText(GetSHA256OfFile(PackSource), '{#PackFileSha256}') = 0 then Exit;
    MsgBox('The add-on installer next to this Setup is damaged (its checksum does not match). It will be downloaded instead.',
      mbInformation, MB_OK);
    PackSource := '';
  end;
  Url := ExpandConstant('{param:PACKURL|{#PackUrl}}');
  PackDlPage.Clear;
  PackDlPage.Add(Url, '{#PackFileName}', '{#PackFileSha256}');
  PackDlPage.Show;
  try
    try
      PackDlPage.Download;
    except
      if PackDlPage.AbortedByUser then
        Log('Texture pack download cancelled')
      else
        MsgBox('The texture pack could not be downloaded:' + #13#10 + GetExceptionMessage, mbError, MB_OK);
      Result := False;
    end;
  finally
    PackDlPage.Hide;
  end;
end;

procedure BrowseFile(Sender: TObject);
var
  F: String;
begin
  F := SrcEdit.Text;
  if GetOpenFileName('Select the disc image', F, '', 'Xbox disc images (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*', 'iso') then
    SrcEdit.Text := F;
end;

procedure BrowseFolder(Sender: TObject);
var
  D: String;
begin
  D := SrcEdit.Text;
  if BrowseForFolder('Select the folder that holds the game''s files (the one with default.xbe)', D, False) then
    SrcEdit.Text := D;
end;

procedure InitializeWizard;
var
  L: TNewStaticText;
  B1, B2: TNewButton;
begin
  NewVer := '{#AppVersion}';
  OldDir := PreviousAppDir;
  UpdateMode := (OldDir <> '') and GameFilesPresent(OldDir);
  if UpdateMode then
  begin
    OldVer := PreviousVersion;
    if OldVer <> '' then VerRel := CompareVer(OldVer, NewVer) else VerRel := -1;
    WizardForm.WelcomeLabel2.Caption :=
      'NHL Hitz Pro is already installed on this computer.' + #13#10#13#10 +
      'Setup will show the installed version and this Setup''s version on the next page and ask you to confirm before it changes anything.';
    CreateUpdatePage;
  end;
  SrcPage := CreateCustomPage(wpSelectDir, 'Your copy of the game',
    'Select the disc image or folder that Setup should copy the game files from.');
  L := TNewStaticText.Create(SrcPage);
  L.Parent := SrcPage.Surface;
  L.Caption := 'Disc image (.iso, .xiso) or a folder containing default.xbe:';
  L.Left := 0; L.Top := 0;
  SrcEdit := TNewEdit.Create(SrcPage);
  SrcEdit.Parent := SrcPage.Surface;
  SrcEdit.Left := 0; SrcEdit.Top := L.Height + 8;
  SrcEdit.Width := SrcPage.SurfaceWidth;
  SrcEdit.Text := ExpandConstant('{param:SOURCE|}');   { setup /SOURCE=<image or folder> for silent installs }
  B1 := TNewButton.Create(SrcPage);
  B1.Parent := SrcPage.Surface;
  B1.Caption := 'Disc image...';
  B1.Left := 0; B1.Top := SrcEdit.Top + SrcEdit.Height + 8;
  B1.Width := ScaleX(110);
  B1.OnClick := @BrowseFile;
  B2 := TNewButton.Create(SrcPage);
  B2.Parent := SrcPage.Surface;
  B2.Caption := 'Folder...';
  B2.Left := B1.Left + B1.Width + 8; B2.Top := B1.Top;
  B2.Width := ScaleX(110);
  B2.OnClick := @BrowseFolder;
  CreatePackPage;
  { silent installs: setup /SILENT /HIRES=yes installs the texture pack too }
  PackSource := FindPackBesideSetup;
  PackWanted := CompareText(ExpandConstant('{param:HIRES|no}'), 'yes') = 0;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = PackPage.ID then ShowPackPage(False);
  if UpdateMode and (CurPageID = UpdatePage.ID) then
  begin
    if VerRel = 0 then RadioClick(nil) else WizardForm.NextButton.Caption := 'Update';
  end
  else if UpdateMode and UpdateOnly and (CurPageID = wpFinished) then
  begin
    if VerRel = 0 then
      WizardForm.FinishedLabel.Caption := 'NHL Hitz Pro ' + NewVer + ' was repaired.' + #13#10#13#10 +
        'Your game files, saves and settings were kept.'
    else
      WizardForm.FinishedLabel.Caption := 'NHL Hitz Pro was updated to version ' + NewVer + '.' + #13#10#13#10 +
        'Your game files, saves and settings were kept.';
  end;
end;

{ The "Ready to install" page says what is about to happen. }
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo, MemoTypeInfo, MemoComponentsInfo,
  MemoGroupInfo, MemoTasksInfo: String): String;
var
  S, Pk: String;
begin
  Pk := '';
  if PackWanted then
  begin
    Pk := NewLine + NewLine + 'Hi-res texture pack:' + NewLine + Space;
    if PackSource <> '' then Pk := Pk + 'installed from the add-on installer next to this Setup'
    else Pk := Pk + 'downloaded (' + MB({#PackFileSize}) + ') and installed';
  end;
  if UpdateMode and GameFilesPresent(WizardDirValue) then
  begin
    if VerRel = 0 then
      S := 'Repair NHL Hitz Pro ' + NewVer + ' in:'
    else
    begin
      S := 'Update NHL Hitz Pro';
      if OldVer <> '' then S := S + ' from version ' + OldVer;
      S := S + ' to version ' + NewVer + ' in:';
    end;
    S := S + NewLine + Space + WizardDirValue + NewLine + NewLine +
         'Only the program files are replaced. Your game files, saves and settings are kept.';
    if MemoTasksInfo <> '' then S := S + NewLine + NewLine + MemoTasksInfo;
    Result := S + Pk;
  end else
  begin
    S := '';
    if MemoDirInfo <> '' then S := S + MemoDirInfo + NewLine + NewLine;
    if MemoTasksInfo <> '' then S := S + MemoTasksInfo + NewLine;
    Result := S + Pk;
  end;
end;

{ No disc is needed when the game files are already in the chosen folder. }
function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = SrcPage.ID) and GameFilesPresent(WizardDirValue);
  { An installed pack that matches this release (quick check) needs no question and no download: the page is skipped }
  if PageID = PackPage.ID then
  begin
    if PackCheckedDir <> WizardDirValue then
    begin
      CheckInstalledPack(WizardDirValue, False);
      PackCheckedDir := WizardDirValue;
    end;
    if PackState = 1 then
    begin
      PackWanted := False;
      Result := True;
    end;
  end;
end;

procedure CancelButtonClick(CurPageID: Integer; var Cancel, Confirm: Boolean);
begin
  if QuitNow then Confirm := False;      { Quit on the matching-version page: no "Exit Setup?" question }
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if UpdateMode and (VerRel = 0) and (CurPageID = UpdatePage.ID) and QuitRadio.Checked then
  begin
    QuitNow := True;
    WizardForm.Close;
    Result := False;
    Exit;
  end;
  if CurPageID = PackPage.ID then
  begin
    PackWanted := PackCheck.Checked;
    if PackWanted and not PackSpaceOk then Result := False;
  end;
  if (CurPageID = wpReady) and PackWanted and not WizardSilent then
    if not PreparePackInstaller then
    begin
      if MsgBox('Continue installing without the hi-res texture pack? You can add it later with the add-on installer (' +
         '{#PackFileName}).', mbConfirmation, MB_YESNO) = IDYES then
        PackWanted := False
      else
        Result := False;
    end;
  if CurPageID = SrcPage.ID then
    if (Trim(SrcEdit.Text) = '') or
       not (FileExists(Trim(SrcEdit.Text)) or DirExists(Trim(SrcEdit.Text))) then
    begin
      MsgBox('Select an existing disc image or folder.', mbError, MB_OK);
      Result := False;
    end;
end;

{ Run the add-on installer for the pack, from next to this Setup or from the download }
procedure InstallPack;
var
  Exe: String;
  Code: Integer;
begin
  if WizardSilent then              { a silent install does not reinstall a pack that already matches }
  begin
    CheckInstalledPack(ExpandConstant('{app}'), False);
    if PackState = 1 then
    begin
      Log('The texture pack is installed and matches this release; skipped');
      Exit;
    end;
  end;
  Exe := PackSource;
  if Exe = '' then Exe := ExpandConstant('{tmp}\{#PackFileName}');
  if WizardSilent and (PackSource <> '') and (CompareText(GetSHA256OfFile(Exe), '{#PackFileSha256}') <> 0) then
  begin
    Log('The add-on installer next to Setup does not match this release; texture pack skipped');
    Exit;
  end;
  if not FileExists(Exe) then
  begin
    Log('No add-on installer available; texture pack skipped');
    Exit;
  end;
  WizardForm.StatusLabel.Caption := 'Installing the hi-res texture pack...';
  WizardForm.FilenameLabel.Caption := '';
  if not Exec(Exe, '/SILENT /SUPPRESSMSGBOXES /NORESTART /DIR="' + ExpandConstant('{app}') + '"', '',
              SW_SHOWNORMAL, ewWaitUntilTerminated, Code) or (Code <> 0) then
    MsgBox('The hi-res texture pack could not be installed. The game itself is installed and works without it. ' +
      'Run ' + '{#PackFileName}' + ' later to add the pack.', mbError, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Msg: String;
begin
  if CurStep = ssInstall then
    UpdateOnly := GameFilesPresent(ExpandConstant('{app}'));
  if (CurStep = ssPostInstall) and not UpdateOnly then
  begin
    WizardForm.StatusLabel.Caption := 'Copying the game files from your disc (about 1.5 GB, a few minutes)...';
    WizardForm.FilenameLabel.Caption := '';
    if not Exec(ExpandConstant('{tmp}\nhl_import.exe'),
        '"' + Trim(SrcEdit.Text) + '" "' + ExpandConstant('{app}\game_files') + '"',
        '', SW_HIDE, ewWaitUntilTerminated, Code) then
      Code := 99;
    if Code <> 0 then
    begin
      case Code of
        2: Msg := 'The disc image or folder could not be found.';
        3: Msg := 'That file is not an Xbox disc image.';
        4: Msg := 'That is not the supported version of NHL Hitz Pro (the Xbox USA release).';
        5: Msg := 'Some game files are missing or damaged. The dump may be incomplete.';
        6: Msg := 'The game files could not be written to the install folder.';
      else
        Msg := 'The game files could not be copied.';
      end;
      MsgBox(Msg + #13#10#13#10 + 'NHL Hitz Pro was installed but cannot start yet. ' +
        'Run Setup again and select a correct disc image or folder.', mbError, MB_OK);
    end;
  end;
  if (CurStep = ssPostInstall) and PackWanted then
    InstallPack;
end;
