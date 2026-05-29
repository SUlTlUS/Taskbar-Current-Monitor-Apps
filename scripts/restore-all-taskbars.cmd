@echo off
chcp 65001 >nul

echo [Taskbar Current Monitor Apps] Restore mode: all taskbars show all running window buttons.
echo.
echo This script writes:
echo   MMTaskbarEnabled = 1
echo   MMTaskbarMode    = 0
echo.
echo Please disable the Windhawk mod before running this script.
echo.

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" /v MMTaskbarEnabled /t REG_DWORD /d 1 /f
if errorlevel 1 goto failed

reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" /v MMTaskbarMode /t REG_DWORD /d 0 /f
if errorlevel 1 goto failed

echo.
echo Restarting explorer.exe...
taskkill /f /im explorer.exe >nul 2>nul
timeout /t 1 /nobreak >nul
start explorer.exe

echo.
echo Done. If the taskbar still doesn't refresh, sign out and sign in again.
pause
exit /b 0

:failed
echo.
echo Failed to write registry values. Try running this script again, or restart Windows and run it once more.
pause
exit /b 1
