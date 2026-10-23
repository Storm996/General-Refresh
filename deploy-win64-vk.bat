@echo off
REM Canonical GeneralsX Windows deploy: win64vk Release exe + sidebar WND + x64 DXVK.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build\windows\deploy-win64-vk.ps1" %*
exit /b %errorlevel%
