@echo off
rem Keep this file ASCII-only: cmd.exe decodes .cmd with the OEM codepage.
rem Release build in a separate directory so the Debug build stays untouched.
rem vcvars output must NOT be redirected - it silently kills the env init.
rem Same header rule as build.cmd: localized /showIncludes breaks header deps,
rem so wipe and rebuild when any header is newer than the build graph.
if exist build-release\build.ninja (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$n=(Get-Item 'build-release\build.ninja').LastWriteTimeUtc; $h=Get-ChildItem -Path core,platform,app,tools,tests -Recurse -Filter *.h -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTimeUtc -gt $n } | Select-Object -First 1; if ($h) { exit 1 } else { exit 0 }"
  if errorlevel 1 rmdir /s /q build-release
)

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
