@echo off
setlocal enabledelayedexpansion
rem Installs the VR proxy into the game folder, or removes it again.
rem
rem   install.bat                 install the proxy (system d3d9 backend)
rem   install.bat dxvk            also install DXVK as the backend
rem   install.bat uninstall       remove everything this put there
rem
rem Nothing in the original install is modified. The only files added are the
rem ones listed under uninstall, so removing them puts the game back exactly as
rem it was -- no need to verify integrity through Steam.
rem
rem Note the !GAME! rather than %GAME% inside every parenthesised block: the
rem default path contains "(x86)", and at parse time that closing parenthesis
rem would end the block.

set "ROOT=%~dp0"
set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider Legend"
if not "%~2"=="" set "GAME=%~2"

if not exist "!GAME!\trl.exe" (
    echo ERROR: trl.exe not found in:
    echo   !GAME!
    echo Pass the game folder as the second argument if it lives elsewhere.
    exit /b 1
)

if /i "%~1"=="uninstall" goto :uninstall

if not exist "!ROOT!build\d3d9.dll" (
    echo ERROR: build\d3d9.dll not found. Run build.bat first.
    exit /b 1
)

rem Refuse to clobber a d3d9.dll we did not put there -- TRAWindowed installs
rem under the same name, and silently replacing it would be a confusing loss.
if exist "!GAME!\d3d9.dll" if not exist "!GAME!\trlvr.installed" (
    echo ERROR: a d3d9.dll is already in the game folder and it is not ours.
    echo Move it aside first if you want to replace it.
    exit /b 1
)

copy /y "!ROOT!build\d3d9.dll" "!GAME!\d3d9.dll" >nul
if errorlevel 1 (
    echo ERROR: could not copy d3d9.dll into the game folder.
    exit /b 1
)
echo installed by install.bat> "!GAME!\trlvr.installed"

rem The launcher patches a COPY of the exe to be large-address-aware. The
rem original is never touched, so Steam's integrity check has nothing to undo.
if exist "!ROOT!dist\trlvr_launcher.exe" (
    copy /y "!ROOT!dist\trlvr_launcher.exe" "!GAME!\trlvr_launcher.exe" >nul

rem The controller mapping is built into d3d9.dll now (OpenXR actions). The
rem SteamVR manifest and bindings earlier installs put here are not read any
rem more -- remove them.
for %%F in (trlvr_actions.json bindings_hpmotioncontroller.json bindings_holographic_controller.json bindings_oculus_touch.json bindings_knuckles.json) do (
    if exist "!GAME!\%%F" del /q "!GAME!\%%F"
)

rem The XInput proxy stopped shipping when the controllers moved to keyboard
rem and mouse. Earlier installs left one here -- remove it, but only if it is
rem ours: x360ce and friends install a file of the same name, and ours is the
rem only one carrying our action paths.
if exist "!GAME!\xinput9_1_0.dll" (
    findstr /m /c:"/actions/gameplay" "!GAME!\xinput9_1_0.dll" >nul 2>&1
    if not errorlevel 1 del /q "!GAME!\xinput9_1_0.dll"
)
) else (
    echo WARNING: trlvr_launcher.exe not built; the game stays limited to 2 GB.
)

rem The OpenXR loader is linked into d3d9.dll, so there is nothing else to
rem copy. openvr_api.dll from an earlier install is no longer used.
if exist "!GAME!\openvr_api.dll" del /q "!GAME!\openvr_api.dll"

if /i "%~1"=="dxvk" (
    rem Our fork first -- it is the one with Direct3DCreateVR9, which the VR
    rem submit path needs. The stock prebuilt is only a fallback for testing
    rem that the game runs on plain DXVK at all.
    if exist "!ROOT!dist\d3d9_dxvk.dll" (
        copy /y "!ROOT!dist\d3d9_dxvk.dll" "!GAME!\d3d9_dxvk.dll" >nul
        echo Installed the TRL VR DXVK fork as d3d9_dxvk.dll
    ) else (
        if not exist "!ROOT!..\dxvk-3.1-release\x32\d3d9.dll" (
            echo ERROR: no DXVK found. Run build_dxvk.bat, or fetch dxvk-3.1-release.
            exit /b 1
        )
        copy /y "!ROOT!..\dxvk-3.1-release\x32\d3d9.dll" "!GAME!\d3d9_dxvk.dll" >nul
        echo Installed STOCK DXVK as d3d9_dxvk.dll -- no VR submit support.
        echo Run build_dxvk.bat to get the fork.
    )
    echo backend = auto in trlvr.ini picks it up automatically.
)

echo.
echo Installed into:
echo   !GAME!
echo.
echo   d3d9.dll          the proxy ^(OpenXR built in^)
echo   trlvr_launcher.exe  starts the game with a 4 GB address space
echo.
echo Steam ^> Properties ^> Launch Options:
echo   "!GAME!\trlvr_launcher.exe" %%command%%
echo.
echo On first run the game writes trlvr.ini and trlvr.log beside trl.exe.
echo trlvr.log records startup, headset, hook and controller status.
echo Missing current settings are added to older INIs without replacing values.
echo Obsolete keys are harmless and ignored.
exit /b 0

:uninstall
del /q "!GAME!\d3d9.dll" 2>nul
del /q "!GAME!\d3d9_dxvk.dll" 2>nul
del /q "!GAME!\openvr_api.dll" 2>nul
del /q "!GAME!\trlvr_launcher.exe" 2>nul
if exist "!GAME!\xinput9_1_0.dll" (
    findstr /m /c:"/actions/gameplay" "!GAME!\xinput9_1_0.dll" >nul 2>&1
    if not errorlevel 1 del /q "!GAME!\xinput9_1_0.dll"
)
del /q "!GAME!\bindings_hpmotioncontroller.json" 2>nul
del /q "!GAME!\bindings_knuckles.json" 2>nul
del /q "!GAME!\bindings_oculus_touch.json" 2>nul
del /q "!GAME!\bindings_holographic_controller.json" 2>nul
del /q "!GAME!\trlvr_actions.json" 2>nul
del /q "!GAME!\trl_vr.exe" 2>nul
del /q "!GAME!\trlvr_launcher.log" 2>nul
del /q "!GAME!\trlvr.ini" 2>nul
del /q "!GAME!\trlvr.log" 2>nul
del /q "!GAME!\trlvr.installed" 2>nul
echo Removed the proxy from:
echo   !GAME!
exit /b 0
