{ Shared by game Setup and the extraction-only standalone texture addon.
  The caller supplies pinned Texture* constants from texture_payload.iss. }
var
  TextureProgress: TOutputProgressWizardPage;
  TextureTransferActive, TextureTransferCancelled, TextureExtractActive: Boolean;
  TexturePackInstalled: Boolean;
  TexturePackAlreadyCurrent: Boolean;
  TextureUsedCache, TextureCacheChoiceAsked, TextureDeleteCacheOnExit: Boolean;
  TexturePartPaths, TexturePartCaches: array[1..2] of String;
  TexturePart: Integer;
  TextureLastUiTick: LongWord;
  TexturePayloadPath, TextureCacheDir, TextureCancelFile, TextureError: String;
  TextureRatePhase: String;
  TextureRateTick: LongWord;
  TextureRateBytes, TextureRatePerSecond: Int64;
  TextureRateAvailable: Boolean;

function TextureSetupProcessId: LongWord;
  external 'GetCurrentProcessId@kernel32.dll stdcall';
function TextureTick: LongWord;
  external 'GetTickCount@kernel32.dll stdcall';

function GetFileAttributes(FileName: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function TextureForceRepair: Boolean;
begin
  Result := ExpandConstant('{param:REPAIRTEXTURES|0}') = '1';
end;

function TextureInstalledPackCurrent(Dir: String): Boolean;
var
  Manifest, Rest, Relative: String;
  Lines: TArrayOfString;
  I, Space: Integer;
  ExpectedSize, ActualSize: Int64;
begin
  Result := False;
  if Dir = '' then Exit;
  Dir := AddBackslash(Dir) + 'texture_pack\';
  Manifest := Dir + 'manifest.txt';
  if not FileExists(Manifest) then Exit;
  try
    { Pin the expected file list rather than trusting a folder or editable
      version label. Stat every listed file to reject missing/truncated files.
      Explicit repair restores all files from the verified archive. }
    if CompareText(GetSHA256OfFile(Manifest), '{#TextureManifestSHA256}') <> 0 then Exit;
    if not LoadStringsFromFile(Manifest, Lines) then Exit;
    if GetArrayLength(Lines) <> {#TextureFileCount} then Exit;
    for I := 0 to GetArrayLength(Lines) - 1 do begin
      Rest := Lines[I];
      Space := Pos(' ', Rest);
      if Space <> 65 then Exit;
      Delete(Rest, 1, Space);
      Space := Pos(' ', Rest);
      if Space <= 1 then Exit;
      ExpectedSize := StrToInt64Def(Copy(Rest, 1, Space - 1), -1);
      Relative := Copy(Rest, Space + 1, Length(Rest));
      StringChangeEx(Relative, '/', '\', True);
      if (Relative = '') or (ExpectedSize < 0) then Exit;
      if not FileSize64(Dir + Relative, ActualSize) then Exit;
      if ActualSize <> ExpectedSize then Exit;
    end;
    Result := True;
  except
    Result := False; { Unreadable metadata/files never suppress an install. }
  end;
end;

function TexturePackNeedsInstall: Boolean;
begin
  Result := TextureForceRepair or not TextureInstalledPackCurrent(WizardDirValue);
  if not Result then Log('The current texture pack is installed; the texture download offer is hidden.');
end;

function WantTexturePack: Boolean;
begin
#ifdef TextureOptional
  Result := WizardIsTaskSelected('hires') or
    (CompareText(ExpandConstant('{param:HIRES|no}'), 'yes') = 0);
#else
  Result := True;
#endif
end;

function TexturePartName: String;
begin
  if TexturePart = 1 then Result := 'texture-01.bin'
  else Result := 'texture-02.bin';
end;

function TextureLocalPart: String;
begin
  Result := AddBackslash(ExpandConstant('{src}')) + TexturePartName;
end;

procedure TextureCancelButtonClick(var Cancel, Confirm: Boolean);
begin
  if TextureTransferActive then begin
    TextureTransferCancelled := True;
    SaveStringToFile(TextureCancelFile, 'cancel', False);
    if TextureExtractActive then
      TextureProgress.SetText('Stopping extraction...', 'The verified download is kept for next time.')
    else
      TextureProgress.SetText('Stopping download...', 'Saved progress will be resumed next time.');
    Cancel := False;
    Confirm := False;
  end;
end;

function TextureNetworkPhase(Phase: String): Boolean;
begin
  Result := (Phase = 'DOWNLOADING') or (Phase = 'RESUMING') or (Phase = 'RESTARTING');
end;

function TextureFormatBytes(Bytes: Int64): String;
begin
  if Bytes >= 1000000000 then Result := Format('%.2f GB', [Bytes / 1000000000.0])
  else if Bytes >= 1000000 then Result := Format('%.1f MB', [Bytes / 1000000.0])
  else if Bytes >= 1000 then Result := Format('%.1f KB', [Bytes / 1000.0])
  else Result := IntToStr(Bytes) + ' B';
end;

procedure TextureUpdateDownloadRate(Phase: String; Done: Int64; Tick: LongWord);
begin
  { Begin at the first reported offset: saved/resumed bytes are not new network
    traffic. Verification, retries restarting at zero and clock wrap reset the
    sample. Refresh speed about once a second; idle intervals show zero. }
  if not TextureNetworkPhase(Phase) or (Phase <> TextureRatePhase) or
     (Tick < TextureRateTick) or (Done < TextureRateBytes) then begin
    TextureRatePhase := Phase;
    TextureRateTick := Tick;
    TextureRateBytes := Done;
    TextureRatePerSecond := 0;
    TextureRateAvailable := False;
  end else if Tick - TextureRateTick >= 1000 then begin
    TextureRatePerSecond := (Done - TextureRateBytes) * 1000 div (Tick - TextureRateTick);
    TextureRateAvailable := True;
    TextureRateTick := Tick;
    TextureRateBytes := Done;
  end;
end;

function TextureProgressDetails(Phase: String; Done, Total: Int64): String;
var Percent: Extended; Speed: String;
begin
  if Done < 0 then Done := 0;
  if Done > Total then Done := Total;
  Percent := 0;
  if Total > 0 then Percent := Done * 100.0 / Total;
  if TextureNetworkPhase(Phase) then begin
    if TextureRateAvailable then Speed := TextureFormatBytes(TextureRatePerSecond) + '/s'
    else Speed := 'Measuring speed...';
    Result := Format('%.1f%% - %.1f / %.1f MB - %s', [Percent, Done / 1000000.0, Total / 1000000.0, Speed]);
  end else if Phase = 'VERIFYING' then
    Result := Format('%s - %s - %.1f%% verified', [TexturePartName, TextureFormatBytes(Total), Percent])
  else Result := TexturePartName + ' - Size: ' + TextureFormatBytes(Total);
end;

procedure TextureDownloadLog(const S: String; const Error, FirstLine: Boolean);
var
  Rest, Phase, LabelText: String;
  K: Integer;
  Done, Total: Int64;
begin
  if Error then begin TextureError := S; Log(S); Exit; end;
  if Copy(S, 1, 9) = 'PROGRESS ' then begin
    Rest := Copy(S, 10, Length(S));
    K := Pos(' ', Rest);
    Done := StrToInt64Def(Copy(Rest, 1, K - 1), 0);
    Delete(Rest, 1, K);
    K := Pos(' ', Rest);
    Total := StrToInt64Def(Copy(Rest, 1, K - 1), 0);
    Phase := Copy(Rest, K + 1, Length(Rest));
    if TextureTransferCancelled then LabelText := 'Stopping download...'
    else if Phase = 'WAITING' then LabelText := 'Waiting for another texture download to finish...'
    else if Phase = 'RESUMING' then LabelText := 'Resuming texture pack download...'
    else if Phase = 'VERIFYING' then LabelText := 'Verifying texture pack...'
    else if Phase = 'RESTARTING' then LabelText := 'The server cannot resume this copy. Restarting download...'
    else if Phase = 'CHECKING' then LabelText := 'Checking for a local texture pack...'
    else LabelText := 'Downloading texture pack...';
    TextureUpdateDownloadRate(Phase, Done, TextureTick);
    if TextureNetworkPhase(Phase) and not TextureTransferCancelled then
      LabelText := LabelText + ' (' + TextureFormatBytes(Total) + ')';
    TextureProgress.SetText('Part ' + IntToStr(TexturePart) + ' of 2: ' + LabelText, TextureProgressDetails(Phase, Done, Total));
    TextureProgress.SetProgress(Done div 1024, Total div 1024);
  end else begin
    Log(S);
    if S = 'READY local' then begin
      TexturePayloadPath := TextureLocalPart;
      TextureUsedCache := False;
    end else if S = 'READY cache' then begin
      TexturePayloadPath := TextureCacheDir + '\texture.bin';
      TextureUsedCache := True;
    end
    else if Copy(S, 1, 6) = 'ERROR ' then TextureError := Copy(S, 7, Length(S));
  end;
end;

function PrepareTexturePart(Url, SHA: String; Bytes: Int64): String;
var
  Params, LocalParam: String;
  Code: Integer;
  Started, RetryWithoutLocal: Boolean;
begin
  Result := '';
  TextureCacheDir := ExpandConstant('{#TextureCacheRoot}') + '\' + SHA;
  TextureCancelFile := ExpandConstant('{tmp}\texture-download.cancel');
  if TextureProgress = nil then
    TextureProgress := CreateOutputProgressPage('Hi-res texture pack', 'Preparing the 4x texture pack.');
  TextureProgress.SetText('Checking for a local texture pack...', TexturePartName);
  TextureProgress.SetProgress(0, 0);
  if not FileExists(TextureLocalPart) and
     not FileExists(TextureCacheDir + '\texture.bin') then begin
    if FileExists(TextureCacheDir + '\texture.bin.part') then
      TextureProgress.SetText('Resuming texture pack download...', 'Saved progress will be used.')
    else
      TextureProgress.SetText('The texture pack will be downloaded.', 'You can stop and resume it later.');
  end;
  TextureProgress.Show;
  TextureTransferActive := True;
  TextureTransferCancelled := False;
  WizardForm.CancelButton.Enabled := True;
  WizardForm.CancelButton.Visible := True;
  LocalParam := ' --local ' + AddQuotes(TextureLocalPart);
  try
    repeat
      DeleteFile(TextureCancelFile);
      TexturePayloadPath := '';
      TextureUsedCache := False;
      TextureError := '';
      TextureRatePhase := '';
      TextureRateAvailable := False;
      Params := '--url ' + AddQuotes(Url) +
                ' --cache ' + AddQuotes(TextureCacheDir) +
                ' --sha256 ' + SHA + ' --bytes ' + IntToStr(Bytes) +
                ' --cancel ' + AddQuotes(TextureCancelFile) +
                ' --parent ' + IntToStr(TextureSetupProcessId) + LocalParam;
      Code := 99;
      Started := ExecAndLogOutput(ExpandConstant('{tmp}\nhl_texture_download.exe'), Params, '',
                                 SW_SHOWNORMAL, ewWaitUntilTerminated, Code, @TextureDownloadLog);
      RetryWithoutLocal := False;
      if Started and (Code = 5) and not TextureTransferCancelled then begin
        if WizardSilent then RetryWithoutLocal := True
        else RetryWithoutLocal := MsgBox('The ' + TexturePartName + ' beside Setup does not match this texture pack.' + #13#10#13#10 +
          'Download the correct copy instead? Your local file will be kept.', mbConfirmation, MB_YESNO) = IDYES;
        if RetryWithoutLocal then LocalParam := '';
      end;
    until not RetryWithoutLocal;
    if TextureTransferCancelled or (Code = 2) then
      Result := 'Download stopped. Saved progress is kept. Retry to resume, or Cancel to close Setup.'
    else if not Started or (Code <> 0) or (TexturePayloadPath = '') then begin
      if TextureError = '' then TextureError := 'The texture pack could not be prepared. Retry to continue the saved download.';
      Result := TextureError;
    end else begin
      TexturePartPaths[TexturePart] := TexturePayloadPath;
      if TextureUsedCache then TexturePartCaches[TexturePart] := TexturePayloadPath;
    end;
  finally
    TextureTransferActive := False;
    TextureProgress.Hide;
  end;
end;

function PrepareTexturePayload(OtherRequiredBytes: Int64): String;
var FreeBytes, TotalBytes: Int64;
begin
  Result := '';
  if not WantTexturePack then Exit;
  TexturePackAlreadyCurrent := not TextureForceRepair and TextureInstalledPackCurrent(WizardDirValue);
  TexturePackInstalled := TexturePackAlreadyCurrent;
  if TexturePackAlreadyCurrent then begin
    Log('The current texture pack is already installed; no download or extraction is needed.');
    Exit;
  end;
  if not GetSpaceOnDisk64(AddBackslash(ExtractFileDrive(WizardDirValue)), FreeBytes, TotalBytes) then begin
    Result := 'Could not check free space on the selected game drive.';
    Exit;
  end;
  if FreeBytes < ({#TextureExtractedBytes} + OtherRequiredBytes) then begin
    Result := 'There is not enough free space to stage the textures and install the selected game files.';
    Exit;
  end;
  MapArchiveExtensions('.bin', '.7z');
  if not FileExists(ExpandConstant('{tmp}\nhl_texture_download.exe')) then
    ExtractTemporaryFile('nhl_texture_download.exe');
  TexturePartCaches[1] := '';
  TexturePartCaches[2] := '';
  TexturePart := 1;
  Result := PrepareTexturePart('{#TextureURL1}', '{#TextureSHA2561}', {#TextureBytes1});
  if Result <> '' then Exit;
  TexturePart := 2;
  Result := PrepareTexturePart('{#TextureURL2}', '{#TextureSHA2562}', {#TextureBytes2});
end;

procedure TextureFinishCacheChoice;
var Choice: String;
begin
  { ssDone is reached only after a successful install. Never offer to discard
    partial downloads or a verified BIN needed after failed extraction. }
  if not TexturePackInstalled or TexturePackAlreadyCurrent or TextureCacheChoiceAsked or
     ((TexturePartCaches[1] = '') and (TexturePartCaches[2] = '')) then Exit;
  TextureCacheChoiceAsked := True;
  Choice := ExpandConstant('{param:KEEPTEXTUREBIN|}');
  if Choice = '0' then TextureDeleteCacheOnExit := True
  else if (Choice = '1') or WizardSilent then TextureDeleteCacheOnExit := False
  else
    TextureDeleteCacheOnExit := MsgBox(
      'Keep the downloaded texture archives for future repairs or reinstalls?' + #13#10#13#10 +
      'Yes keeps the cached downloads. No deletes them when Setup closes.',
      mbConfirmation, MB_YESNO) = IDNO;
  if TextureDeleteCacheOnExit then Log('The user chose to delete the cached texture archives on Setup exit.')
  else Log('The cached texture archives is kept for future repairs or reinstalls.');
end;

procedure TextureCleanupOnExit;
var I: Integer;
begin
  if not TextureDeleteCacheOnExit or not TexturePackInstalled then Exit;
  for I := 1 to 2 do
    if (TexturePartCaches[I] <> '') and FileExists(TexturePartCaches[I]) then
      if not DeleteFile(TexturePartCaches[I]) then Log('A cached texture archive may still be in use.');
end;

function TextureExtractionProgress(const ArchiveName, FileName: String; const Progress, ProgressMax: Int64): Boolean;
var Tick: LongWord;
begin
  Tick := TextureTick;
  if (Tick - TextureLastUiTick >= 100) or (Progress = ProgressMax) then begin
    TextureLastUiTick := Tick;
    TextureProgress.SetText('Extracting hi-res textures...', ExtractFileName(FileName));
    TextureProgress.SetProgress(Progress div 1024, ProgressMax div 1024);
  end;
  Result := not TextureTransferCancelled;
end;

const
  LegacyPackKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{B3A5C2D4-7E91-4F0A-8C36-2D5E9A41F7B8}_is1';

function LegacyPackRegistered(Root: Integer; AppDir: String): Boolean;
var Dir, Version: String;
begin
  Result := RegQueryStringValue(Root, LegacyPackKey, 'Inno Setup: App Path', Dir) and
    (CompareText(RemoveBackslashUnlessRoot(Dir), AppDir) = 0) and
    RegQueryStringValue(Root, LegacyPackKey, 'DisplayVersion', Version) and (Version = '1');
end;

procedure RemoveLegacyPackEntry(Root: Integer; AppDir: String);
var Command, LegacyExe: String;
begin
  if not LegacyPackRegistered(Root, AppDir) then Exit;
  RegQueryStringValue(Root, LegacyPackKey, 'UninstallString', Command);
  if not RegDeleteKeyIncludingSubkeys(Root, LegacyPackKey) then begin
    Log('Could not remove the old texture add-on registration.');
    Exit;
  end;
  LegacyExe := AppDir + '\addons\hires_textures\unins000.exe';
  if (CompareText(Command, LegacyExe) = 0) or (CompareText(Command, '"' + LegacyExe + '"') = 0) then begin
    DeleteFile(LegacyExe);
    DeleteFile(ChangeFileExt(LegacyExe, '.dat'));
    DeleteFile(ChangeFileExt(LegacyExe, '.msg'));
  end;
  Log('Removed the revision 1 texture add-on registration.');
end;

function TextureKnownPack(AppDir, PackDir: String): Boolean;
var Hash: String;
begin
  Result := LegacyPackRegistered(HKCU, AppDir) or LegacyPackRegistered(HKLM, AppDir);
  if Result or not FileExists(PackDir + '\manifest.txt') then Exit;
  try
    Hash := GetSHA256OfFile(PackDir + '\manifest.txt');
    Result := (CompareText(Hash, '{#TextureManifestSHA256}') = 0) or
      (CompareText(Hash, '11959780e965f8ac623eea6b7ba7b96c25d4fed1456b72c9a106ca4248f3e403') = 0);
  except
    Result := False;
  end;
end;

procedure InstallTexturePack;
var
  AppDir, PackDir, Stage, Backup: String;
  I: Integer;
  OldMoved, KnownPack: Boolean;
begin
  if not WantTexturePack or TexturePackAlreadyCurrent then Exit;
  if (TexturePartPaths[1] = '') or (TexturePartPaths[2] = '') then
    RaiseException('Both texture archives must be verified before extraction.');
  AppDir := RemoveBackslashUnlessRoot(ExpandConstant('{app}'));
  PackDir := AddBackslash(AppDir) + 'texture_pack';
  Stage := AddBackslash(AppDir) + '.texture-stage-' + IntToStr(TextureSetupProcessId);
  Backup := PackDir + '.previous-' + IntToStr(TextureSetupProcessId);
  if DirExists(Stage) or FileExists(Stage) or DirExists(Backup) or FileExists(Backup) then
    RaiseException('A texture staging or backup folder already exists.');
  if not CreateDir(Stage) then RaiseException('Cannot create the texture staging folder.');
  OldMoved := False;
  TextureTransferCancelled := False;
  TextureExtractActive := True;
  TextureTransferActive := True;
  TextureLastUiTick := 0;
  TextureProgress.SetText('Extracting hi-res textures...', 'Preparing both archives');
  TextureProgress.SetProgress(0, 0);
  TextureProgress.Show;
  WizardForm.CancelButton.Enabled := True;
  WizardForm.CancelButton.Visible := True;
  try
    try
      for I := 1 to 2 do
        ExtractArchive(TexturePartPaths[I], Stage, '', True, @TextureExtractionProgress);
      if TextureTransferCancelled or not TextureInstalledPackCurrent(Stage) then
        RaiseException('The staged texture pack is incomplete.');
      KnownPack := TextureKnownPack(AppDir, PackDir);
      if DirExists(PackDir) then begin
        if not RenameFile(PackDir, Backup) then RaiseException('Cannot move the old texture folder.');
        OldMoved := True;
      end;
      if not RenameFile(Stage + '\texture_pack', PackDir) then begin
        if OldMoved and not RenameFile(Backup, PackDir) then
          RaiseException('Cannot restore the previous texture folder. It is saved at ' + Backup);
        RaiseException('Cannot move the verified texture pack into the game folder.');
      end;
      TexturePackInstalled := True;
      if OldMoved then begin
        if KnownPack and ((GetFileAttributes(Backup) and $400) = 0) then begin
          if not DelTree(Backup, True, True, True) then Log('Old texture backup remains at ' + Backup);
        end else Log('Previous texture folder preserved at ' + Backup);
      end;
      RemoveLegacyPackEntry(HKCU, AppDir);
      RemoveLegacyPackEntry(HKLM, AppDir);
      Log('Revision 2 texture pack extraction completed.');
    except
      if TextureTransferCancelled then
        RaiseException('Texture extraction stopped. The verified downloads are kept for the next run.')
      else RaiseException('Texture installation could not finish. ' + GetExceptionMessage);
    end;
  finally
    DelTree(Stage, True, True, True);
    TextureTransferActive := False;
    TextureExtractActive := False;
    TextureProgress.Hide;
  end;
end;
