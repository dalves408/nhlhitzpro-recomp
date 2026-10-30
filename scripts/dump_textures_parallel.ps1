# Runs several copies of dump_textures_loop.ps1 side by side (each in its own slot: own window, settings, save copy,
# log) so a long dump finishes about N times sooner. All copies write into the same folder; at the end any texture
# that two copies wrote at the same moment is reduced to one file (the extra goes to Out\_dups).
#
#   powershell -File scripts\dump_textures_parallel.ps1 [-Workers 3] [-Runs 100] [-Seconds 120]
#                                                       [-Out analysis\textures-auto] [-StopAfterNoNew 0]
#
#   -Runs is the total across all copies (each does Runs / Workers). The game windows open on screen at once;
#   3 copies of the game use roughly 3x the CPU and GPU, so leave the PC alone while it runs.
param([int]$Workers = 3, [int]$Runs = 100, [int]$Seconds = 120, [string]$Out = 'analysis\textures-auto', [int]$StopAfterNoNew = 0, [string]$Menu = '')
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Set-Location $root
$out = if ([IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $root $Out }
[IO.Directory]::CreateDirectory("$out\_runs") | Out-Null
while (Get-Process nhl_hitz_pro_recomp -ErrorAction SilentlyContinue) { "another copy of the game is running; waiting"; Start-Sleep -Seconds 20 }
$per = [int][math]::Ceiling($Runs / $Workers)
"Starting $Workers copies, $per runs each of $Seconds s -> $out"
$procs = @()
for ($k = 1; $k -le $Workers; $k++) {
  $a = @('-NoProfile', '-File', "$root\scripts\dump_textures_loop.ps1", '-Runs', $per, '-Seconds', $Seconds, '-Out', $out, '-Slot', $k)
  if ($StopAfterNoNew -gt 0) { $a += @('-StopAfterNoNew', $StopAfterNoNew) }
  if ($Menu) { $a += @('-Menu', $Menu) }
  $procs += Start-Process powershell -ArgumentList $a -PassThru -WindowStyle Minimized -RedirectStandardOutput "$out\_runs\worker_$k.log" -RedirectStandardError "$out\_runs\worker_$k.err"
  Start-Sleep -Seconds 25          # staggered, so the copies are not all loading shaders at the same moment
}
$procs | ForEach-Object { $_.WaitForExit() }

# one file per texture
$seen = @{}; $dups = 0
Get-ChildItem $out -Filter '*.bmp' -File | Where-Object { $_.Name -match '^[0-9A-Fa-f]{16}_' } | Sort-Object Name | ForEach-Object {
  $h = $_.Name.Substring(0, 16).ToUpper()
  if ($seen.ContainsKey($h)) {
    [IO.Directory]::CreateDirectory("$out\_dups") | Out-Null
    Move-Item $_.FullName "$out\_dups\$($_.Name)" -Force; $dups++
  } else { $seen[$h] = 1 }
}
"Done. $($seen.Count) distinct textures in $out ($dups duplicates moved to _dups)"
Get-Content "$out\_runs\summary.csv" | Select-Object -Last 6
