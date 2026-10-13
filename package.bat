@echo off
setlocal enabledelayedexpansion
rem Assembles the drag-and-drop release package from the current build.
rem
rem   package.bat              into %USERPROFILE%\Desktop\TRLVRmod
rem   package.bat <folder>     somewhere else
rem
rem It refuses to write into a folder that already exists and is not one of
rem ours. A mistyped path once pointed a copy like this at an unrelated mod,
rem and a packaging script is exactly the thing that should never overwrite
rem somebody else's d3d9.dll. Ours carry trlvr-package.txt at the top.
rem
rem Nothing is deleted wholesale: files are copied over, and files that used
rem to ship and no longer do are removed by name.

set "ROOT=%~dp0"
set "DEST=%USERPROFILE%\Desktop\TRLVRmod"
if not "%~1"=="" set "DEST=%~1"
set "MAIN=!DEST!\Main - Copy these into the base game folder"
set "MARK=!DEST!\trlvr-package.txt"

if exist "!DEST!\" (
    if not exist "!MARK!" (
        dir /b "!DEST!" 2>nul | findstr "." >nul
        if not errorlevel 1 (
            echo ERROR: !DEST!
            echo   exists and is not a TRL VR package ^(no trlvr-package.txt^).
            echo   Nothing was written. Pass a different folder.
            exit /b 1
        )
    )
)

for %%F in ("build\d3d9.dll" "dist\d3d9_dxvk.dll" "dist\trlvr_launcher.exe" "release\READ ME FIRST.txt") do (
    if not exist "!ROOT!%%~F" (
        echo ERROR: !ROOT!%%~F is missing. Run build.bat first.
        exit /b 1
    )
)
if not exist "!MAIN!" mkdir "!MAIN!"

copy /y "!ROOT!build\d3d9.dll"                          "!MAIN!\d3d9.dll" >nul
copy /y "!ROOT!dist\d3d9_dxvk.dll"                      "!MAIN!\d3d9_dxvk.dll" >nul
copy /y "!ROOT!dist\trlvr_launcher.exe"                 "!MAIN!\trlvr_launcher.exe" >nul
copy /y "!ROOT!release\READ ME FIRST.txt"               "!DEST!\READ ME FIRST.txt" >nul
copy /y "!ROOT!release\VR CONTROLLER BINDINGS.txt"      "!DEST!\VR CONTROLLER BINDINGS.txt" >nul

rem Licences: the mod's (Apache 2.0) and those of the binaries shipped with it
rem (the OpenXR loader is linked into d3d9.dll).
if not exist "!DEST!\Licences" mkdir "!DEST!\Licences"
copy /y "!ROOT!LICENSE"                        "!DEST!\Licences\LICENSE.txt" >nul
copy /y "!ROOT!NOTICE"                         "!DEST!\Licences\NOTICE.txt" >nul
copy /y "!ROOT!THIRD_PARTY_NOTICES.md"         "!DEST!\Licences\THIRD_PARTY_NOTICES.txt" >nul
copy /y "!ROOT!dxvk-trlvr\LICENSE"             "!DEST!\Licences\DXVK-LICENSE.txt" >nul
copy /y "!ROOT!third_party\openxr\LICENSE"     "!DEST!\Licences\OpenXR-SDK-LICENSE.txt" >nul

rem Retired from the package. The XInput proxy stopped shipping when the
rem controllers moved to keyboard and mouse -- see src/vr/vr_input.cpp. OpenVR
rem and the SteamVR bindings went with the move to OpenXR.
if exist "!MAIN!\xinput9_1_0.dll" del /q "!MAIN!\xinput9_1_0.dll"
if exist "!MAIN!\openvr_api.dll" del /q "!MAIN!\openvr_api.dll"
if exist "!MAIN!\trlvr_actions.json" del /q "!MAIN!\trlvr_actions.json"
for %%F in (bindings_hpmotioncontroller.json bindings_holographic_controller.json bindings_oculus_touch.json bindings_knuckles.json) do (
    if exist "!MAIN!\%%F" del /q "!MAIN!\%%F"
)
if exist "!DEST!\Licences\OpenVR-LICENSE.txt" del /q "!DEST!\Licences\OpenVR-LICENSE.txt"

echo Tomb Raider: Legend VR release package, written by package.bat> "!MARK!"

echo Packaged into:
echo   !DEST!
dir /b "!MAIN!"
exit /b 0
