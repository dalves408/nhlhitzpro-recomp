@echo off
rem NHL Hitz Pro: record a menu walk for the texture dump runs.
rem
rem Play from the intro to the start of a match, cycling through the teams (and their variants) on SELECT TEAMS
rem the way you want the dump runs to. Every key press is logged. Close the game window to end the recording.
rem The textures the game decodes while you do this are also saved, to analysis\textures-auto.
rem
rem Keys (click the game window first): Enter = Start, Z = A, X = B, C = X, V = Y, arrows = D-pad,
rem WASD = left stick, Backspace = Back.
rem
rem Afterwards the key log becomes a replay file:
rem   python scripts\keylog_to_menu.py analysis\menu_record_<stamp>.log analysis\menu_cycle.txt
rem and the dump runs use it with  -Menu analysis\menu_cycle.txt
setlocal
cd /d "%~dp0"
if not exist native\build\RelWithDebInfo\nhl_hitz_pro_recomp.exe (
    echo Build the game first: cd native, then cmake --build build --config RelWithDebInfo
    pause
    exit /b 1
)
if not exist analysis mkdir analysis
if not exist analysis\textures-auto mkdir analysis\textures-auto
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd_HHmmss"') do set STAMP=%%i
set LOG=%~dp0analysis\menu_record_%STAMP%.log

set RECOMP_D3D11=1
set RECOMP_KEYBOARD=1
set RECOMP_KEYLOG=1
set RECOMP_TEXDUMP_ALL=%~dp0analysis\textures-auto
set RECOMP_SAVE_DIR=%~dp0runtime\SaveData

echo Recording. Log: %LOG%
echo Close the game window to stop.
pushd native
"build\RelWithDebInfo\nhl_hitz_pro_recomp.exe" > "%LOG%.out" 2> "%LOG%"
popd
echo Session ended. Key presses:
findstr /c:"[KEYLOG]" "%LOG%"
echo.
echo Log: %LOG%
pause
