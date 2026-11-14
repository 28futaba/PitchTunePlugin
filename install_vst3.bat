@echo off
set "SCRIPT_DIR=%~dp0"
set "SRC=%SCRIPT_DIR%Builds\VisualStudio2026\x64\Release\VST3\PitchTunePlugin.vst3\Contents"
if not exist "%SRC%" (
    set "SRC=%SCRIPT_DIR%Builds\VisualStudio2026\x64\Debug\VST3\PitchTunePlugin.vst3\Contents"
)
set "DST=C:\Program Files\Common Files\VST3\PitchTunePlugin.vst3\Contents"
set "DST_ALT=C:\Program Files\Common Files\VST3\Self-Pl\Contents"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo Administrator privileges required. Requesting elevation...
    powershell -NoProfile -Command "Start-Process cmd.exe -ArgumentList '/c \"\"%~f0\"\"' -Verb RunAs"
    exit /b
)

echo ========================================================
echo  PitchTunePlugin VST3 Installer (Administrator)
echo ========================================================
echo.

if not exist "%DST%\x86_64-win" mkdir "%DST%\x86_64-win" 2>nul
if not exist "%DST%\Resources" mkdir "%DST%\Resources" 2>nul

copy /Y "%SRC%\x86_64-win\PitchTunePlugin.vst3" "%DST%\x86_64-win\PitchTunePlugin.vst3"
copy /Y "%SRC%\Resources\moduleinfo.json" "%DST%\Resources\moduleinfo.json"

if not exist "%DST_ALT%\x86_64-win" mkdir "%DST_ALT%\x86_64-win" 2>nul
if not exist "%DST_ALT%\Resources" mkdir "%DST_ALT%\Resources" 2>nul
copy /Y "%SRC%\x86_64-win\PitchTunePlugin.vst3" "%DST_ALT%\x86_64-win\PitchTunePlugin.vst3" 2>nul
copy /Y "%SRC%\Resources\moduleinfo.json" "%DST_ALT%\Resources\moduleinfo.json" 2>nul

if %errorlevel% equ 0 (
    echo.
    echo ========================================================
    echo  [SUCCESS] Successfully installed PitchTunePlugin.vst3!
    echo ========================================================
) else (
    echo.
    echo ========================================================
    echo  [ERROR] Copy failed. Please make sure DAW is closed!
    echo ========================================================
)

echo.
pause
