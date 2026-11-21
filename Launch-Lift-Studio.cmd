@echo off
setlocal
pushd "%~dp0pullup-viewer" || (
    echo Could not open the pullup-viewer folder.
    pause
    exit /b 1
)

if not exist ".venv\Scripts\python.exe" (
    echo Python setup is missing. See pullup-viewer\README.md for setup instructions.
    popd
    pause
    exit /b 1
)

".venv\Scripts\python.exe" "pullup_viewer.py" %*
set "lift_exit=%errorlevel%"
popd
if not "%lift_exit%"=="0" pause
exit /b %lift_exit%
