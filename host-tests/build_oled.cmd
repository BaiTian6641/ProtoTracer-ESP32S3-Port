@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0.."
if not exist build\registration-check mkdir build\registration-check
cl /nologo /std:c++17 /EHsc /O2 /MT /I"host-tests\display-shim" /I".pio\libdeps\esp32s3-PROFILE\M5GFX\src" /I".pio\libdeps\esp32s3-PROFILE\M5GFX\src\lgfx\utility" host-tests\oled_registration.cpp .pio\libdeps\esp32s3-PROFILE\M5GFX\src\lgfx\utility\lgfx_qrcode.c /Fe:build\registration-check\oled_registration.exe /Fo:build\registration-check\
if errorlevel 1 exit /b 1
build\registration-check\oled_registration.exe
exit /b %errorlevel%
