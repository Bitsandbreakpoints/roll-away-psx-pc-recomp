@echo off
setlocal
rem Build the game and level editor. An optional disc path can be supplied:
rem   build.bat --disc "D:\Games\Kula World.chd"
pushd "%~dp0"
call "%~dp0source\build.bat" %*
set "build_result=%errorlevel%"
popd
echo.
if not "%build_result%"=="0" (
    echo Build failed. See the messages above.
) else (
    echo Game:   "%~dp0source\build\roll_away.exe"
    echo Editor: "%~dp0source\build\roll_away_editor.exe"
)
rem Keep the output visible when launched by double-clicking in Explorer.
pause
exit /b %build_result%
