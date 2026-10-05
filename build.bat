@echo off
setlocal
rem Builds the Tomb Raider: Legend VR d3d9 proxy.
rem
rem The game is 32-bit, so this must be a 32-bit DLL. Static CRT (/MT) keeps it
rem to a single file with no redistributable to install.

set "ROOT=%~dp0"
set "OUT=%ROOT%build"
set "BIN=%ROOT%dist"

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%BIN%" mkdir "%BIN%"

call "%ROOT%msvc.bat"
if errorlevel 1 exit /b 1

set SRC=
set SRC=%SRC% "%ROOT%src\proxy\dllmain.cpp"
set SRC=%SRC% "%ROOT%src\proxy\backend.cpp"
set SRC=%SRC% "%ROOT%src\proxy\proxy_d3d9.cpp"
set SRC=%SRC% "%ROOT%src\proxy\proxy_device.cpp"
set SRC=%SRC% "%ROOT%src\proxy\proxy_swapchain.cpp"
set SRC=%SRC% "%ROOT%src\proxy\window.cpp"
set SRC=%SRC% "%ROOT%src\proxy\gpu_profile.cpp"
set SRC=%SRC% "%ROOT%src\common\log.cpp"
set SRC=%SRC% "%ROOT%src\common\config.cpp"
set SRC=%SRC% "%ROOT%src\vr\vr_math.cpp"
set SRC=%SRC% "%ROOT%src\vr\vr_session.cpp"
set SRC=%SRC% "%ROOT%src\vr\vr_submit.cpp"
set SRC=%SRC% "%ROOT%src\vr\tune.cpp"
set SRC=%SRC% "%ROOT%src\vr\hook.cpp"
set SRC=%SRC% "%ROOT%src\vr\cull.cpp"
set SRC=%SRC% "%ROOT%src\vr\camera_head.cpp"
set SRC=%SRC% "%ROOT%src\vr\stereo.cpp"
set SRC=%SRC% "%ROOT%src\vr\ui_space.cpp"
set SRC=%SRC% "%ROOT%src\vr\vr_input.cpp"
set SRC=%SRC% "%ROOT%src\vr\vr_gesture.cpp"
set SRC=%SRC% "%ROOT%src\vr\water_stereo.cpp"
set SRC=%SRC% "%ROOT%src\vr\input_labels.cpp"
set SRC=%SRC% "%ROOT%src\vr\hud_capture.cpp"
set SRC=%SRC% "%ROOT%src\vr\perf_cpu.cpp"

rem OpenVR headers only. openvr_api.dll is loaded at runtime rather than
rem linked, so a missing or broken VR runtime cannot stop the game starting.
set "OPENVR=%ROOT%third_party\openvr\headers"
rem The VR interop header lives in our DXVK fork and pulls in Vulkan.
set "DXVKVR=%ROOT%dxvk-trlvr\src\d3d9"
set "VKINC="
for /d %%D in ("C:\VulkanSDK\*") do set "VKINC=%%D\Include"

set CFLAGS=/nologo /c /EHsc /MT /O2 /W4 /std:c++17 /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /I"%OPENVR%" /I"%DXVKVR%" /I"%VKINC%" /Fo"%OUT%\\"
set LFLAGS=/nologo /DLL /MACHINE:X86 /DEF:"%ROOT%src\proxy\d3d9.def" /OUT:"%OUT%\d3d9.dll"

echo Compiling...
del /q "%OUT%\*.obj" 2>nul
cl %CFLAGS% %SRC%
if errorlevel 1 (
    echo BUILD FAILED ^(compile^)
    exit /b 1
)

echo Linking...
rem Deliberately NOT linking d3d9.lib: this DLL *is* d3d9.dll, and an import
rem library would make it import from itself. Every entry point is resolved
rem from the backend with GetProcAddress instead. dxguid.lib supplies the
rem interface IIDs only, and pulls in no DLL.
link %LFLAGS% "%OUT%\*.obj" dxguid.lib user32.lib kernel32.lib
if errorlevel 1 (
    echo BUILD FAILED ^(link^)
    exit /b 1
)

copy /y "%OUT%\d3d9.dll" "%BIN%\d3d9.dll" >nul

rem The XInput proxy (src\xinput) is no longer built. The controllers drive
rem the keyboard and mouse from the d3d9 side instead -- the game polls
rem XInput once at startup and never again, and a thumbstick caps how fast
rem aim can turn. See src\vr\vr_input.cpp. The source is kept, unbuilt.

echo Building the launcher...
rem Its own exe, deliberately: the large-address-aware bit is read by the
rem loader from the file before any code in the process runs, so nothing the
rem mod does from inside the game can set it.
cl /nologo /EHsc /MT /O2 /W4 /std:c++17 /DWIN32 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%OUT%\launcher_" /Fe"%OUT%\trlvr_launcher.exe" ^
   "%ROOT%src\launcher\main.cpp" ^
   /link /MACHINE:X86 /SUBSYSTEM:WINDOWS user32.lib kernel32.lib shell32.lib
if errorlevel 1 (
    echo BUILD FAILED ^(launcher^)
    exit /b 1
)
copy /y "%OUT%\trlvr_launcher.exe" "%BIN%\trlvr_launcher.exe" >nul

echo.
echo Built: %BIN%\d3d9.dll
echo        %BIN%\trlvr_launcher.exe
echo Use install.bat to put it in the game folder.
exit /b 0
