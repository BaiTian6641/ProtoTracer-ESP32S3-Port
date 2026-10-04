@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
if not exist pack_obj mkdir pack_obj
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 /I"..\.pio\libdeps\esp32s3-PROFILE\ESP32 HUB75 LED MATRIX PANEL DMA Display\src" hub75_pack.cpp /Fe:hub75_pack.exe /Fo:pack_obj\
if errorlevel 1 exit /b 1
hub75_pack.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 /I"debug-shim" serial_debug.cpp /Fe:serial_debug.exe /Fo:pack_obj\
if errorlevel 1 exit /b 1
serial_debug.exe
exit /b %errorlevel%
