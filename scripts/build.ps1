param([ValidateSet('Debug','Release','RelWithDebInfo')][string]$Configuration = 'Debug')
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$generatedHeader = Join-Path $projectRoot 'native\src\recomp\gen\recomp_funcs.h'
if (-not (Test-Path -LiteralPath $generatedHeader)) { throw 'Generated code is missing. Run python scripts/prepare.py first.' }
Push-Location (Join-Path $projectRoot 'native')
try {
    cmake -S . -B build -G 'Visual Studio 17 2022' -A x64
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
    cmake --build build --config $Configuration --parallel 4
    if ($LASTEXITCODE -ne 0) { throw 'Build failed; inspect compiler output' }
} finally { Pop-Location }
