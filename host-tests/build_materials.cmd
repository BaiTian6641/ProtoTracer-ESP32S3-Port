@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 /DESP32 /I material-shim /I shim /I ..\src materials.cpp /Fe:materials.exe /Fo:materials.obj
if errorlevel 1 exit /b 1
materials.exe
exit /b %errorlevel%
