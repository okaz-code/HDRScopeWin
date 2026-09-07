@echo off
rem ???????????? tools\make-icon.cpp ???????????????
setlocal
cd /d "%~dp0\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist build\icon mkdir build\icon
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /Fo:build\icon\ /Fe:build\icon\make-icon.exe tools\make-icon.cpp || exit /b 1
build\icon\make-icon.exe Resources\AppIcon.ico || exit /b 1
endlocal
