@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Update-CodexTaskbar.ps1" %*
if errorlevel 1 (
  echo.
  echo Update failed. The existing CodexTaskbar.exe was not intentionally replaced.
  pause
)
endlocal
