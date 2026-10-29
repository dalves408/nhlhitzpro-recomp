; Inno Setup script for the NHL Hitz Pro recompilation (Windows 10/11, 64-bit).
; Build the player exe and the import tool first:
;   cmake -S native -B native/build-release -DNHL_RELEASE=ON
;   cmake --build native/build-release --config Release --target nhl_hitz_pro_recomp nhl_import
; then compile this script with ISCC.exe. Output: native\installer\Output\NHLHitzPro-Setup-1.0.0.exe
#define AppVersion "1.0.0"
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
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
SetupIconFile=..\resources\nhl_hitz_pro.ico
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
; the import tool only runs during setup; it is not installed
Source: "{#BuildDir}\nhl_import.exe"; DestDir: "{tmp}"; Flags: ignoreversion deleteafterinstall

[Icons]
Name: "{autoprograms}\NHL Hitz Pro"; Filename: "{app}\nhl_hitz_pro_recomp.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\NHL Hitz Pro"; Filename: "{app}\nhl_hitz_pro_recomp.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[UninstallDelete]
Type: filesandordirs; Name: "{app}\game_files"

[Code]
var
  SrcPage: TWizardPage;
  SrcEdit: TNewEdit;

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
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID = SrcPage.ID then
    if (Trim(SrcEdit.Text) = '') or
       not (FileExists(Trim(SrcEdit.Text)) or DirExists(Trim(SrcEdit.Text))) then
    begin
      MsgBox('Select an existing disc image or folder.', mbError, MB_OK);
      Result := False;
    end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
  Msg: String;
begin
  if CurStep = ssPostInstall then
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
end;
