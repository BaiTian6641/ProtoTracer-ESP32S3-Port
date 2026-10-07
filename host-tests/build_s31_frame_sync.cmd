@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cl /nologo /EHsc /std:c++17 /W4 /I"%~dp0s31-frame-shim" "%~dp0s31_frame_sync.cpp" /Fe:"%~dp0s31_frame_sync.exe" /Fo:"%~dp0s31_frame_sync.obj"
if errorlevel 1 exit /b 1
"%~dp0s31_frame_sync.exe"
exit /b %errorlevel%
