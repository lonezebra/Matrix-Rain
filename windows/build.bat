@echo off
rem Matrix Rain screensaver — MSVC build.
rem Run from a "Developer Command Prompt for VS" (or after vcvars64.bat).

cl /nologo /W4 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS matrix_rain.c rain.c ^
   /Fe:MatrixRain.scr ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib comctl32.lib advapi32.lib

if %errorlevel% neq 0 (
    echo Build failed.
    exit /b 1
)
del /q matrix_rain.obj rain.obj 2>nul
echo Built MatrixRain.scr
