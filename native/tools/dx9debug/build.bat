@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
rem No C runtime: the exe must start on Windows XP. /Gs disables stack probes (they live in the CRT); entry is our own.
cl /nologo /O1 /Oi /GS- /Gs1000000 /Zl /W3 /c dx9debug.c || exit /b 1
link /nologo /NODEFAULTLIB /ENTRY:dx9debug_start /SUBSYSTEM:CONSOLE,5.01 /LARGEADDRESSAWARE /OPT:REF /OPT:ICF dx9debug.obj kernel32.lib user32.lib gdi32.lib advapi32.lib winmm.lib ole32.lib shell32.lib version.lib d3d9.lib /OUT:dx9debug.exe || exit /b 1
