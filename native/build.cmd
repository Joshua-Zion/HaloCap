@echo off
rem vcvars output must NOT be redirected - it silently kills the env init.
rem See "S1 toolchain" in docs/2.0-native-plan.md. Keep this file ASCII-only:
rem cmd.exe decodes .cmd with the OEM codepage, so UTF-8 comments break parsing.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

set "TOOL=C:\Users\Joshua\tools\cmake-4.4.5-windows-x86_64\bin"
"%TOOL%\cmake.exe" -S . -B build -G Ninja -DCMAKE_MAKE_PROGRAM=C:\Users\Joshua\tools\ninja\ninja.exe
if errorlevel 1 exit /b 2
"%TOOL%\cmake.exe" --build build
if errorlevel 1 exit /b 3
"%TOOL%\ctest.exe" --test-dir build --output-on-failure
if errorlevel 1 exit /b 4
echo ---- native build + tests ok ----
