@echo off
setlocal
rem Builds and runs the OpenXR probe (src\test\xrprobe.cpp): a standalone
rem 32-bit exe that reports what the active OpenXR runtime offers. No game
rem and no D3D9 involved.

set "ROOT=%~dp0"
set "OUT=%ROOT%build\test"
if not exist "%OUT%" mkdir "%OUT%"

call "%ROOT%msvc.bat"
if errorlevel 1 exit /b 1

cl /nologo /EHsc /MT /O2 /W4 /std:c++17 /D_CRT_SECURE_NO_WARNINGS ^
   /I"%ROOT%third_party\openxr\include" /I"%ROOT%dxvk-trlvr\include\vulkan\include" ^
   /Fo"%OUT%\\" /Fe"%OUT%\xrprobe.exe" "%ROOT%src\test\xrprobe.cpp" ^
   /link /MACHINE:X86 "%ROOT%third_party\openxr\lib\win32\openxr_loader.lib" ^
   advapi32.lib kernel32.lib user32.lib pathcch.lib
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

rem Saved as well as shown: double-clicked, the window closes when this ends.
"%OUT%\xrprobe.exe" > "%OUT%\xrprobe.log" 2>&1
set RESULT=%ERRORLEVEL%
type "%OUT%\xrprobe.log"
echo.
echo Saved to %OUT%\xrprobe.log
pause
exit /b %RESULT%
