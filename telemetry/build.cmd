@echo off
call "%~dp0..\build.cmd" telemetry test-telemetry %*
exit /b %errorlevel%
