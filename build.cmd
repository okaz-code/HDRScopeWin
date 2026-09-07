@echo off
setlocal
cd /d "%~dp0"

rem Configures once and builds. Everything lands in build\dist\HDRScope.exe.
rem Override the generator with HDRSCOPE_GENERATOR if a different toolset is wanted.
if "%HDRSCOPE_GENERATOR%"=="" set HDRSCOPE_GENERATOR=Visual Studio 17 2022

if not exist build\CMakeCache.txt (
  cmake -S . -B build -G "%HDRSCOPE_GENERATOR%" -A x64 || exit /b 1
)
cmake --build build --config Release || exit /b 1

echo.
echo Built: %CD%\build\dist\HDRScope.exe

if "%1"=="--test" (
  echo.
  build\dist\HDRScope.exe --self-test --test-output build\test-results || exit /b 1
  where python >nul 2>&1 && python Tests\verify_tiff.py build\test-results\roundtrip.tiff
)
endlocal
