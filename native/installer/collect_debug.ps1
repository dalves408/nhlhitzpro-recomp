# Packs the debug build's files into one zip on the Desktop: logs, crash/hang dumps, settings.
# Started from the Start menu shortcut "NHL Hitz Pro - Collect debug files".
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
$quiet = [bool]$env:NHL_COLLECT_QUIET   # developer use: no dialogs
function Say($text, $icon) { if ($quiet) { Write-Output $text } else { [System.Windows.Forms.MessageBox]::Show($text, 'NHL Hitz Pro debug', 'OK', $icon) | Out-Null } }
$base = Join-Path $env:LOCALAPPDATA 'NHLHitzPro'
$dbg = Join-Path $base 'debug'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$stage = Join-Path $env:TEMP "nhl_debug_$stamp"
$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "NHLHitzPro-debug-$stamp.zip"
try {
    if (-not (Test-Path $dbg)) {
        Say ("No debug files found yet in:`n$dbg`n`nPlay the game first.") 'Information'
        exit
    }
    New-Item -ItemType Directory -Force $stage | Out-Null
    # the files may still be open (a running game): copy with sharing
    foreach ($f in Get-ChildItem $dbg -File) {
        $src = [IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite,Delete')
        try { $dst = [IO.File]::Create((Join-Path $stage $f.Name)); try { $src.CopyTo($dst) } finally { $dst.Dispose() } } finally { $src.Dispose() }
    }
    foreach ($n in 'settings.ini', 'keymap.ini') {
        $p = Join-Path $base $n
        if (Test-Path $p) { Copy-Item $p (Join-Path $stage $n) }
    }
    $info = Join-Path $stage 'computer.txt'
    $os = Get-CimInstance Win32_OperatingSystem
    $gpu = Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) driver $($_.DriverVersion)" }
    @("Windows: $($os.Caption) $($os.Version)", "GPU: " + ($gpu -join '; '), "Collected: $stamp") | Set-Content $info
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
    $mb = [math]::Round((Get-Item $zip).Length / 1MB, 1)
    Say ("Saved $mb MB to your Desktop:`n$zip`n`nSend that file.") 'Information'
    if (-not $quiet) { Start-Process explorer.exe "/select,`"$zip`"" }
} catch {
    Say ("Could not collect the files:`n$($_.Exception.Message)`n`nYou can zip this folder by hand:`n$dbg") 'Error'
} finally {
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue }
}
