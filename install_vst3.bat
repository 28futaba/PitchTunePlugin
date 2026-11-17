@echo off
cd /d "%~dp0"
set "SCRIPT_DIR=%~dp0"
set "SRC=%SCRIPT_DIR%Builds\VisualStudio2026\x64\Release\VST3\PitchTunePlugin.vst3\Contents"
if exist "%SRC%\x86_64-win\PitchTunePlugin.vst3" goto :check_admin
set "SRC=%SCRIPT_DIR%Builds\VisualStudio2026\x64\Debug\VST3\PitchTunePlugin.vst3\Contents"

:check_admin
set "DST=C:\Program Files\Common Files\VST3\PitchTunePlugin.vst3\Contents"
set "DST_ALT=C:\Program Files\Common Files\VST3\Self-Pl\Contents"

net session >nul 2>&1
if %errorlevel% equ 0 goto :do_install

echo Administrator privileges required. Requesting elevation...
powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
exit /b

:do_install
echo ========================================================
echo  PitchTunePlugin VST3 Installer
echo ========================================================
echo.

if not exist "%DST%\x86_64-win" mkdir "%DST%\x86_64-win" 2>nul
if not exist "%DST%\Resources" mkdir "%DST%\Resources" 2>nul

copy /Y "%SRC%\x86_64-win\PitchTunePlugin.vst3" "%DST%\x86_64-win\PitchTunePlugin.vst3"
if %errorlevel% neq 0 goto :install_error
copy /Y "%SRC%\Resources\moduleinfo.json" "%DST%\Resources\moduleinfo.json"

if not exist "%DST_ALT%\x86_64-win" mkdir "%DST_ALT%\x86_64-win" 2>nul
if not exist "%DST_ALT%\Resources" mkdir "%DST_ALT%\Resources" 2>nul
copy /Y "%SRC%\x86_64-win\PitchTunePlugin.vst3" "%DST_ALT%\x86_64-win\PitchTunePlugin.vst3" >nul 2>&1
copy /Y "%SRC%\Resources\moduleinfo.json" "%DST_ALT%\Resources\moduleinfo.json" >nul 2>&1

echo.
echo ========================================================
echo  [SUCCESS] PitchTunePlugin.vst3 installed successfully!
echo  Location: C:\Program Files\Common Files\VST3\PitchTunePlugin.vst3
echo ========================================================
goto :finish

:install_error
echo.
echo ========================================================
echo  [ERROR] Copy failed. Please close your DAW and try again.
echo ========================================================

:finish
echo.
pause
