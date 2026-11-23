@echo off
REM OwnKrn Windows Command Prompt Launcher
set ACTION=%1
if "%ACTION%"=="" set ACTION=build
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %ACTION%
exit /b %ERRORLEVEL%
