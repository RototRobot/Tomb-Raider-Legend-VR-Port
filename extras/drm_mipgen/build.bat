@echo off
rem Builds drm_mipgen.exe next to this script.
rem
rem 64-bit on purpose (msvc.bat sets up x86 for the game DLLs): decoding a
rem 4096x4096 texture to float RGBA takes 256 MB, and several worker threads
rem doing that at once doesn't fit in a 32-bit address space.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSTMP=%TEMP%\drm_mipgen_vspath.txt"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSTMP%"
set "VSPATH="
set /p VSPATH=<"%VSTMP%"
del /q "%VSTMP%" 2>nul
if not defined VSPATH (
    echo ERROR: no Visual Studio install with the C++ toolset.
    exit /b 1
)
rem See msvc.bat for why vcvarsall's output is discarded.
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul 2>&1
pushd "%~dp0"
cl /nologo /O2 /EHsc /std:c++17 /W3 /MT /D_CRT_SECURE_NO_WARNINGS drm_mipgen.cpp /Fe:drm_mipgen.exe
set ERR=%ERRORLEVEL%
del /q drm_mipgen.obj 2>nul
popd
exit /b %ERR%
