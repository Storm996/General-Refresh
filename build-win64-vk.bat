@echo off
REM Canonical GeneralsX Windows build: Vulkan core only. No other Windows preset is supported.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\build\windows\build-win64-vk.ps1" %*
exit /b %errorlevel%
