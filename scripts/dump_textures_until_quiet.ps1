# Texture dump with an automatic stop: up to MaxRuns runs (default 100), run Workers at a time (default 3), but
# stop as soon as QuietRuns runs in a row added no new texture. Hands free from start to end.
#
#   powershell -File scripts\dump_textures_until_quiet.ps1 [-MaxRuns 100] [-QuietRuns 5] [-Workers 3] [-Seconds 120]
#                                                          [-Menu analysis\menu_cycle.txt] [-Out analysis\textures-auto]
#
# The runs go in groups of Workers (the games run side by side). After each group it waits until the folder has
# stopped changing (no file written for -SettleSeconds), then counts the new textures; a group with none adds all
# its runs to the quiet streak, so with 3 at a time the stop comes after 2 quiet groups (6 runs in a row).
# Only the textures of the formats in -Formats count as new (the compressed formats the rink, logos and jerseys
# use): the game also writes textures that change on every run (intro movie frames, clock digits), which would
# otherwise keep the streak from ever starting.
param([int]$MaxRuns = 100, [int]$QuietRuns = 5, [int]$Workers = 3, [int]$Seconds = 120, [string]$Menu = '',
      [string]$Out = 'analysis\textures-auto', [string]$Formats = 'f0C|f0E|f0F', [int]$SettleSeconds = 10)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
$out = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
$menuArg = @{}; if ($Menu) { $menuArg['Menu'] = $Menu }
function Count-Kept { (Get-ChildItem $out -Filter '*.bmp' -File | Where-Object { $_.Name -match "^[0-9A-Fa-f]{16}_\d+x\d+_($Formats)_" }).Count }
function Wait-Settled {   # no texture file written for $SettleSeconds (or give up after 3 minutes)
  $limit = (Get-Date).AddMinutes(3)
  while ((Get-Date) -lt $limit) {
    $last = Get-ChildItem $out -Filter '*.bmp' -File | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $last -or ((Get-Date) - $last.LastWriteTime).TotalSeconds -ge $SettleSeconds) { return }
    Start-Sleep -Seconds 2
  }
}
while (Get-Process nhl_hitz_pro_recomp -ErrorAction SilentlyContinue) { "another copy of the game is running; waiting"; Start-Sleep -Seconds 20 }

Wait-Settled
$count = Count-Kept; $done = 0; $quiet = 0; $group = 0
"Texture dump: up to $MaxRuns runs, $Workers at a time, $Seconds s each; stop after $QuietRuns quiet runs in a row. Start: $count textures of the counted formats."
while ($done -lt $MaxRuns -and $quiet -lt $QuietRuns) {
  $n = [Math]::Min($Workers, $MaxRuns - $done); $group++
  $linesBefore = if (Test-Path "$out\_runs\summary.csv") { (Get-Content "$out\_runs\summary.csv").Count } else { 0 }
  & "$root\scripts\dump_textures_parallel.ps1" -Workers $n -Runs $n -Seconds $Seconds -Out $out @menuArg | Out-Null
  $linesAfter = if (Test-Path "$out\_runs\summary.csv") { (Get-Content "$out\_runs\summary.csv").Count } else { 0 }
  if ($linesAfter - $linesBefore -lt $n) { "ERROR: group $group did not run its $n games (only $($linesAfter - $linesBefore) finished); stopping instead of counting it."; break }
  Wait-Settled
  $after = Count-Kept; $new = $after - $count; $count = $after; $done += $n
  if ($new -le 0) { $quiet += $n } else { $quiet = 0 }
  "{0:s} group {1}: runs {2}/{3}, +{4} new textures, {5} total, quiet runs in a row {6}/{7}" -f (Get-Date), $group, $done, $MaxRuns, $new, $count, $quiet, $QuietRuns
}
if ($quiet -ge $QuietRuns) { "STOPPED EARLY: $quiet runs in a row added nothing new, after $done runs. $count textures." }
else { "Reached the limit of $MaxRuns runs. $count textures." }
