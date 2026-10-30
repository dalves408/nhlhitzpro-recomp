# Runs the game many times, one scripted match each, and collects every texture the game decodes.
# A texture is written once, named <content hash>_<w>x<h>_f<format>_fl<flip>.bmp (32-bit BMP, alpha kept); a
# texture already in the folder from an earlier run is not written again. The rink follows the home team, which
# the game picks at random, so more runs find more rinks.
#
#   powershell -File scripts\dump_textures_loop.ps1 [-Runs 100] [-Seconds 120] [-Out analysis\textures-auto]
#                                                   [-StopAfterNoNew 0] [-Slot 0]
#
#   -Seconds N         length of each run (the match reaches the face-off at about 110 s; 120 leaves some margin)
#   -StopAfterNoNew N  stop early after N runs in a row that add no new texture (0 = never)
#   -Slot K            working folder analysis\slots\K (log, frames, save copy, settings), so several copies can run
#                      side by side (see dump_textures_parallel.ps1)
#
# Per run it keeps the last frame (Out\_runs\run_NNN.bmp, to see which teams/rink loaded) and appends a line to
# Out\_runs\summary.csv. Uses its own settings (windowed, 1x, no MSAA) and a copy of the save folder, not yours,
# and the sound is muted. The game window opens on screen for each run.
param([int]$Runs = 100, [int]$Seconds = 120, [string]$Out = 'analysis\textures-auto', [int]$StopAfterNoNew = 0, [int]$Slot = 0, [string]$Menu = '')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
$out = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
$runsDir = Join-Path $out '_runs'
[IO.Directory]::CreateDirectory($runsDir) | Out-Null
$wd = Join-Path $root "analysis\slots\$Slot"
[IO.Directory]::CreateDirectory("$wd\settings\NHLHitzPro") | Out-Null
Set-Content "$wd\settings\NHLHitzPro\settings.ini" "[display]`nfullscreen=0`nresolution_scale=1`nstretch=0`nvsync=0`nmsaa=0`nanisotropy=1`n"
$save = "$wd\save"
$summary = Join-Path $runsDir 'summary.csv'
if (-not (Test-Path $summary)) { Set-Content $summary 'run,slot,when,flips,new_textures,total_textures,seconds,status' }
$envs = "RECOMP_D3D11_LOG=1;RECOMP_AUDIO_MUTE=1;RECOMP_TEXDUMP_ALL=$($out -replace '\\','/');LOCALAPPDATA=$("$wd\settings" -replace '\\','/')"

function Count-Tex { (Get-ChildItem $out -Filter '*.bmp' -File | Where-Object { $_.Name -match '^[0-9A-Fa-f]{16}_' }).Count }
function Last-Flips {
  $log = "$wd\capture.log"
  if (-not (Test-Path $log)) { return 0 }
  $m = Select-String -Path $log -Pattern 'flips=(\d+)' | Select-Object -Last 1
  if ($m) { [int]$m.Matches[0].Groups[1].Value } else { 0 }
}
function Fresh-Save {   # the game writes its save folder during a run: every run starts from the same copy
  if (Test-Path $save) { [IO.Directory]::Delete($save, $true) }
  Copy-Item "$root\runtime\SaveData" $save -Recurse
}

$noNew = 0; $startN = Count-Tex
"[slot $Slot] texture dump loop: $Runs runs of $Seconds s -> $out (already $startN textures there)"
for ($i = 1; $i -le $Runs; $i++) {
  $before = Count-Tex; $sw = [Diagnostics.Stopwatch]::StartNew(); $status = 'ok'; $flips = 0
  for ($try = 1; $try -le 2; $try++) {
    Fresh-Save
    & "$root\scripts\quick_match.ps1" -tag texloop -seconds $Seconds -envs $envs -every 1500 -workdir $wd -savedir $save -menu $Menu | Out-Null
    $flips = Last-Flips
    if ($flips -ge 2500) { break }
    $status = "retry($flips flips)"
  }
  if ($flips -lt 2500) { $status = "FAILED($flips flips)" }
  $after = Count-Tex; $new = $after - $before
  $frame = Get-ChildItem "$wd\frames" -Filter 'frame_d3d_*.bmp' -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -Last 1
  if ($frame) { Copy-Item $frame.FullName (Join-Path $runsDir ('run_s{0}_{1:D3}.bmp' -f $Slot, $i)) -Force }
  Add-Content $summary ('{0},{1},{2:s},{3},{4},{5},{6},{7}' -f $i, $Slot, (Get-Date), $flips, $new, $after, [int]$sw.Elapsed.TotalSeconds, $status)
  "[slot $Slot] run $i/$Runs : $flips flips, +$new new textures ($after total) [$status]"
  if ($new -eq 0) { $noNew++ } else { $noNew = 0 }
  if ($StopAfterNoNew -gt 0 -and $noNew -ge $StopAfterNoNew) { "[slot $Slot] no new textures in $noNew runs in a row; stopping"; break }
}
"[slot $Slot] done. $(Count-Tex) textures in $out"
