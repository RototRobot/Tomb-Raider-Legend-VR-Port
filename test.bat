@echo off
setlocal
rem Builds and runs the proxy load test in build\test, so the game folder is
rem never involved.

set "ROOT=%~dp0"
set "OUT=%ROOT%build\test"

if not exist "%ROOT%build\d3d9.dll" (
    echo ERROR: build\d3d9.dll not found. Run build.bat first.
    exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"

call "%ROOT%msvc.bat"
if errorlevel 1 exit /b 1

cl /nologo /EHsc /MT /O2 /W4 /std:c++17 /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%OUT%\\" /Fe"%OUT%\proxytest.exe" "%ROOT%src\test\proxytest.cpp" ^
   /link /MACHINE:X86 user32.lib dxguid.lib
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

copy /y "%ROOT%build\d3d9.dll" "%OUT%\d3d9.dll" >nul
if exist "%ROOT%dist\d3d9_dxvk.dll" copy /y "%ROOT%dist\d3d9_dxvk.dll" "%OUT%\d3d9_dxvk.dll" >nul
del /q "%OUT%\trlvr.log" 2>nul
del /q "%OUT%\trlvr.ini" 2>nul

rem Full path, not a bare name: the current directory is not on the executable
rem search path when NoDefaultCurrentDirectoryInExePath is set.
pushd "%OUT%"
"%OUT%\proxytest.exe"
set RESULT=%ERRORLEVEL%
popd

echo.
echo ---------------- trlvr.log ----------------
if exist "%OUT%\trlvr.log" (type "%OUT%\trlvr.log") else (echo ^(no log written^))
exit /b %RESULT%
