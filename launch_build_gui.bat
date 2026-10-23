@echo off
REM Launch the Apollo510 watchface build / flash / RTT GUI.
REM pythonw keeps the console window hidden; falls back to python if absent.
cd /d "%~dp0"
where pythonw >nul 2>&1
if %ERRORLEVEL%==0 (
    start "" pythonw "build_gui.py"
) else (
    python "build_gui.py"
)
