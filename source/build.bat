@echo off
rem Builds the Roll Away PC port: downloads the tools on first use, then
rem configure (reads the executables from your disc and recompiles them) + ninja.
rem   build.bat                          release build (build\roll_away.exe)
rem   build.bat --disc "D:\Games\Roll Away (USA).chd"   with your disc image
rem   build.bat --debug                  debug build
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo Python 3 is needed to build ^(https://www.python.org^).
  exit /b 1
)
if not exist "tools\llvm-mingw\bin" python tools\setup.py || exit /b 1
python configure.py %* || exit /b 1
"tools\ninja\ninja.exe" || exit /b 1
echo.
echo Built build\roll_away.exe
