@echo off
rem ============================================================================
rem  host-tests\build_and_compare.cmd
rem
rem  A/B validation of the two render paths in src\Render\Camera.h:
rem    legacy.exe  -DDIRECT_RASTERIZER=0  (pixel-driven ray-cast + QuadTree)
rem    direct.exe  -DDIRECT_RASTERIZER=1  (triangle-driven bbox rasterizer)
rem
rem  Builds both from the REAL firmware headers (MSVC, C++17), renders one
rem  deterministic frame each, then compares the 2048x3-byte color buffers
rem  byte-for-byte. Exits 0 on byte-identical output, 1 on any mismatch or
rem  build/run failure.
rem
rem  Usage:  host-tests\build_and_compare.cmd
rem ============================================================================
setlocal EnableExtensions

set "ROOT=%~dp0.."
set "HT=%~dp0"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

if exist "%VCVARS%" goto vcvars_ok
echo ERROR: vcvars64.bat not found at:
echo   %VCVARS%
echo Adjust the VCVARS variable in this script if your BuildTools path differs.
exit /b 1
:vcvars_ok

if exist "%ROOT%\src\Render\Camera.h" goto camera_ok
echo ERROR: %ROOT%\src\Render\Camera.h not found - wrong repo root?
exit /b 1
:camera_ok

cd /d "%HT%"

rem Shim dir first so ProtoGC.h / esp_*.h resolve to the host shims.
rem (unquoted set: keeps the per-path quotes intact for cl)
set INCLUDES=-I"%HT%shim" -I"%ROOT%src" -I"%ROOT%src\Math" -I"%ROOT%src\Render" -I"%ROOT%src\Materials" -I"%ROOT%src\Flash" -I"%ROOT%src\Screenspace"
set "CLFLAGS=/nologo /std:c++17 /EHsc /O2 /MT /utf-8 /D_CRT_SECURE_NO_WARNINGS"

if not exist legacy_obj mkdir legacy_obj
if not exist direct_obj mkdir direct_obj

rem Generate a temp batch per build so vcvars + cl run in one clean cmd
rem context (avoids nested-quoting issues with the spaced BuildTools path).
rem NOTE: no parenthesized blocks here - the vcvars path contains "(x86)"
rem and a stray ")" would terminate a ( ... ) block early.
> "%HT%\build_legacy.bat" echo @echo off
>> "%HT%\build_legacy.bat" echo call "%VCVARS%" ^&^& cl %CLFLAGS% %INCLUDES% -DDIRECT_RASTERIZER=0 main.cpp /Fe:legacy.exe /Fo:legacy_obj\\ /link
> "%HT%\build_direct.bat" echo @echo off
>> "%HT%\build_direct.bat" echo call "%VCVARS%" ^&^& cl %CLFLAGS% %INCLUDES% -DDIRECT_RASTERIZER=1 main.cpp /Fe:direct.exe /Fo:direct_obj\\ /link

echo === [1/4] building legacy.exe (DIRECT_RASTERIZER=0) ===
call "%HT%\build_legacy.bat"
if errorlevel 1 ( echo BUILD FAILED: legacy.exe & exit /b 1 )

echo === [2/4] building direct.exe (DIRECT_RASTERIZER=1) ===
call "%HT%\build_direct.bat"
if errorlevel 1 ( echo BUILD FAILED: direct.exe & exit /b 1 )

echo === [3/4] rendering frames ===
"%HT%\legacy.exe" "%HT%\legacy.bin" || ( echo RUN FAILED: legacy.exe & exit /b 1 )
"%HT%\direct.exe" "%HT%\direct.bin" || ( echo RUN FAILED: direct.exe & exit /b 1 )

echo === [4/4] comparing byte-for-byte ===
powershell -NoProfile -ExecutionPolicy Bypass -File "%HT%compare.ps1" -LegacyBin "%HT%\legacy.bin" -DirectBin "%HT%\direct.bin"
set "RC=%errorlevel%"

del "%HT%\build_legacy.bat" "%HT%\build_direct.bat" 2>nul
exit /b %RC%
