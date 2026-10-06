@echo off
setlocal
rem Builds the TRL VR DXVK fork as a 32-bit d3d9.dll.
rem
rem trl.exe is a 32-bit process, so this MUST be an x86 build. msvc.bat sets up
rem the x86 toolchain; DXVK builds fine under MSVC despite its stock cross files
rem naming mingw.
rem
rem   build_dxvk.bat          incremental
rem   build_dxvk.bat clean    wipe the build directory first
rem
rem Output is staged as dist\d3d9_dxvk.dll -- the name install.bat expects,
rem since the proxy itself has to be the one called d3d9.dll.

set "ROOT=%~dp0"
set "SRC=%ROOT%dxvk-trlvr"
set "BUILD=%SRC%\build-win32"
set "OUT=%ROOT%dist"

if not exist "%SRC%\meson.build" (
    echo ERROR: dxvk-trlvr not found at "%SRC%"
    exit /b 1
)

call "%ROOT%msvc.bat"
if errorlevel 1 exit /b 1

rem DXVK compiles its own helper shaders during the build.
where glslangValidator >nul 2>&1
if errorlevel 1 (
    where glslang >nul 2>&1
    if errorlevel 1 (
        echo ERROR: glslangValidator not found. Install the LunarG Vulkan SDK,
        echo or put glslang's bin directory on PATH.
        exit /b 1
    )
)

where meson >nul 2>&1
if errorlevel 1 (
    echo ERROR: meson not found. Install it with:  py -3 -m pip install meson ninja
    exit /b 1
)
where ninja >nul 2>&1
if errorlevel 1 (
    echo ERROR: ninja not found. Install it with:  py -3 -m pip install meson ninja
    exit /b 1
)

if /i "%~1"=="clean" (
    if exist "%BUILD%" (
        echo Removing "%BUILD%"
        rmdir /s /q "%BUILD%"
    )
)

if not exist "%BUILD%" (
    echo === meson setup ===
    rem Only d3d9 is wanted. d3d8 additionally fails to build here because it
    rem needs d3d8.h from the old DirectX SDK, which the Windows SDK does not
    rem ship; the rest are off to keep the build small.
    meson setup --backend ninja --buildtype release ^
        -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false ^
        "%BUILD%" "%SRC%"
    if errorlevel 1 goto :fail
)

rem Record only the PDB file name in the DLL, not the build machine's
rem full path (which carries the user name) -- privacy, 2026-10-05.
rem b_ndebug as upstream's package-release.sh: asserts off. Left on, every
rem assert in dxbc-spirv baked its absolute source path into the DLL.
meson configure "%BUILD%" -Db_ndebug=if-release -Dcpp_link_args=/PDBALTPATH:%%_PDB%% -Dc_link_args=/PDBALTPATH:%%_PDB%%
if errorlevel 1 goto :fail

echo.
echo === compile ===
meson compile -C "%BUILD%"
if errorlevel 1 goto :fail

if not exist "%OUT%" mkdir "%OUT%"
copy /y "%BUILD%\src\d3d9\d3d9.dll" "%OUT%\d3d9_dxvk.dll" >nul
if errorlevel 1 (
    echo ERROR: could not stage d3d9.dll into "%OUT%"
    goto :fail
)

echo.
echo === built ===
echo   %OUT%\d3d9_dxvk.dll
echo Run install.bat dxvk to put it beside the game.
exit /b 0

:fail
echo.
echo DXVK BUILD FAILED
exit /b 1
