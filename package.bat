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

for %%F in ("build\d3d9.dll" "dist\d3d9_dxvk.dll" "dist\trlvr_launcher.exe" "release\READ ME FIRST.txt" "input\actions.json") do (
    if not exist "!ROOT!%%~F" (
        echo ERROR: !ROOT!%%~F is missing. Run build.bat first.
        exit /b 1
    )
)
if not exist "!ROOT!third_party\openvr\bin\win32\openvr_api.dll" (
    echo ERROR: openvr_api.dll ^(win32^) not found in third_party\openvr.
    exit /b 1
)

if not exist "!MAIN!" mkdir "!MAIN!"

copy /y "!ROOT!build\d3d9.dll"                          "!MAIN!\d3d9.dll" >nul
copy /y "!ROOT!dist\d3d9_dxvk.dll"                      "!MAIN!\d3d9_dxvk.dll" >nul
copy /y "!ROOT!dist\trlvr_launcher.exe"                 "!MAIN!\trlvr_launcher.exe" >nul
copy /y "!ROOT!third_party\openvr\bin\win32\openvr_api.dll" "!MAIN!\openvr_api.dll" >nul
copy /y "!ROOT!input\actions.json"                      "!MAIN!\trlvr_actions.json" >nul
for %%F in ("!ROOT!input\bindings_*.json") do copy /y "%%~F" "!MAIN!\%%~nxF" >nul
copy /y "!ROOT!release\READ ME FIRST.txt"               "!DEST!\READ ME FIRST.txt" >nul
copy /y "!ROOT!release\VR CONTROLLER BINDINGS.txt"      "!DEST!\VR CONTROLLER BINDINGS.txt" >nul

rem Retired from the package. The XInput proxy stopped shipping when the
rem controllers moved to keyboard and mouse -- see src/vr/vr_input.cpp.
if exist "!MAIN!\xinput9_1_0.dll" del /q "!MAIN!\xinput9_1_0.dll"

echo Tomb Raider: Legend VR release package, written by package.bat> "!MARK!"

echo Packaged into:
echo   !DEST!
dir /b "!MAIN!"
exit /b 0
