# Keeps dumping textures, in rounds of N games side by side, until QuietRounds rounds in a row add no new texture
# of the kinds that matter. Waits first for any dump_textures_parallel.ps1 already running (e.g. the first 100 runs).
#
#   powershell -File scripts\dump_textures_converge.ps1 [-Out analysis\textures-auto] [-Workers 3] [-Seconds 120]
#                                                       [-QuietRounds 2] [-MaxRounds 60] [-Formats 'f0C|f0E|f0F']
#
# "New" counts only textures whose format code is in -Formats (the compressed formats the rink art uses). The
# game also writes textures that change every run (intro-movie frames, jersey names and numbers, the clock), which
# would never let the count reach zero. -MaxRounds is a hard stop in case it never settles.
param([string]$Out = 'analysis\textures-auto', [int]$Workers = 3, [int]$Seconds = 120, [int]$QuietRounds = 2,
      [int]$MaxRounds = 60, [string]$Formats = 'f0C|f0E|f0F', [string]$Menu = '')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
$out = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
function Count-Kept { (Get-ChildItem $out -Filter '*.bmp' -File | Where-Object { $_.Name -match "^[0-9A-Fa-f]{16}_\d+x\d+_($Formats)_" }).Count }
function Dump-Running {
  Get-CimInstance Win32_Process -Filter "Name='powershell.exe' or Name='pwsh.exe'" |
    Where-Object { $_.CommandLine -match 'dump_textures_(parallel|loop)\.ps1' -and $_.ProcessId -ne $PID }
}
while (Dump-Running) { "waiting for the running dump to finish..."; Start-Sleep -Seconds 60 }

$quiet = 0
for ($r = 1; $r -le $MaxRounds -and $quiet -lt $QuietRounds; $r++) {
  $before = Count-Kept
  & "$root\scripts\dump_textures_parallel.ps1" -Workers $Workers -Runs $Workers -Seconds $Seconds -Out $out -Menu $Menu | Out-Null
  $after = Count-Kept; $new = $after - $before
  if ($new -le 0) { $quiet++ } else { $quiet = 0 }
  "round $r : +$new new ($Formats) textures, $after total, quiet rounds in a row: $quiet/$QuietRounds"
}
if ($quiet -ge $QuietRounds) { "Settled: $QuietRounds rounds in a row with no new textures. $(Count-Kept) textures kept." }
else { "Stopped at the round limit ($MaxRounds). $(Count-Kept) textures kept." }
