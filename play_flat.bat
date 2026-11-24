@echo off
rem Starts the original game, untouched by the VR mod: no VR, camera, controls,
rem menus or forced window -- fullscreen works from the game's own settings.
rem
rem TRLVR_VANILLA tells our d3d9.dll to forward straight to the system d3d9 and
rem do nothing else, and the original trl.exe is run rather than the launcher's
rem 4 GB copy. Only this run is affected; Steam's launch is still VR.
rem
rem Other mods in the folder (the ASI loader and anything it loads, such as
rem texture packs) still load, as they would without the VR mod installed.
rem
rem Lives in the repo root; install.bat also copies it into the game folder.
setlocal
set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider Legend"
if exist "%~dp0trl.exe" set "GAME=%~dp0."
if not exist "%GAME%\trl.exe" (
    echo ERROR: trl.exe not found in "%GAME%".
    pause
    exit /b 1
)
set TRLVR_VANILLA=1
start "" /d "%GAME%" "%GAME%\trl.exe"
