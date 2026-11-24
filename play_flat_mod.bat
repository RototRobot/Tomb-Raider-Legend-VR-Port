@echo off
rem Starts the game flat with the VR mod still loaded: its overlay (Insert),
rem body physics and other non-VR features work, but no OpenXR session is
rem started (no headset, no simulator window) and the game's own window mode
rem is used, fullscreen included.
rem
rem TRLVR_FLAT tells the mod's d3d9.dll to do this for this run only;
rem trlvr.ini is not changed, so Steam's launch is still VR. For the game
rem with the mod bypassed entirely, use play_flat.bat.
rem
rem Lives in the repo root; install.bat also copies it into the game folder.
setlocal
set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider Legend"
if exist "%~dp0trl.exe" set "GAME=%~dp0."
if not exist "%GAME%\trlvr_launcher.exe" (
    echo ERROR: trlvr_launcher.exe not found in "%GAME%". Run install.bat first.
    pause
    exit /b 1
)
set TRLVR_FLAT=1
start "" /d "%GAME%" "%GAME%\trlvr_launcher.exe" "%GAME%\trl.exe"
