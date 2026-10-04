@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /MT media_codec.cpp /Fe:media_codec.exe /Fo:media_codec.obj
if errorlevel 1 exit /b 1
media_codec.exe
exit /b %errorlevel%
