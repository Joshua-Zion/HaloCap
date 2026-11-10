@echo off
rem Keep this file ASCII-only: cmd.exe decodes .cmd with the OEM codepage.
rem Release build in a separate directory so the Debug build stays untouched.
rem vcvars output must NOT be redirected - it silently kills the env init.

call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

set "TOOL=C:\Users\Joshua\tools\cmake-4.4.5-windows-x86_64\bin"
"%TOOL%\cmake.exe" -S . -B build-release -G Ninja -DCMAKE_MAKE_PROGRAM=C:\Users\Joshua\tools\ninja\ninja.exe -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 2
"%TOOL%\cmake.exe" --build build-release
if errorlevel 1 exit /b 3
"%TOOL%\ctest.exe" --test-dir build-release --output-on-failure
if errorlevel 1 exit /b 4
echo ---- release build + tests ok ----
