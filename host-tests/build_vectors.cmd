@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cd /d "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /MT /utf-8 /DVECTOR2D_USE_DSP /DVECTOR3D_USE_DSP /I shim /I ..\src vector_dsp.cpp /Fe:vector_dsp.exe /Fo:vector_dsp.obj
if errorlevel 1 exit /b 1
vector_dsp.exe
exit /b %errorlevel%
