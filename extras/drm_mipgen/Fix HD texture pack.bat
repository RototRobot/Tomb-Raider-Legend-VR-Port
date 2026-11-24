@echo off
setlocal enabledelayedexpansion
rem Adds the missing mipmaps to the AI-Enhanced HD texture pack for Tomb
rem Raider: Legend (https://www.nexusmods.com/tombraiderlegend/mods/97), so
rem distant textures stop shimmering.
rem
rem   Drag the pack's folder onto this file, or double-click it and type
rem   (or paste) the folder when asked.
rem
rem The pack's own files are never changed: the converted copies go into a
rem new folder beside it, and can then be moved into the game's mods folder.
rem
rem !VAR! rather than %VAR% inside every parenthesised block: paths such as
rem "Program Files (x86)" end a block at parse time otherwise.

title Fix HD texture pack (mipmaps)
set "HERE=%~dp0"
set "TOOL=!HERE!drm_mipgen.exe"
set "REF=!HERE!origtex.txt"

echo ================================================================
echo   Fix HD texture pack -- adds mipmaps
echo ================================================================
echo.
echo Some HD texture packs for Tomb Raider: Legend were saved without
echo mipmaps (the smaller copies of each texture the game uses at a
echo distance). Without them, far-away surfaces shimmer and sparkle,
echo which is much more noticeable in VR. This rebuilds them.
echo.

if not exist "!TOOL!" (
    echo ERROR: drm_mipgen.exe is missing. Keep this file in the same folder
    echo as drm_mipgen.exe and origtex.txt.
    goto :fail
)
if not exist "!REF!" (
    echo ERROR: origtex.txt is missing. Keep this file in the same folder as
    echo drm_mipgen.exe and origtex.txt.
    goto :fail
)

rem --- 1. the pack's folder ------------------------------------------------
set "SRC=%~1"
if not defined SRC (
    echo Which folder holds the texture pack's .drm files?
    echo   Tip: drag the folder from Explorer into this window, then press Enter.
    echo.
    set /p "SRC=Folder: "
)
set "SRC=!SRC:"=!"
if not defined SRC goto :fail
if "!SRC:~-1!"=="\" set "SRC=!SRC:~0,-1!"
if not exist "!SRC!\" (
    echo.
    echo ERROR: not a folder: !SRC!
    echo Unpack the downloaded archive first, then give its folder.
    goto :fail
)

set /a COUNT=0
for /f "delims=" %%F in ('dir /s /b /a-d "!SRC!\*.drm" 2^>nul') do set /a COUNT+=1
if !COUNT!==0 (
    echo.
    echo ERROR: no .drm files in !SRC! or its subfolders.
    echo Give the folder you unpacked the texture pack into.
    goto :fail
)

rem --- 2. where the result goes -------------------------------------------
for %%P in ("!SRC!") do (
    set "DEST=%%~dpP%%~nxP - mipmapped"
    set "DRIVE=%%~dP"
)

rem Sizes in GB through PowerShell: batch arithmetic stops at 2 GB.
for /f %%S in ('powershell -NoProfile -Command "[math]::Round((Get-ChildItem -LiteralPath $env:SRC -Recurse -Filter *.drm | Measure-Object Length -Sum).Sum / 1GB, 1)"') do set "SIZE=%%S"
for /f %%S in ('powershell -NoProfile -Command "[math]::Round((Get-PSDrive -Name $env:DRIVE.Substring(0,1)).Free / 1GB, 1)"') do set "FREE=%%S"

echo.
echo Found !COUNT! .drm files, !SIZE! GB, in:
echo   !SRC!
echo.

rem --- 3. size cap --------------------------------------------------------
echo Texture size:
echo   1 = cap at 2048 pixels  (recommended for VR: the game is 32-bit and
echo       can run out of memory with full 4K textures)
echo   2 = keep full size      (4K where the pack has it)
echo.
choice /c 12 /n /m "Choose 1 or 2: "
if errorlevel 2 (
    set "MAX="
    set "CAPNAME=full size"
    set "NEED_FACTOR=1.4"
) else (
    set "MAX=--max 2048"
    set "CAPNAME=2048 cap"
    rem Generous: much of a pack is already 2048 or smaller, and the
    rem added mip chains grow those by a third.
    set "NEED_FACTOR=1.0"
)

for /f %%S in ('powershell -NoProfile -Command "[math]::Ceiling(%SIZE% * %NEED_FACTOR%)"') do set "NEED=%%S"
echo.
echo The result goes into:
echo   !DEST!
echo It needs about !NEED! GB; !FREE! GB is free on !DRIVE!
for /f %%R in ('powershell -NoProfile -Command "if (%FREE% -lt %NEED%) { 'low' } else { 'ok' }"') do set "ROOM=%%R"
if "!ROOM!"=="low" (
    echo.
    echo WARNING: that may not be enough space. Free some up, or move the
    echo pack to a drive with more room, then run this again.
    choice /c YN /n /m "Try anyway? [Y/N] "
    if errorlevel 2 goto :fail
)
echo.
echo This can take a long time -- from several minutes to over an hour,
echo depending on your PC. If it is stopped, run it again with the same
echo folder: files already done are skipped.
echo.
choice /c YN /n /m "Start now? [Y/N] "
if errorlevel 2 goto :fail

rem --- 4. convert ---------------------------------------------------------
echo.
"!TOOL!" --out "!DEST!" !MAX! --ref "!REF!" "!SRC!"
set "ERR=!ERRORLEVEL!"
echo.
if not "!ERR!"=="0" (
    echo Some files could not be converted ^(see the messages above^).
    echo The rest are in !DEST!
    echo.
)

rem --- 5. into the game ---------------------------------------------------
echo Done (!CAPNAME!). To use the textures, the files must go into the
echo game's "mods" folder, and TR7-Menu-Hook must be installed (it is what
echo loads that folder).
echo.
choice /c YN /n /m "Move them into the game's mods folder now? [Y/N] "
if errorlevel 2 goto :manual

set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Tomb Raider Legend"
if not exist "!GAME!\trl.exe" (
    echo.
    echo The game is not in Steam's default folder. Which folder holds trl.exe?
    set /p "GAME=Game folder: "
    set "GAME=!GAME:"=!"
    if "!GAME:~-1!"=="\" set "GAME=!GAME:~0,-1!"
)
if not exist "!GAME!\trl.exe" (
    echo.
    echo trl.exe is not in that folder.
    goto :manual
)
if not exist "!GAME!\TR7-Menu-Hook.asi" (
    echo.
    echo NOTE: TR7-Menu-Hook.asi is not in the game folder. Install
    echo TR7-Menu-Hook too, or the game will ignore the mods folder.
)
set /a EXISTING=0
if exist "!GAME!\mods\" for /f "delims=" %%F in ('dir /b /a-d "!GAME!\mods\*.drm" 2^>nul') do set /a EXISTING+=1
if !EXISTING! gtr 0 (
    echo.
    echo The mods folder already has !EXISTING! .drm files. Files with the same
    echo names will be replaced by the converted ones.
    choice /c YN /n /m "Go ahead? [Y/N] "
    if errorlevel 2 goto :manual
)
echo.
echo Moving...
robocopy "!DEST!" "!GAME!\mods" *.drm /MOV /NJH /NJS /NDL /NFL /NP >nul
if errorlevel 8 (
    echo ERROR: moving failed ^(the game folder may need administrator
    echo rights^). Move the files from !DEST! into !GAME!\mods yourself.
    goto :fail
)
rd "!DEST!" 2>nul
echo Done: the textures are in !GAME!\mods
echo.
pause
exit /b 0

:manual
echo.
echo Move every file from
echo   !DEST!
echo into the "mods" folder inside the game folder (make it if missing).
echo.
pause
exit /b 0

:fail
echo.
pause
exit /b 1
