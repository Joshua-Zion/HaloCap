@echo off
rem Keep this file ASCII-only: cmd.exe decodes .cmd with the OEM codepage.
rem
rem vcvars output must NOT be redirected - it silently kills the env init.
rem
rem Header dependency tracking is unreliable here: MSVC prints /showIncludes in the
rem localized language, so ninja never records header deps and a header change does
rem not rebuild its dependents (this produced a stale vtable and an AV in S2).
rem Safe rule: if any header is newer than the build graph, wipe and rebuild.
if exist build\build.ninja (
  powershell -NoProfile -ExecutionPolicy Bypass -Command "$n=(Get-Item 'build\build.ninja').LastWriteTimeUtc; $h=Get-ChildItem -Path core,platform,app,tools,tests -Recurse -Filter *.h -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTimeUtc -gt $n } | Select-Object -First 1; if ($h) { exit 1 } else { exit 0 }"
  if errorlevel 1 rmdir /s /q build
)

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
