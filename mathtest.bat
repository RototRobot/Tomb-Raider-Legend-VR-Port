@echo off
setlocal
rem Builds and runs the projection maths tests. No game, no device, no headset.
set "ROOT=%~dp0"
set "OUT=%ROOT%build\test"
if not exist "%OUT%" mkdir "%OUT%"
call "%ROOT%msvc.bat"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /MT /O2 /W4 /std:c++17 /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%OUT%\\" /Fe"%OUT%\mathtest.exe" ^
   "%ROOT%src\test\mathtest.cpp" "%ROOT%src\vr\vr_math.cpp" ^
   "%ROOT%src\vr\vr_gesture.cpp" ^
   "%ROOT%src\vr\physics\body_push.cpp" "%ROOT%src\vr\physics\body_volumes.cpp" ^
   "%ROOT%src\vr\godmode\god_carry.cpp" "%ROOT%src\vr\godmode\god_pluck.cpp" ^
   "%ROOT%src\vr\physics\soft_body.cpp" "%ROOT%src\vr\physics\rear_jiggle.cpp" ^
   "%ROOT%src\vr\physics\rear_shape.cpp" ^
   "%ROOT%src\vr\godmode\board_leash.cpp" "%ROOT%src\vr\menu_panel.cpp" ^
   "%ROOT%src\vr\finger_input.cpp" ^
   /link /MACHINE:X86
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
"%OUT%\mathtest.exe"
exit /b %ERRORLEVEL%
