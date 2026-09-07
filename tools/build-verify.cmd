@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set SDKINC=C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\cppwinrt
if not exist build\probe mkdir build\probe
cl /nologo /std:c++20 /EHsc /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I "%SDKINC%" /Fo:build\probe\ /Fe:build\probe\verify.exe tools\verify.cpp
