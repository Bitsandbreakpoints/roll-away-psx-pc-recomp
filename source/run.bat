@echo off
rem Builds the port (build.bat) and starts it.
rem   run.bat [--disc "D:\Games\Roll Away (USA).chd"] [--debug] [--no-run]
cd /d "%~dp0"
set RUN=1
set ARGS=
:args
if "%~1"=="" goto build
if "%~1"=="--no-run" (set RUN=0) else set ARGS=%ARGS% %1
shift
goto args
:build
call "%~dp0build.bat" %ARGS% || exit /b 1
if "%RUN%"=="1" start "" "build\roll_away.exe"
